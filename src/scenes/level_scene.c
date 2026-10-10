/*
 * Level scene (see level_scene.h). SDL-tier: loads the demo map, owns a
 * Camera3D + DrawList + billboards and a small UI root (the achievement
 * toast plus the icon-only ROT L / ROT R / DEBUG / RESET controls, built and
 * laid out by the pure level_controls module), and per frame builds/draws
 * the world list, then handles the world commands from the InputFrame.
 *
 * Event wiring:
 * - Every successful camera rotation step publishes EV_GAMEPLAY_CAMERA_TURNED
 *   (topic EV_TOPIC_GAMEPLAY) and an EV_AUDIO_PLAY request for the rotate
 *   sound.
 * - The scene subscribes to EV_TOPIC_ACHIEVEMENT; an EV_ACHIEVEMENT_UNLOCKED
 *   payload shows the toast with the achievement title. The subscription is
 *   removed in unload.
 *
 * The on-screen controls are children of the UI root; a tap routes through
 * uiBridgeDispatch to a button callback, which calls the same helpers as the
 * keyboard command handler (no duplicated rotate/reset logic).
 */

#include "scenes/level_scene.h"

#include "achievement.h"
#include "ai/fsm.h"
#include "ai/nav.h"
#include "ai/pathfind.h"
#include "app.h"
#include "audio/audio.h"
#include "entities/entities.h"
#include "events.h"
#include "input/input.h"
#include "platform/platform.h"
#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/frame.h"
#include "render/gpu_backend.h"
#include "render/grid.h"
#include "render/lightgrid.h"
#include "render/map_loader.h"
#include "render/materials.h"
#include "render/math3d.h"
#include "render/sprites.h"
#include "render/textures.h"
#include "render/voxmap.h"
#include "scenes/level_controls.h"
#include "scenes/pause_scene.h"
#include "scenes/ui_bridge.h"
#include "sim/machines.h"
#include "ui/element.h"
#include "ui/layout.h"
#include "ui/toast.h"
#include "ui/ui_font.h"

#include <SDL3/SDL.h>

#define LEVEL_DRAWLIST_CAPACITY 4096
#define LEVEL_SPRITE_MAX 32
#define LEVEL_WALKER_COUNT 3
/* Most T20 debug markers held per frame (the demo paths are a handful of
 * tiles; the derivation caps safely if this is ever exceeded). */
#define LEVEL_DEBUG_MARKER_MAX 256

#define LEVEL_TOAST_W 300
#define LEVEL_TOAST_H 48
#define LEVEL_TOAST_TOP 20		/* virtual px from the top edge */

/* T21 demo: FSM + A* driven critters. Each walker cycles a two-destination
 * list; a leg's route is computed with A* (entityPathTo) and the executor FSM
 * drives IDLE -> MOVING_TO -> (ARRIVED) ACTING -> (PHASE_DONE) WAITING ->
 * (PHASE_DONE) DONE -> ..., so every state/action is exercised. Destinations
 * are open plateau tiles:
 *   A paces the tower row z=8 (x2<->x12): the tower (x6-7, z7-8) blocks the
 *     straight line, so A* routes 20 tiles around the south edge (straight-line
 *     10) — the visible detour;
 *   B walks the x=10 lane (z3<->z13): a straight 10-tile run through the gap
 *     between the tower and the room (contrast, no detour);
 *   C crosses west (2,7) to east (11,10): the room (x4-8, z9-12) blocks it, so
 *     the route runs 18 tiles around the south (straight-line 12). */
static const int DEMO_DESTS_A[2][2] = { { 12, 8 }, { 2, 8 } };
static const int DEMO_DESTS_B[2][2] = { { 10, 13 }, { 10, 3 } };
static const int DEMO_DESTS_C[2][2] = { { 11, 10 }, { 2, 7 } };

/* The WAIT reason the demo's timed machine phase carries, and its bound. */
#define DEMO_WAIT_CYCLE 1
#define DEMO_WAIT_SECONDS 0.4f

/* --- T22 machines ------------------------------------------------------- *
 *
 * Five machines in a "laundromat corner" row on the north grass (z=1, x1..5),
 * clear of the room (x4-8,z9-12), the tower (x6-7,z3-6) and every critter
 * lane (A paces z=8, B the x=10 column z3-13, C detours south of the room).
 * The row sits west of the tower and two grass rows north of the z=3 stone, so
 * all five billboards read unobstructed. Each renders as a 1.2x1.4 billboard
 * through the sprite path, tinted per kind (washers cool, dryers warm); the
 * tile stays walkable so a critter can step onto it to interact (v1
 * convention, sim/machines.h). */
#define LEVEL_MACHINE_COUNT 5
#define LEVEL_MACHINE_W 1.2f
#define LEVEL_MACHINE_H 1.4f
#define LEVEL_MACHINE_RUN_SECS 30.0f
#define LEVEL_MACHINE_WAKE_MAX 8
#define LEVEL_MACHINE_TINT_WASHER DRAW_TINT(150, 200, 255, 255)	/* cool */
#define LEVEL_MACHINE_TINT_DRYER DRAW_TINT(255, 190, 130, 255)	/* warm */

static const struct {
	int kind;
	int x;
	int z;
	int fee;
} LEVEL_MACHINE_SPEC[LEVEL_MACHINE_COUNT] = {
	{ MACHINE_KIND_WASHER, 1, 1, 3 },
	{ MACHINE_KIND_WASHER, 2, 1, 3 },
	{ MACHINE_KIND_WASHER, 3, 1, 3 },
	{ MACHINE_KIND_DRYER, 4, 1, 2 },
	{ MACHINE_KIND_DRYER, 5, 1, 2 },
};

/* A demo critter: its entity, its executor FSM and a two-destination loop.
 * waitLeft counts down the FSM's WAITING phase (the simulated machine cycle;
 * the demo never lets it reach a timeout). */
typedef struct DemoWalker {
	EntityHandle handle;
	Fsm fsm;
	const int (*dests)[2];
	int destCount;
	int destIndex;
	float waitLeft;
} DemoWalker;

/* --- TEMPORARY T22 SMOKE DRIVER (replaced wholesale by the T23 brain) ----
 *
 * Makes the machine layer visible before the brain exists: one critter walks
 * onto washer 0's tile, claims it, LOADs 5 s, starts the 30 s RUN, walks home,
 * walks back on the PHASE_DONE wake, COLLECTs 5 s and releases, looping; a
 * second critter repeatedly walks up mid-run and has its claim refused (the
 * machine stays RUNNING). Delete this struct plus every level-smoke helper
 * and smoke field in T23. */
#define LEVEL_SMOKE_COUNT 2
#define LEVEL_SMOKE_TARGET 0		/* washer at (1,1) */
#define LEVEL_SMOKE_LOAD_SECS 5.0f
#define LEVEL_SMOKE_COLLECT_SECS 5.0f

typedef enum SmokePhase {
	SMOKE_IDLE = 0,
	SMOKE_WALK_IN,
	SMOKE_LOAD,
	SMOKE_RUN_AWAY,
	SMOKE_RUN_WAIT,
	SMOKE_WALK_BACK,
	SMOKE_COLLECT,
	SMOKE_RETURN,
} SmokePhase;

typedef struct SmokeDriver {
	EntityHandle handle;
	int role;			/* 0 = full cycle, 1 = challenger */
	MachineHandle target;
	int homeX;
	int homeZ;
	int phase;			/* SmokePhase */
	float timer;
	bool kicked;			/* saw the machine's PHASE_DONE */
} SmokeDriver;

/* The four controls (ROT L / ROT R / DEBUG / RESET) and their layout rule
 * live in the pure level_controls module so they are headless-testable;
 * LEVEL_BUTTON_W/H/MARGIN/GAP come from there. */

typedef struct LevelState {
	Voxmap *map;
	LightGrid *lights;	/* baked from the map's emitters + sky */
	Camera3D camera;
	DrawList list;
	SpriteEntity sprites[LEVEL_SPRITE_MAX];
	size_t spriteCount;
	EntityRegistry entities;	/* the demo critters (T19) */
	DemoWalker walkers[LEVEL_WALKER_COUNT];
	int walkerCount;
	MachineRegistry machines;	/* T22 interactables */
	int16_t spriteMaterial;		/* atlas material for the critters */
	int16_t machineMaterial;	/* atlas material for the machines */
	MachineWake wakes[LEVEL_MACHINE_WAKE_MAX];	/* per-frame machine wakes */
	SmokeDriver smoke[LEVEL_SMOKE_COUNT];	/* TEMPORARY T22 smoke driver */
	int smokeCount;
	EntityDebugMarker markers[LEVEL_DEBUG_MARKER_MAX];	/* T20 overlay */
	const MaterialTable *materials;	/* borrowed from the GPU backend */

	EventBus *bus;		/* borrowed from the App; may be NULL */
	int rotationSteps;	/* running count of applied 45-degree steps */
	bool smoothLight;	/* T15: per-corner smooth lighting + AO (T toggles) */
	bool lightDebug;	/* T15: light-only debug view (F toggles) */
	Element *root;		/* UI root (a transparent container) */
	Element *toast;		/* achievement toast */
	LevelControls controls;	/* rotate x2 / debug / reset icon buttons */
} LevelState;

/* Create the demo critters (T19/T21): three registry entities placed on open
 * plateau, each driving an FSM + A* loop (see the DEMO_DESTS_* note). The
 * first leg is started in levelUpdateWalkers on the first update. */
static void levelBuildEntities(LevelState *st, int16_t spriteMaterial)
{
	static const struct {
		int startX;
		int startZ;
		const int (*dests)[2];
		int destCount;
		float speed;
		EasingFn easing;
		float width;
		float height;
	} demo[LEVEL_WALKER_COUNT] = {
		{ 2, 8, DEMO_DESTS_A, 2, 3.0f, EASE_IN_OUT, 1.2f, 1.8f },
		{ 10, 3, DEMO_DESTS_B, 2, 2.5f, EASE_LINEAR, 1.2f, 1.8f },
		{ 2, 7, DEMO_DESTS_C, 2, 3.5f, EASE_OUT, 1.5f, 2.2f },
	};
	int i;

	entitiesInit(&st->entities, st->map);
	st->walkerCount = 0;
	for (i = 0; i < LEVEL_WALKER_COUNT; i++) {
		EntityHandle h = entityCreate(&st->entities);
		Entity *e = entityGet(&st->entities, h);
		DemoWalker *w;

		if (e == NULL ||
		    !entityPlace(e, demo[i].startX, demo[i].startZ)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: demo entity %d placement failed",
				     i);
			continue;
		}
		e->speed = demo[i].speed;
		e->easing = demo[i].easing;
		e->animated = true;
		entitySetSprite(e, demo[i].width, demo[i].height,
				DRAW_TINT(255, 255, 255, 255), spriteMaterial);

		w = &st->walkers[st->walkerCount];
		w->handle = h;
		fsmInit(&w->fsm);
		w->dests = demo[i].dests;
		w->destCount = demo[i].destCount;
		w->destIndex = 0;
		w->waitLeft = 0.0f;
		w->fsm.waitReason = DEMO_WAIT_CYCLE;
		w->fsm.waitSeconds = DEMO_WAIT_SECONDS;
		st->walkerCount++;
		SDL_Log("isomata: demo entity %d created at (%d,%d)",
			h, demo[i].startX, demo[i].startZ);
	}
}

/* Apply one FSM event and log the transition (opt-in ISO_LOG=debug), so the
 * log shows the FSM actually driving the critters. */
static FsmResult demoFsmStep(DemoWalker *w, int event)
{
	int before = w->fsm.state;
	FsmResult r = fsmStep(&w->fsm, event);

	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		     "isomata: entity %d fsm %s --%s--> %s (action %s)",
		     w->handle, fsmStateName(before), fsmEventName(event),
		     fsmStateName(r.state), fsmActionName(r.action));
	return r;
}

/* Begin the walker's next leg: drive the FSM START (MOVE_TO the target), then
 * A* the route with entityPathTo and queue it; log the recomputed path length
 * against the straight-line distance so a detour leg is provable. A rejected
 * route fails the task (DENIED -> FINISH_GOAL) and the next leg is tried on
 * the following update. */
static void demoStartLeg(LevelState *st, DemoWalker *w)
{
	const int *d = w->dests[w->destIndex];
	Entity *e = entityGet(&st->entities, w->handle);
	int straight;
	int len;

	if (e == NULL)
		return;
	straight = pathfindManhattan(e->tileX, e->tileZ, d[0], d[1]);
	/* FSM decision first: START carries the target and emits MOVE_TO. */
	fsmSetGoal(&w->fsm, d[0], d[1]);
	demoFsmStep(w, FSM_EVENT_START);

	len = entityPathTo(e, d[0], d[1]);
	if (len < 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: entity %d path to (%d,%d) rejected",
			     w->handle, d[0], d[1]);
		demoFsmStep(w, FSM_EVENT_DENIED);
		w->destIndex = (w->destIndex + 1) % w->destCount;
		return;
	}
	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		     "isomata: entity %d: task path (len %d) to (%d,%d) straight %d%s",
		     w->handle, len, d[0], d[1], straight,
		     len > straight ? " DETOUR" : "");
	w->destIndex = (w->destIndex + 1) % w->destCount;
}

/* Advance each critter's executor one step: on arrival run the activity step
 * and hand off to the timed WAIT, count the WAIT down, then begin the next
 * leg. The FSM models the decisions; this is the sim glue that executes
 * MOVE_TO (entityPathTo) and WAIT (waitLeft). */
static void levelUpdateWalkers(LevelState *st, float dt)
{
	int i;

	for (i = 0; i < st->walkerCount; i++) {
		DemoWalker *w = &st->walkers[i];
		Entity *e = entityGet(&st->entities, w->handle);
		FsmResult r;

		if (e == NULL)
			continue;
		switch (w->fsm.state) {
		case FSM_STATE_MOVING_TO:
			/* Arrival = the whole route is walked: no active
			 * segment and an empty queue. */
			if (!e->moving && e->pathCount == 0) {
				demoFsmStep(w, FSM_EVENT_ARRIVED);
				/* The activity step completes at once,
				 * handing off to the timed phase. */
				r = demoFsmStep(w, FSM_EVENT_PHASE_DONE);
				w->waitLeft = r.waitSeconds;
			}
			break;
		case FSM_STATE_WAITING:
			w->waitLeft -= dt;
			if (w->waitLeft <= 0.0f)
				demoFsmStep(w, FSM_EVENT_PHASE_DONE);
			break;
		case FSM_STATE_IDLE:
		case FSM_STATE_DONE:
			demoStartLeg(st, w);
			break;
		default:
			break;
		}
	}
}

/* Derive the billboard array from the registry each frame (position + sprite
 * link). Order is registry slot order, which is stable. */
/* ---- T22 machines + the temporary smoke driver ------------------------ */

/* The pinned tint for a machine kind; an unknown kind reads as a washer. */
static uint32_t levelMachineTint(int kind)
{
	return kind == MACHINE_KIND_DRYER ? LEVEL_MACHINE_TINT_DRYER
					  : LEVEL_MACHINE_TINT_WASHER;
}

/* Milliseconds since SDL init, prefixed on every smoke line as a timestamp. */
static unsigned long long levelMs(void)
{
	return (unsigned long long)SDL_GetTicks();
}

/* One machine-state-change log line (opt-in ISO_LOG=debug), naming the kind,
 * the state and the owner so the claim/run/release cycle is readable. */
static void levelLogMachine(const LevelState *st, MachineHandle h,
			    const char *what)
{
	const Machine *m = machineGetConst(&st->machines, h);

	if (m == NULL)
		return;
	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		     "isomata: [t=%llums] machine %d %s -> kind %s state %s owner %d runLeft %.2f",
		     levelMs(), h, what, machineKindName(m->kind),
		     machineStateName(m->state), m->owner, (double)m->runLeft);
}

/* Register the five machines (T22 layer-1 objects). */
static void levelBuildMachines(LevelState *st)
{
	int i;

	machinesInit(&st->machines);
	for (i = 0; i < LEVEL_MACHINE_COUNT; i++) {
		MachineHandle h = machineCreate(
			&st->machines, LEVEL_MACHINE_SPEC[i].kind,
			LEVEL_MACHINE_SPEC[i].x, LEVEL_MACHINE_SPEC[i].z,
			LEVEL_MACHINE_SPEC[i].fee, LEVEL_MACHINE_RUN_SECS);

		if (h == MACHINE_INVALID) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: machine %d create failed", i);
			continue;
		}
		SDL_Log("isomata: machine %d %s at (%d,%d) fee %d run %.0fs",
			h, machineKindName(LEVEL_MACHINE_SPEC[i].kind),
			LEVEL_MACHINE_SPEC[i].x, LEVEL_MACHINE_SPEC[i].z,
			LEVEL_MACHINE_SPEC[i].fee,
			(double)LEVEL_MACHINE_RUN_SECS);
	}
}

/* Create the two smoke critters: the primary (full cycle) and the challenger.
 * Tinted apart from the T21 demo walkers so the screenshot reads. */
static void levelBuildSmoke(LevelState *st)
{
	static const struct {
		int x;
		int z;
		int role;
		int homeX;
		int homeZ;
		uint32_t tint;
		float speed;
	} spec[LEVEL_SMOKE_COUNT] = {
		{ 1, 3, 0, 1, 3, DRAW_TINT(140, 255, 140, 255), 2.2f },
		{ 1, 0, 1, 1, 0, DRAW_TINT(255, 150, 150, 255), 2.8f },
	};
	int i;

	st->smokeCount = 0;
	for (i = 0; i < LEVEL_SMOKE_COUNT; i++) {
		EntityHandle h = entityCreate(&st->entities);
		Entity *e = entityGet(&st->entities, h);
		SmokeDriver *d;

		if (e == NULL || !entityPlace(e, spec[i].x, spec[i].z)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: smoke entity %d placement failed",
				     i);
			continue;
		}
		e->speed = spec[i].speed;
		e->easing = EASE_IN_OUT;
		e->animated = true;
		entitySetSprite(e, 1.0f, 1.5f, spec[i].tint,
				st->spriteMaterial);

		d = &st->smoke[st->smokeCount];
		d->handle = h;
		d->role = spec[i].role;
		d->target = LEVEL_SMOKE_TARGET;
		d->homeX = spec[i].homeX;
		d->homeZ = spec[i].homeZ;
		d->phase = SMOKE_IDLE;
		d->timer = 0.0f;
		d->kicked = false;
		st->smokeCount++;
		SDL_Log("isomata: smoke critter %d (%s) at (%d,%d)",
			h, spec[i].role == 0 ? "primary" : "challenger",
			spec[i].x, spec[i].z);
	}
}

/* True when the entity has finished its whole route (no active segment, empty
 * queue). */
static bool levelArrived(const Entity *e)
{
	return !e->moving && e->pathCount == 0;
}

static void levelSmokePathTo(LevelState *st, SmokeDriver *d, int x, int z)
{
	Entity *e = entityGet(&st->entities, d->handle);

	if (e != NULL)
		(void)entityPathTo(e, x, z);
}

static void levelSmokeSendHome(LevelState *st, SmokeDriver *d)
{
	levelSmokePathTo(st, d, d->homeX, d->homeZ);
}

static void levelSmokeSendToMachine(LevelState *st, SmokeDriver *d)
{
	const Machine *m = machineGetConst(&st->machines, d->target);

	if (m != NULL)
		levelSmokePathTo(st, d, m->tileX, m->tileZ);
}
/* The primary's fixed scripted cycle against washer 0. */
static void levelSmokePrimary(LevelState *st, SmokeDriver *d, float dt)
{
	Entity *e = entityGet(&st->entities, d->handle);

	if (e == NULL)
		return;
	switch (d->phase) {
	case SMOKE_IDLE:
		levelSmokeSendToMachine(st, d);
		d->phase = SMOKE_WALK_IN;
		break;
	case SMOKE_WALK_IN:
		if (!levelArrived(e))
			break;
		if (machineClaim(machineGet(&st->machines, d->target),
				 d->handle)) {
			levelLogMachine(st, d->target, "claimed");
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: [t=%llums] smoke primary: loading %.0fs in place",
				     levelMs(), (double)LEVEL_SMOKE_LOAD_SECS);
			d->phase = SMOKE_LOAD;
			d->timer = LEVEL_SMOKE_LOAD_SECS;
		} else {
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: [t=%llums] smoke primary claim DENIED",
				     levelMs());
			d->phase = SMOKE_RETURN;
			levelSmokeSendHome(st, d);
		}
		break;
	case SMOKE_LOAD:
		d->timer -= dt;
		if (d->timer <= 0.0f) {
			machineStartRun(machineGet(&st->machines, d->target));
			levelLogMachine(st, d->target, "run started");
			d->phase = SMOKE_RUN_AWAY;
			levelSmokeSendHome(st, d);
		}
		break;
	case SMOKE_RUN_AWAY:
		if (levelArrived(e))
			d->phase = SMOKE_RUN_WAIT;
		break;
	case SMOKE_RUN_WAIT:
		if (d->kicked) {
			d->kicked = false;
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: [t=%llums] smoke primary: phase done, walking back",
				     levelMs());
			d->phase = SMOKE_WALK_BACK;
			levelSmokeSendToMachine(st, d);
		}
		break;
	case SMOKE_WALK_BACK:
		if (levelArrived(e)) {
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: [t=%llums] smoke primary: collecting %.0fs in place",
				     levelMs(), (double)LEVEL_SMOKE_COLLECT_SECS);
			d->phase = SMOKE_COLLECT;
			d->timer = LEVEL_SMOKE_COLLECT_SECS;
		}
		break;
	case SMOKE_COLLECT:
		d->timer -= dt;
		if (d->timer <= 0.0f) {
			machineRelease(machineGet(&st->machines, d->target));
			levelLogMachine(st, d->target, "released");
			d->phase = SMOKE_RETURN;
			levelSmokeSendHome(st, d);
		}
		break;
	case SMOKE_RETURN:
		if (levelArrived(e))
			d->phase = SMOKE_IDLE;
		break;
	default:
		break;
	}
}

/* The challenger: only walks up while the machine is RUNNING, so its claim is
 * guaranteed to be refused — the exclusivity the T23 brain relies on. */
static void levelSmokeChallenger(LevelState *st, SmokeDriver *d)
{
	Entity *e = entityGet(&st->entities, d->handle);
	const Machine *m = machineGetConst(&st->machines, d->target);

	if (e == NULL)
		return;
	switch (d->phase) {
	case SMOKE_IDLE:
		if (m != NULL && m->state == MACHINE_STATE_RUNNING) {
			levelSmokeSendToMachine(st, d);
			d->phase = SMOKE_WALK_IN;
		}
		break;
	case SMOKE_WALK_IN:
		if (!levelArrived(e))
			break;
		{
			Machine *mm = machineGet(&st->machines, d->target);

			if (mm != NULL && machineClaim(mm, d->handle)) {
				SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
					     "isomata: [t=%llums] smoke challenger WON the claim (unexpected)",
					     levelMs());
				machineRelease(mm);
			} else {
				SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
					     "isomata: [t=%llums] smoke challenger claim DENIED by machine %d state %s owner %d",
					     levelMs(), d->target,
					     mm != NULL ? machineStateName(mm->state) : "?",
					     mm != NULL ? mm->owner : MACHINE_NO_OWNER);
			}
		}
		d->phase = SMOKE_RETURN;
		levelSmokeSendHome(st, d);
		break;
	case SMOKE_RETURN:
		if (levelArrived(e))
			d->phase = SMOKE_IDLE;
		break;
	default:
		break;
	}
}

/* Drain the frame's machine wakes into the driver (the PHASE_DONE wake is what
 * sends the primary back to collect), then advance both smoke critters. */
static void levelUpdateSmoke(LevelState *st, float dt,
			     const MachineWake *wakes, int nw)
{
	int i;

	for (i = 0; i < nw; i++) {
		int j;

		if (wakes[i].kind != MACHINE_WAKE_PHASE_DONE)
			continue;
		for (j = 0; j < st->smokeCount; j++) {
			SmokeDriver *d = &st->smoke[j];

			if (d->role == 0 && d->target == wakes[i].machine)
				d->kicked = true;
		}
	}
	for (i = 0; i < st->smokeCount; i++) {
		if (st->smoke[i].role == 0)
			levelSmokePrimary(st, &st->smoke[i], dt);
		else
			levelSmokeChallenger(st, &st->smoke[i]);
	}
}

static void levelSyncSprites(LevelState *st)
{
	EntityHandle h;
	size_t n = 0;

	for (h = entityFirst(&st->entities); h != ENTITY_INVALID;
	     h = entityNext(&st->entities, h)) {
		const Entity *e = entityGet(&st->entities, h);

		if (e == NULL || n >= LEVEL_SPRITE_MAX)
			break;
		st->sprites[n].x = e->x;
		st->sprites[n].y = e->y;
		st->sprites[n].z = e->z;
		st->sprites[n].width = e->width;
		st->sprites[n].height = e->height;
		st->sprites[n].tint = e->tint;
		st->sprites[n].material = e->material;
		n++;
	}
	/* T22: append each machine as a billboard standing on its tile (bottom-
	 * centre on the surface), tinted by kind. */
	for (MachineHandle mh = machineFirst(&st->machines);
	     mh != MACHINE_INVALID; mh = machineNext(&st->machines, mh)) {
		const Machine *m = machineGetConst(&st->machines, mh);
		float sy;

		if (m == NULL || n >= LEVEL_SPRITE_MAX)
			break;
		sy = voxmapSurfaceY(st->map, m->tileX, m->tileZ);
		if (sy < 0.0f)
			continue;	/* void tile: nothing to stand on */
		st->sprites[n].x = (float)m->tileX + 0.5f;
		st->sprites[n].y = sy;
		st->sprites[n].z = (float)m->tileZ + 0.5f;
		st->sprites[n].width = LEVEL_MACHINE_W;
		st->sprites[n].height = LEVEL_MACHINE_H;
		st->sprites[n].tint = levelMachineTint(m->kind);
		st->sprites[n].material = st->machineMaterial;
		n++;
	}
	st->spriteCount = n;
}

static TextStyle levelStyle(const App *app)
{
	UiFont *font = appFont(app);
	TextStyle style;

	style.font = font;
	style.measure = uiFontMeasure(font);
	style.pixelSize = APP_UI_FONT_PIXELS;
	return style;
}

/* EV_TOPIC_ACHIEVEMENT subscriber: show the unlock title in the toast. */
static void levelOnAchievement(void *ctx, const Event *event)
{
	LevelState *st = ctx;
	const AchievementUnlocked *unlock;

	if (event->topic != EV_TOPIC_ACHIEVEMENT ||
	    event->type != EV_ACHIEVEMENT_UNLOCKED)
		return;
	if (event->payload == NULL ||
	    event->payloadSize < sizeof(AchievementUnlocked))
		return;
	unlock = event->payload;
	toastShow(st->toast, unlock->title);
	SDL_Log("isomata: achievement unlocked: %s",
		unlock->title != NULL ? unlock->title : "(untitled)");
}

static void levelPublishTurn(LevelState *st, int direction)
{
	GameplayCameraTurn turn;
	AudioPlayRequest play = { SOUND_ROTATE };

	st->rotationSteps++;
	turn.direction = direction;
	turn.step = st->rotationSteps;
	/* NULL bus is tolerated by publishEvent (returns false). */
	publishEvent(st->bus, EV_TOPIC_GAMEPLAY, EV_GAMEPLAY_CAMERA_TURNED,
		     &turn, sizeof(turn));
	publishEvent(st->bus, EV_TOPIC_AUDIO, EV_AUDIO_PLAY, &play,
		     sizeof(play));
}

/* Restore the exact startup camera state. Shared by level_init and the
 * CMD_RESET handler (and the RESET button). */
static void levelResetCamera(LevelState *st)
{
	initCamera3D(&st->camera);
	/* Centre the 16x16 map: at yaw 0, pan x moves +X and pan y moves -Z. */
	cameraPan(&st->camera, 8.0f, -8.0f);
	SDL_Log("isomata: level camera reset (yaw %.1f, zoom %.2f)",
		cameraYawDeg(&st->camera), cameraZoomLevel(&st->camera));
}

/* Apply one 45-degree rotate step and publish it. The single path shared by
 * the command handler and the on-screen ROT buttons. */
static void levelRotate(LevelState *st, int direction)
{
	cameraRotateStep(&st->camera, direction);
	SDL_Log("isomata: level rotate %+d -> yaw %.1f", direction,
		cameraYawTargetDeg(&st->camera));
	levelPublishTurn(st, direction);
}

/* Button callbacks: no logic of their own, just the shared helpers. */
static void levelOnRotateCcw(void *ctx)
{
	levelRotate(ctx, -1);
}

static void levelOnRotateCw(void *ctx)
{
	levelRotate(ctx, 1);
}

static void levelOnReset(void *ctx)
{
	levelResetCamera(ctx);
}

/* The shared light-debug toggle: flips the flag, keeps the DEBUG button's
 * icon in step and logs. The single path behind both the F key
 * (CMD_TOGGLE_LIGHT_DEBUG) and the on-screen DEBUG button. */
static void levelToggleLightDebug(LevelState *st)
{
	levelControlsToggleDebug(&st->controls, &st->lightDebug);
	SDL_Log("isomata: light debug view %s",
		st->lightDebug ? "on" : "off");
}

static void levelOnToggleDebug(void *ctx)
{
	levelToggleLightDebug(ctx);
}

/* Build the light grid from the map: seed every parsed `$` emitter (a spot's
 * line of sight comes from the map), then flood sky + block once. Kept alive
 * with the level and destroyed in unload. */
static void levelBuildLights(LevelState *st)
{
	int w;
	int d;
	int h;
	int i;
	int count;

	if (st->map == NULL)
		return;
	w = voxmapWidth(st->map);
	d = voxmapDepth(st->map);
	/* One air layer above the topmost voxel, so sky seeds above a roof (and
	 * matches the old maxHeight+1 grid for a single-section heightmap). */
	h = voxmapLevels(st->map) + 1;
	st->lights = lightGridCreate(w, d, h);
	if (st->lights == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: light grid allocation failed");
		return;
	}
	count = voxmapLightCount(st->map);
	for (i = 0; i < count; i++) {
		const VoxmapLight *l = voxmapLightAt(st->map, i);

		if (l->kind == VOXMAP_LIGHT_POINT)
			lightGridSeedPoint(st->lights, l->x, l->y, l->z, l->r,
					   l->g, l->b, l->radius);
		else
			lightGridSeedSpot(st->lights, st->map, l->x, l->y, l->z,
					  l->dir, l->halfAngleDeg, l->r, l->g,
					  l->b, l->radius);
	}
	lightGridPropagate(st->lights, st->map);
	SDL_Log("isomata: light grid %dx%dx%d, %d emitters", w, d, h, count);
}

static bool level_init(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	TextStyle style = levelStyle(app);
	GpuBackend *gpu = appGpuBackend(app);
	char mapPath[512];
	int16_t spriteMaterial;

	levelResetCamera(st);
	/* T15 defaults: smooth lighting on (the classic voxel look), debug off. */
	st->smoothLight = true;
	st->lightDebug = false;
	initDrawList(&st->list, LEVEL_DRAWLIST_CAPACITY);
	/* The atlas + material table are owned by the GPU backend; in smoke
	 * mode (no GPU) materials stay NULL and the emitters use the built-in
	 * fallback regions. */
	st->materials = gpu != NULL ? gpuBackendMaterials(gpu) : NULL;
	spriteMaterial = (int16_t)materialIdByName(st->materials, "sprite");
	st->spriteMaterial = spriteMaterial;
	st->machineMaterial =
		(int16_t)materialIdByName(st->materials, "machine");

	/* Load the demo map through SDL I/O so Android APK assets resolve. The
	 * PNG slice directory (T13b) is preferred; the ASCII demo.txt is the
	 * source of truth and the fallback. Both are the SAME scene, so the
	 * render is identical; the log names the source that loaded. */
	{
		char mapDir[512];
		int mapSlices = 0;

		if (platformAssetPath("maps/demo", mapDir, sizeof(mapDir)) !=
		    NULL) {
			st->map = loadVoxmapDirectory(mapDir, st->materials,
						      &mapSlices);
			if (st->map != NULL)
				SDL_Log("isomata: level map from PNG directory '%s' (%d slices)",
					mapDir, mapSlices);
		}
	}
	if (st->map == NULL) {
		/* parseVoxmapText copies the cells it needs, so the SDL_LoadFile
		 * buffer is freed immediately. */
		if (platformAssetPath("maps/demo.txt", mapPath,
				      sizeof(mapPath)) != NULL) {
			size_t mapSize = 0;
			void *mapText = SDL_LoadFile(mapPath, &mapSize);

			if (mapText != NULL) {
				st->map = parseVoxmapText(mapText, mapSize,
							  st->materials);
				SDL_free(mapText);
			}
		}
		if (st->map != NULL)
			SDL_Log("isomata: level map from ASCII '%s'", mapPath);
	}
	if (st->map == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: demo map unavailable");
	levelBuildLights(st);
	levelBuildEntities(st, spriteMaterial);
	levelBuildMachines(st);
	levelBuildSmoke(st);		/* TEMPORARY T22 smoke driver (T23 deletes) */

	/* UI root: a zero-padding pane is the layout root the toast and the
	 * controls hang off. The pane itself is never drawn (its draw() paints
	 * an opaque fill); each child is drawn individually (see levelDrawUi). */
	st->root = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	st->toast = uiCreateToast(&style);
	if (st->root == NULL || st->toast == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: level UI allocation failed");
		/* Nothing has been appended yet, so each element is freed once
		 * (uiDestroyElement is NULL-safe). */
		uiDestroyElement(st->toast);
		uiDestroyElement(st->root);
		st->root = NULL;
		st->toast = NULL;
	} else {
		uiAppendChild(st->root, st->toast);
		/* The four icon-only controls; a failed build leaves all
		 * pointers NULL, which the draw/layout walks tolerate. */
		if (!levelControlsCreate(&st->controls, st->root, &style,
					 levelOnRotateCcw, levelOnRotateCw,
					 levelOnToggleDebug, levelOnReset, st))
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: control buttons allocation failed");
	}
	/* Start the DEBUG icon in step with the flag (false at init). */
	levelControlsSetDebugIcon(&st->controls, st->lightDebug);

	/* Subscribe to achievement unlocks (self-unsubscribes in unload). */
	st->bus = appEventBus(app);
	if (st->bus != NULL) {
		if (!subscribeEvent(st->bus, EV_TOPIC_ACHIEVEMENT,
				    levelOnAchievement, st))
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: achievement subscribe failed");
	} else {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: no event bus (toast/audio disabled)");
	}
	return true;
}

static void levelHandleCommand(LevelState *st, App *app, Command cmd)
{
	switch (cmd) {
	case CMD_ROTATE_CW:
		levelRotate(st, 1);
		break;
	case CMD_ROTATE_CCW:
		levelRotate(st, -1);
		break;
	case CMD_ZOOM_IN:
		cameraZoom(&st->camera, 1.0f);
		SDL_Log("isomata: level zoom %.2f", cameraZoomLevel(&st->camera));
		break;
	case CMD_ZOOM_OUT:
		cameraZoom(&st->camera, -1.0f);
		SDL_Log("isomata: level zoom %.2f", cameraZoomLevel(&st->camera));
		break;
	case CMD_RESET:
		levelResetCamera(st);
		break;
	case CMD_TOGGLE_SMOOTH_LIGHT:
		st->smoothLight = !st->smoothLight;
		SDL_Log("isomata: smooth lighting %s",
			st->smoothLight ? "on" : "off (flat T14 path)");
		break;
	case CMD_TOGGLE_LIGHT_DEBUG:
		levelToggleLightDebug(st);
		break;
	case CMD_BACK:
		{
			Scene *pause = pauseSceneCreate();

			if (pause != NULL && !pushScene(appSceneStack(app), pause)) {
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					     "level_scene: scene stack rejected pause");
				destroyScene(pause);
			}
		}
		break;
	default:
		break;
	}
}

static void level_update(void *self, App *app, float dt)
{
	LevelState *st = scenePayload(self);
	const InputFrame *frame = appInputFrame(app);
	int i;

	/* Pointer-only: the buttons are tap controls with no keyboard focus
	 * model (Q/E/R own rotation/reset), so ACTIVATE must not fire a
	 * button. See uiBridgeDispatchPointer. */
	(void)uiBridgeDispatchPointer(st->root, frame);

	/* Opt-in pointer/tap detail (ISO_LOG=debug). Discrete events only, so
	 * a drag does not spam one line per frame. */
	if (frame->tap)
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: level tap at %d,%d", frame->tapX,
			     frame->tapY);
	if (frame->zoomSteps != 0)
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: level pinch zoom steps %d",
			     frame->zoomSteps);

	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++)
		levelHandleCommand(st, app, frame->commands[i]);

	/* The content follows the finger: dragging right moves the map right
	 * and dragging down moves it down (cameraPanByDrag). */
	if (frame->panDx != 0 || frame->panDy != 0)
		cameraPanByDrag(&st->camera, frame->panDx, frame->panDy);

	updateCamera3D(&st->camera, dt);
	/* Advance the demo critters: FSM + A* driven routes (T21). */
	entitiesUpdate(&st->entities, dt);
	levelUpdateWalkers(st, dt);
	/* Advance the T22 machines, log their state-change wakes and feed the
	 * TEMPORARY T22 smoke driver (which reacts to the PHASE_DONE wake). T23
	 * deletes the levelUpdateSmoke line and the levelSmoke* block. */
	{
		int nw = machinesUpdate(&st->machines, dt, st->wakes,
					LEVEL_MACHINE_WAKE_MAX);
		int i;

		for (i = 0; i < nw; i++)
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: [t=%llums] machine %d wake %s owner %d",
				     levelMs(), st->wakes[i].machine,
				     machineWakeKindName(st->wakes[i].kind),
				     st->wakes[i].owner);
		levelUpdateSmoke(st, dt, st->wakes, nw);
	}
	toastUpdate(st->toast, dt);
}

/* Draw the toast and the four controls on top of the world. The pane root
 * is a layout container only and is NOT drawn: its draw() paints a constant
 * opaque UI_COLOR_BACKGROUND fill, which would sit behind the toast's
 * alpha-scaled fill and make the box snap from full opacity to gone at
 * HIDDEN instead of fading. Each child is drawn individually, so the toast
 * still self-hides via its alpha while the buttons always draw. Child rects
 * are assigned directly (no uiLayout): the pane's single-axis layout would
 * stack all five children instead of placing the corner controls; the rule
 * itself lives in the pure level_controls module. */
static void levelDrawUi(LevelState *st, App *app)
{
	UiDrawCtx *ctx = appUiDrawCtx(app);
	int sx;
	int sy;
	int sw;
	int sh;

	if (st->root == NULL)
		return;
	/* Keep the UI inside the window's safe area so it clears notches /
	 * status bars (on a desktop display the safe area is the full window). */
	appSafeArea(app, &sx, &sy, &sw, &sh);

	/* Toast: clamp to the safe width so it cannot overflow a narrow
	 * virtual viewport (the boosted phone scale makes those the norm),
	 * keeping it centred. */
	{
		int toastW = LEVEL_TOAST_W;

		if (toastW > sw - 2 * LEVEL_BUTTON_MARGIN)
			toastW = sw - 2 * LEVEL_BUTTON_MARGIN;
		uiSetRect(st->toast, sx + (sw - toastW) / 2, sy + LEVEL_TOAST_TOP,
			  toastW, LEVEL_TOAST_H);
	}

	levelControlsLayout(&st->controls, sx, sy, sw, sh);

	uiDraw(st->controls.rotL, ctx);
	uiDraw(st->controls.rotR, ctx);
	uiDraw(st->controls.debug, ctx);
	uiDraw(st->controls.reset, ctx);
	uiDraw(st->toast, ctx);
}

/* T20 path-debug overlay. Markers sample the reserved white atlas cell when
 * the debug view is up so their tint reads as a flat colour; without a white
 * cell (no GPU atlas) the built-in sprite region stands in. */
static const float kDebugMarkerFallbackUV[4][2] = ATLAS_UV_SPRITE;

/* Append each entity's start / path / destination markers as flat-tinted
 * billboards lifted just above the tile surface. Called only in the
 * light-debug view, and AFTER buildFrameDrawList's painter sort, so the
 * markers draw last in a fixed order (start, route, destination) and are never
 * occluded by terrain — a debug overlay should always read. The destination is
 * appended last so it draws over the path marker on its own tile. */
static void levelAppendDebugMarkers(LevelState *st, const float uv[4][2])
{
	int n = entitiesDebugMarkers(&st->entities, st->markers,
				     LEVEL_DEBUG_MARKER_MAX);
	int i;

	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		     "isomata: path-debug overlay %d marker%s", n,
		     n == 1 ? "" : "s");
	for (i = 0; i < n; i++) {
		const EntityDebugMarker *m = &st->markers[i];
		float sy = voxmapSurfaceY(st->map, m->x, m->z);
		SpriteEntity s;

		if (sy < 0.0f)
			continue;	/* void tile: nothing to stand on */
		s.x = (float)m->x + 0.5f;
		s.y = sy + ENTITY_DEBUG_MARKER_LIFT;
		s.z = (float)m->z + 0.5f;
		s.width = entityDebugMarkerWidth((int)m->kind);
		s.height = s.width;
		s.tint = entityDebugMarkerTint((int)m->kind);
		s.material = -1;
		appendSpriteUV(&st->list, &s, &st->camera, uv);
	}
}

static void level_draw(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	GpuBackend *gpu = appGpuBackend(app);

	if (gpu != NULL) {
		int pw = appPixelWidth(app);
		int ph = appPixelHeight(app);
		float aspect = ph > 0 ? (float)pw / (float)ph : 1.0f;
		Mat4 projection = cameraProjection(&st->camera, aspect);
		Mat4 gridProj = gridProjection(&st->camera, aspect);
		Mat4 view = cameraView(&st->camera);
		Mat4 viewProj = mat4Multiply(&projection, &view);
		Mat4 gridViewProj = mat4Multiply(&gridProj, &view);
		GridQuad grid;

		/* Infinite ground grid beneath the map: drawn first so the
		 * voxel faces (painter-ordered after it) occlude it, and it
		 * shows only on void tiles and beyond the map. It uses a
		 * depth range that encloses the whole quad so the camera's
		 * near plane never cuts a visible edge across the near ground
		 * at low zoom. */
		buildGridQuad(&st->camera, aspect, &grid);
		gpuBackendDrawGrid(gpu, &gridViewProj, &grid);

		{
			FrameOptions opts = { st->smoothLight, st->lightDebug,
					      NULL };
			float debugUV[4][2];
			bool haveDebugUV = false;

			/* The debug view samples a reserved white atlas cell;
			 * without a GPU (smoke) the emitter's spare fallback
			 * applies. */
			if (st->lightDebug &&
			    gpuBackendDebugUV(gpu, debugUV)) {
				opts.debugUV = debugUV;
				haveDebugUV = true;
			}
			/* Billboards follow the registry (position + tint);
			 * they still go through the sprite path + flat light
			 * inside buildFrameDrawList. */
			levelSyncSprites(st);
			buildFrameDrawList(st->map, st->materials, st->lights,
					   st->sprites, st->spriteCount,
					   &st->camera, &st->list, &opts);
			/* T20: overlay the entity paths last, and only in the
			 * debug view — zero geometry/cost when it is off. */
			if (st->lightDebug) {
				if (haveDebugUV)
					levelAppendDebugMarkers(st, debugUV);
				else
					levelAppendDebugMarkers(
						st, kDebugMarkerFallbackUV);
			}
		}
		gpuBackendDrawList(gpu, &viewProj, &st->list);
	}
	levelDrawUi(st, app);
}

static void level_unload(void *self, App *app)
{
	LevelState *st = scenePayload(self);

	(void)app;
	if (st->bus != NULL)
		unsubscribeEvent(st->bus, EV_TOPIC_ACHIEVEMENT,
				 levelOnAchievement, st);
	/* uiDestroyElement frees the whole subtree, so the buttons are freed
	 * here exactly once (never separately); the pointers are just cleared. */
	uiDestroyElement(st->root);
	st->root = NULL;
	st->toast = NULL;
	st->controls = (LevelControls){ 0 };
	destroyDrawList(&st->list);
	destroyLightGrid(st->lights);
	st->lights = NULL;
	destroyVoxmap(st->map);
	st->map = NULL;
}

static const SceneVt levelVt = {
	level_init, level_update, level_draw, level_unload,
	sizeof(LevelState), "level",
};

Scene *levelSceneCreate(void)
{
	return createScene(&levelVt);
}
