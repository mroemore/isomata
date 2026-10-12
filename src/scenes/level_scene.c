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
#include "agents/activity.h"
#include "agents/agent.h"
#include "agents/board.h"
#include "agents/job.h"
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
#include "sim/brain.h"
#include "sim/garments.h"
#include "sim/machines.h"
#include "sim/selection.h"
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

/* T21 demo, now agent-driven: critters cycle a two-destination list and each
 * leg is a JOB_WANDER job on the critter's own board, walked by
 * agentsUpdate's lifecycle (UNCLAIMED -> CLAIMED -> WALKING -> INTERACTING ->
 * WAITING -> DONE) with the route computed by A* (entityPathTo). The
 * destinations are unchanged open plateau tiles:
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

/* The demo's timed WAIT phase, unchanged from the executor FSM's: a job
 * parks on its `duration` for this long before completing. It overrides the
 * wander activity's own 3s runtime so a demo leg stays a demo leg. */
#define DEMO_WAIT_SECONDS 0.4f

/* --- T22 machines ------------------------------------------------------- *
 *
 * Five machines in a "laundromat corner" row on the north grass (z=1, x1..5)
 * plus a SIXTH, ISOLATED washer far east at (14,1) — at least K+1 tiles from
 * the cluster. The row is clear of the room (x4-8,z9-12), the tower (x6-7,z3-6)
 * and every critter lane (A paces z=8, B the x=10 column z3-13, C detours
 * south of the room). The isolated washer exists so the selection policies
 * visibly diverge: when a cluster washer is occupied, a `closest` customer
 * still picks the nearest cluster washer while a `spread_k` customer (which
 * penalises candidates within K=2 of the occupied one) picks the isolated
 * washer. Each renders as a 1.2x1.4 billboard through the sprite path, tinted
 * per kind (washers cool, dryers warm, broken dark grey); the tile stays
 * walkable so a critter can step onto it to interact (v1 convention,
 * sim/machines.h). */
#define LEVEL_MACHINE_COUNT 6
#define LEVEL_MACHINE_W 1.2f
#define LEVEL_MACHINE_H 1.4f
#define LEVEL_MACHINE_RUN_SECS 30.0f
#define LEVEL_MACHINE_TINT_WASHER DRAW_TINT(150, 200, 255, 255)	/* cool */
#define LEVEL_MACHINE_TINT_DRYER DRAW_TINT(255, 190, 130, 255)	/* warm */
#define LEVEL_MACHINE_TINT_BROKEN DRAW_TINT(80, 80, 90, 255)	/* dark grey */

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
	/* Isolated washer: >K from the cluster, so spread_k can diverge. */
	{ MACHINE_KIND_WASHER, 14, 1, 3 },
};

/* A demo critter: its body, its mind, and the leg it walks next.
 *
 * The three destination tables and the per-critter speeds and easings below
 * are the demo's and have not changed; what changed is WHO PICKS THE NEXT
 * LEG. Each critter is an Agent with its own personal board, and each leg is
 * a JOB_WANDER job bound to one destination tile (duration = the demo's old
 * WAIT phase, band = DEMO_WANDER_BAND, hand-set until the activity catalogue
 * computes it). The hardcoded leg loop is gone: agentsUpdate claims the job,
 * runs the lifecycle and routes the body, and this file only queues the next
 * leg when the critter is idle and retires the one that finished. */
typedef struct DemoCritter {
	EntityHandle handle;	/* the body, in the shared entity registry */
	AgentHandle agent;	/* the mind, in the level's agent registry */
	const int (*dests)[2];	/* the critter's two-destination route */
	int destCount;
	int destIndex;		/* which destination is next */
	int job;		/* the leg job on the personal board, or -1 */
	int lastState;		/* last observed job state, for the log */
	/* The demo paces its own patrol so the framework's FILL gets airtime
	 * (see levelQueueNextLeg). A critter alternates between its queued leg
	 * and a self-generated fill, which is the visible evidence that an
	 * idle agent does something of its own accord. Without it the producer
	 * would always have a leg ready and the fill path would never be
	 * reached — the framework behaving correctly, since a producer is never
	 * talked over, but leaving the mechanism unwitnessed. */
	bool fillTurn;
} DemoCritter;

/* --- T23 customers ------------------------------------------------------ *
 *
 * The customer demo replaces the T22 smoke driver wholesale. Customers spawn
 * at the door tile on an interval, each with its own selection policy (the
 * spawner cycles closest / spread_k / cheapest so the heuristic genuinely
 * changes choices), walk to a machine picked by that policy, pay, load, walk
 * away while it runs, collect on the wake, dry, and leave clean-dry. A
 * scripted break takes washer 0 down during its second run so the
 * INTERRUPTED -> re-plan path is exercised visibly; it is repaired later.
 *
 * The door, the exit and the "owner away" wait spot are one clear grass tile
 * at the map's NW corner, one step from the machine row. */
#define LEVEL_DOOR_X 0
#define LEVEL_DOOR_Z 0
#define LEVEL_EXIT_X 0
#define LEVEL_EXIT_Z 0
#define LEVEL_WAIT_X 0
#define LEVEL_WAIT_Z 0
#define LEVEL_CUSTOMER_W 1.0f
#define LEVEL_CUSTOMER_H 1.6f
#define LEVEL_CUSTOMER_TINT_CLOSEST DRAW_TINT(120, 200, 255, 255)	/* blue */
#define LEVEL_CUSTOMER_TINT_SPREAD DRAW_TINT(150, 255, 150, 255)	/* green */
#define LEVEL_CUSTOMER_TINT_CHEAPEST DRAW_TINT(255, 220, 120, 255)	/* amber */
/* Scripted break: washer 0 goes down this many seconds into its second run,
 * and is repaired this many seconds later. */
#define LEVEL_SCRIPT_BREAK_MACHINE 0
#define LEVEL_SCRIPT_BREAK_DELAY 2.0f
#define LEVEL_SCRIPT_REPAIR_SECS 25.0f

/* Forward declaration: the opening pair is spawned (and logged) from
 * level_init, before the brain-glue helpers are defined. */
static void levelLogBrainEvent(const BrainEvent *ev);

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
	SimWorld sim;			/* T23: registries + customers + machines */
	DemoCritter critters[LEVEL_WALKER_COUNT];
	int critterCount;
	AgentRegistry agents;		/* one agent per demo critter */
	AgentBoards boards;		/* the shared board (idle in this demo) */
	float clock;			/* seconds since load: the board clock */
	int16_t spriteMaterial;		/* atlas material for the critters */
	int16_t machineMaterial;	/* atlas material for the machines */
	BrainEvent events[BRAIN_MAX_EVENTS];	/* per-frame brain events */
	/* Scripted demo break / repair of washer 0, and the scripted divergence
	 * pair (a closest + a spread_k customer) spawned once washer 0 is
	 * occupied. */
	int washer0Runs;	/* RUN_START events seen for washer 0 */
	bool divergenceSpawned;	/* the closest/spread_k pair was spawned */
	float breakTimer;	/* countdown to the scripted break (<= 0 = idle) */
	float repairTimer;	/* countdown to the scripted repair (<= 0 = idle) */
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
 * plateau, each now paired with an Agent that decides what it does next (see
 * the DEMO_DESTS_* note). The first leg is queued on the first update. */
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

	st->critterCount = 0;
	for (i = 0; i < LEVEL_WALKER_COUNT; i++) {
		EntityHandle h = entityCreate(&st->sim.entities);
		Entity *e = entityGet(&st->sim.entities, h);
		AgentHandle ah;
		Agent *a;
		DemoCritter *c;

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

		ah = agentCreate(&st->agents, ROLE_CRITTER);
		a = agentGet(&st->agents, ah);
		if (a == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: demo agent %d create failed", i);
			continue;
		}
		/* The agent's body is the entity above; from here the two are
		 * separate records that agentsUpdate links. */
		a->entity = h;

		c = &st->critters[st->critterCount];
		c->handle = h;
		c->agent = ah;
		c->dests = demo[i].dests;
		c->destCount = demo[i].destCount;
		c->destIndex = 0;
		c->job = JOB_INVALID;
		c->lastState = JOB_UNCLAIMED;
		st->critterCount++;
		SDL_Log("isomata: demo entity %d created at (%d,%d), agent %d",
			h, demo[i].startX, demo[i].startZ, ah);
	}
}

/* The ACTIVITY a job is an instance of, by name. The debug log names the
 * activity rather than the raw job kind because the catalogue is what the
 * demo is now demonstrating: an idle critter running a COFFEE BREAK is doing
 * an activity of its own accord, and printing the kind slot it happens to be
 * declared on ("PAY") would say nothing true about what the critter did.
 *
 * A kind no row claims falls back to the bare job kind — the only way to get
 * here is a job the tick has already failed for being unscoreable, so this
 * names a dead job rather than a live one. */
static const char *levelActivityName(int kind)
{
	const Activity *act = activitiesGet(kind);

	return (act != NULL) ? act->name : jobKindName(kind);
}

/* Queue the critter's next leg on its personal board. It is taken on the NEXT
 * tick: this runs after agentsUpdate, so the queue lands with the frame's
 * decisions already made — one frame per leg, the same one-frame-per-leg the
 * activity step costs, and the same shape as the old hardcoded loop, which
 * called demoStartLeg from an IDLE frame and therefore also started a leg on
 * the frame after the previous one finished. The destination advances when
 * the leg is QUEUED, exactly as the old code advanced it when the leg
 * STARTED (including when A* refused the route: the next leg is the one
 * after, either way). */
static void levelQueueNextLeg(LevelState *st, DemoCritter *c)
{
	Agent *a = agentGet(&st->agents, c->agent);
	const int *d;
	Job j;

	if (a == NULL || c->job != JOB_INVALID || a->engagedJob != JOB_INVALID)
		return;
	/* THE CRITTER'S TURN TO FILL: stand aside and let the framework's own
	 * agentBestFill put something on the empty board. agentsUpdate fills
	 * only a genuinely empty board, so standing aside is exactly what makes
	 * the self-generated activity happen. */
	if (c->fillTurn)
		return;
	d = c->dests[c->destIndex];
	/* The declaration comes from the catalogue, so the demo's leg is the
	 * same row an errand or a self-chosen fill would be — one
	 * declaration, two producers (agents/activity.h). Only the DESTINATION
	 * is the demo's own, and only the wait length is overridden so a demo
	 * leg stays a demo leg. */
	activityBindJob(&j, ACT_WANDER);
	j.duration = DEMO_WAIT_SECONDS;
	jobSetTarget(&j, JOB_TARGET_TILE, JOB_TILE_PACK(d[0], d[1]));
	/* The band is COMPUTED from the needs a wander serves. Nothing here
	 * assigns it, which is the whole point: the demo no longer knows what
	 * its own job is worth. */
	/* The activity is a compile-time constant that this file also
	 * includes the catalogue for, so the -1 error is unreachable; a
	 * CHECKED write would be a branch that can never be false. If a row
	 * ever failed to resolve, the tick's own refresh DENIES the job on
	 * the next tick, which is the single place that handles it. */
	{
		int band = jobUrgency(a, &j);

		/* Deliberately UNCHECKED, and the (void) says so: ACT_WANDER is
		 * a compile-time constant and this file includes the catalogue,
		 * so the -1 error is structurally unreachable. If it were not,
		 * the tick's own refresh DENIES any job it cannot score on the
		 * next tick — the one place that handles it. */
		(void)band;
		j.band = band;
	}
	c->job = boardEnqueue(&a->personal, &j, JOB_PRIO_FILL);
	if (c->job == JOB_INVALID)
		return;		/* board full: retried next tick */
	c->destIndex = (c->destIndex + 1) % c->destCount;
}

/* Retire finished work so the board does not fill with dead rows (the
 * board's bounded drop would eventually start refusing work), and log the
 * lifecycle as it stands.
 *
 * It sweeps EVERY terminal job on the board, not just the demo's own leg:
 * since task 4 the framework generates its own fill jobs there, and an owner
 * that retires only the rows it remembers leaving would leak the rest. A
 * job the agent is still holding is left for the tick to let go of next
 * frame — the ownership check in agentsUpdate is what makes that safe.
 *
 * Each retirement flips the critter's fill turn, so the demo alternates
 * between a queued leg and a self-generated fill (see levelQueueNextLeg). */
static void levelReapLeg(LevelState *st, DemoCritter *c)
{
	Agent *a = agentGet(&st->agents, c->agent);
	int h;

	if (a == NULL)
		return;
	if (c->job != JOB_INVALID) {
		Job *j = boardJob(&a->personal, c->job);

		if (j != NULL && j->state != c->lastState) {
			SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: agent %d job %d (%s) state %s -> %s",
				     a->id, c->job, levelActivityName(j->kind),
				     jobStateName(c->lastState),
				     jobStateName(j->state));
			c->lastState = j->state;
		}
	}
	for (h = 0; h < BOARD_MAX; h++) {
		Job *j = boardJob(&a->personal, h);
		bool wasLeg;

		if (j == NULL || (j->state != JOB_DONE && j->state != JOB_FAILED))
			continue;
		if (a->engagedJob == h)
			continue;	/* still held: released next frame */
		wasLeg = (c->job == h);
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: agent %d job %d (%s band %d) %s;"
			     " retiring it",
			     a->id, h, levelActivityName(j->kind), j->band,
			     jobStateName(j->state));
		boardRetire(&a->personal, h);
		c->fillTurn = !c->fillTurn;
		if (wasLeg) {
			c->job = JOB_INVALID;
			c->lastState = JOB_UNCLAIMED;
		}
	}
}

/* The why-this-job log (the spec's highest-value observability artifact): the
 * two bands and the margin that decided the tick, plus the route the body was
 * given and the straight-line distance it detoured from. One line per critter
 * whose engagement CHANGED, so the log reads as a sequence of decisions
 * rather than one line per frame. */
static void levelLogDecision(LevelState *st, DemoCritter *c, int before)
{
	Agent *a = agentGet(&st->agents, c->agent);
	JobBoard *ab;
	Job *from;
	Job *to;
	int skip[2];
	int challenger;
	int fromBand = JOB_INVALID;
	int toBand = JOB_INVALID;
	int challengerBand = JOB_INVALID;
	int tx = 0;
	int tz = 0;
	int straight;

	if (a == NULL || a->engagedJob == before)
		return;
	/* The engaged job lives on whichever board the agent holds it, which
	 * for the demo is always its own; the log reads it the same way the
	 * tick does rather than assuming. */
	ab = a->engagedShared ? &st->boards.shared : &a->personal;
	from = (before != JOB_INVALID) ? boardJob(ab, before) : NULL;
	to = boardJob(ab, a->engagedJob);
	fromBand = (from != NULL) ? from->band : JOB_INVALID;
	toBand = (to != NULL) ? to->band : JOB_INVALID;
	skip[0] = a->engagedJob;
	skip[1] = JOB_INVALID;
	challenger = boardBestFor(ab, skip, a->id);
	if (challenger != JOB_INVALID)
		challengerBand = ab->slots[challenger].band;

	if (to == NULL) {
		SDL_Log("isomata: agent %d (entity %d) released job %d (band %d)",
			a->id, c->handle, before, fromBand);
		return;
	}
	if (!jobTargetTile(to, VOXMAP_MAX_DIM, &tx, &tz))
		return;
	{
		Entity *e = entityGet(&st->sim.entities, c->handle);

		straight = (e != NULL)
				   ? pathfindManhattan(e->tileX, e->tileZ, tx, tz)
				   : 0;
	}
	SDL_Log("isomata: agent %d (entity %d) job %d %s -> (%d,%d) band %d"
		" (from %d, challenger %d, margin %d, dwell %.2fs) path %d"
		" straight %d%s",
		a->id, c->handle, a->engagedJob, levelActivityName(to->kind), tx, tz,
		toBand, fromBand, challengerBand, AGENT_PREEMPT_MARGIN,
		(double)a->dwellLeft, a->pathLen, straight,
		a->pathLen > straight ? " DETOUR" : "");
}

/* Advance the demo critters: the board clock, then the agent tick (decay,
 * decide, claim, route, run the lifecycle), then the reap. entitiesUpdate
 * still walks the shared entity registry once per frame, after every decision
 * is in — the same order the hardcoded walker loop ran in. */
static void levelUpdateAgents(LevelState *st, float dt)
{
	int before[LEVEL_WALKER_COUNT];
	int i;

	st->clock += dt;
	/* The board clock is stamped before any claim, so a reservation is
	 * measured from the tick it was taken on and a claim nobody starts
	 * lapses instead of sitting on a job forever. A critter's own claim
	 * is never swept: it starts its job in the same tick, so the sweep
	 * sees a running job and leaves it alone. */
	boardExpire(&st->boards.shared, st->clock);
	for (i = 0; i < st->critterCount; i++) {
		Agent *a = agentGet(&st->agents, st->critters[i].agent);

		if (a != NULL)
			boardExpire(&a->personal, st->clock);
		before[i] = (a != NULL) ? a->engagedJob : JOB_INVALID;
	}

	agentsUpdate(&st->agents, &st->sim.entities, &st->boards, st->map, dt,
		     NULL);

	/* Retire what finished, then queue the next leg: the agent has let
	 * go of a finished job during the tick above, so this is the moment
	 * its board has room again. */
	for (i = 0; i < st->critterCount; i++)
		levelReapLeg(st, &st->critters[i]);
	for (i = 0; i < st->critterCount; i++) {
		levelQueueNextLeg(st, &st->critters[i]);
		levelLogDecision(st, &st->critters[i], before[i]);
	}
}

/* Derive the billboard array from the registry each frame (position + sprite
 * link). Order is registry slot order, which is stable. */
/* ---- T22 machines ----------------------------------------------------- */

/* The pinned tint for a machine: broken machines read as dark grey, otherwise
 * washers are cool and dryers warm (an unknown kind reads as a washer). */
static uint32_t levelMachineTint(int kind, int state)
{
	if (state == MACHINE_STATE_BROKEN)
		return LEVEL_MACHINE_TINT_BROKEN;
	return kind == MACHINE_KIND_DRYER ? LEVEL_MACHINE_TINT_DRYER
					  : LEVEL_MACHINE_TINT_WASHER;
}

/* Milliseconds since SDL init, prefixed on every brain/machine log line as a
 * timestamp. */
static unsigned long long levelMs(void)
{
	return (unsigned long long)SDL_GetTicks();
}


/* Register the five machines (T22 layer-1 objects) into the sim world. */
static void levelBuildMachines(LevelState *st)
{
	int i;

	for (i = 0; i < LEVEL_MACHINE_COUNT; i++) {
		MachineHandle h = machineCreate(
			&st->sim.machines, LEVEL_MACHINE_SPEC[i].kind,
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

/* THE BAND-TINT OVERLAY (the spec's observability item): an agent's own TOP
 * wantUrgency — the most pressing thing it wants, across all six needs — as a
 * colour, so a crowd going restless or panicking is visible as colour rather
 * than as a log line nobody is watching. Cool blue is nothing pressing, hot
 * red is the top of the ladder.
 *
 * It reads the needs DIRECTLY rather than the job's band, so it keeps
 * telling the truth while an agent is between jobs, and it costs one six-slot
 * scan per critter per frame, only while the debug view is on. */
static uint32_t levelUrgencyTint(const Agent *a)
{
	int top = 0;
	int n;

	for (n = 0; n < NEED_COUNT; n++) {
		int u = wantUrgency(&a->needs, n);

		if (u > top)
			top = u;
	}
	/* NEED_BANDS is a compile-time constant >= 2, so the divisor is safe. */
	return DRAW_TINT((unsigned char)(top * 255 / (NEED_BANDS - 1)), 0,
			 (unsigned char)(255 - top * 255 / (NEED_BANDS - 1)), 255);
}

static void levelSyncSprites(LevelState *st)
{
	EntityHandle h;
	size_t n = 0;
	int i;

	for (h = entityFirst(&st->sim.entities); h != ENTITY_INVALID;
	     h = entityNext(&st->sim.entities, h)) {
		const Entity *e = entityGet(&st->sim.entities, h);

		if (e == NULL || n >= LEVEL_SPRITE_MAX)
			break;
		st->sprites[n].x = e->x;
		st->sprites[n].y = e->y;
		st->sprites[n].z = e->z;
		st->sprites[n].width = e->width;
		st->sprites[n].height = e->height;
		st->sprites[n].tint = e->tint;
		st->sprites[n].material = e->material;
		/* Band-tint overlay: a critter's billboard is recoloured by its
		 * own top want under the debug view only — zero cost when the
		 * view is off, and the tint is the agent's own needs rather
		 * than whatever entity colour it happens to carry.
		 *
		 * IT ALSO SWAPS THE MATERIAL, and that is the part that makes
		 * it work. The critters normally draw through "sprite", which
		 * is a RED ball, so a tint can only scale the red channel and
		 * the whole ramp collapses to "black or red". The overlay
		 * therefore borrows the flat-white material the machines use
		 * for exactly the same reason — so the tint IS the colour, and
		 * a crowd going restless reads as one. Debug view only; the
		 * normal view keeps the critters' own sprite. */
		if (st->lightDebug) {
			for (i = 0; i < st->critterCount; i++) {
				const Agent *a;

				if (st->critters[i].handle != h)
					continue;
				a = agentGet(&st->agents,
					     st->critters[i].agent);
				if (a != NULL) {
					st->sprites[n].tint = levelUrgencyTint(a);
					st->sprites[n].material =
						st->machineMaterial;
				}
			}
		}
		n++;
	}
	/* T22: append each machine as a billboard standing on its tile (bottom-
	 * centre on the surface), tinted by kind. */
	for (MachineHandle mh = machineFirst(&st->sim.machines);
	     mh != MACHINE_INVALID; mh = machineNext(&st->sim.machines, mh)) {
		const Machine *m = machineGetConst(&st->sim.machines, mh);
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
		st->sprites[n].tint = levelMachineTint(m->kind, m->state);
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
	/* T23: the sim world owns the entity + machine registries. Bind the
	 * map and the door / exit / wait tiles, then build the critters and the
	 * machines into it, then start the customer spawner. */
	simWorldInit(&st->sim, st->map, LEVEL_DOOR_X, LEVEL_DOOR_Z,
		     LEVEL_EXIT_X, LEVEL_EXIT_Z, LEVEL_WAIT_X, LEVEL_WAIT_Z);
	st->sim.customerWidth = LEVEL_CUSTOMER_W;
	st->sim.customerHeight = LEVEL_CUSTOMER_H;
	/* The flat-white "machine" material so the per-policy tint reads (the
	 * "sprite" material renders red whatever the tint). */
	st->sim.customerMaterial = st->machineMaterial;
	st->sim.policyTint[SEL_POLICY_CLOSEST] = LEVEL_CUSTOMER_TINT_CLOSEST;
	st->sim.policyTint[SEL_POLICY_SPREAD_K] = LEVEL_CUSTOMER_TINT_SPREAD;
	st->sim.policyTint[SEL_POLICY_CHEAPEST] = LEVEL_CUSTOMER_TINT_CHEAPEST;
	st->washer0Runs = 0;
	st->divergenceSpawned = false;
	st->breakTimer = -1.0f;
	st->repairTimer = -1.0f;
	/* The agent side: a registry (which owns every critter's personal
	 * board by value), the one shared board, and the clock that board's
	 * reservations are measured against. */
	agentsInit(&st->agents);
	boardInit(&st->boards.shared, true);
	st->clock = 0.0f;
	levelBuildEntities(st, spriteMaterial);
	levelBuildMachines(st);
	/* The interval spawner stays OFF until the scripted divergence pair has
	 * arrived (see levelUpdateBrain). One closest customer goes in first and
	 * claims the nearest cluster washer; then a closest + a spread_k pair
	 * arrive while that washer is occupied, so their choices diverge: the
	 * closest one takes the next free cluster washer, the spread_k one
	 * (penalising candidates within K=2 of the occupied one) takes the
	 * isolated washer far east. */
	simSetSpawner(&st->sim, false, BRAIN_SPAWN_INTERVAL, BRAIN_SPAWN_CAP);
	{
		int n0 = 0;
		int k;

		(void)simSpawnCustomer(&st->sim, SEL_POLICY_CLOSEST,
				       BRAIN_START_MONEY, st->events,
				       BRAIN_MAX_EVENTS, &n0);
		for (k = 0; k < n0; k++)
			levelLogBrainEvent(&st->events[k]);
	}

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

/* --- T23 customer brain glue ------------------------------------------- */

/* One readable log line per brain event (ISO_LOG=debug), stamped with the
 * millisecond clock so the story lines up with the machine wakes. */
static void levelLogBrainEvent(const BrainEvent *ev)
{
	unsigned long long t = levelMs();

	switch (ev->kind) {
	case BRAIN_EV_SPAWN:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d (handle %d, policy %s) arrived at the door with $%d",
			     t, ev->id, ev->customer, selPolicyName(ev->policy),
			     ev->amount);
		break;
	case BRAIN_EV_TASK_BEGIN:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d chose machine %d (dist %d, score %d, policy %s)",
			     t, ev->id, ev->machine, ev->distance,
			     ev->score, selPolicyName(ev->policy));
		break;
	case BRAIN_EV_NO_CANDIDATE:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d found no free machine; waiting and retrying",
			     t, ev->id);
		break;
	case BRAIN_EV_CLAIM_DENIED:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d claim DENIED on machine %d; re-planning",
			     t, ev->id, ev->machine);
		break;
	case BRAIN_EV_PAY:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d paid %d to machine %d",
			     t, ev->id, ev->amount, ev->machine);
		break;
	case BRAIN_EV_LOAD:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d loading %.0fs in place at machine %d",
			     t, ev->id, (double)BRAIN_LOAD_SECS,
			     ev->machine);
		break;
	case BRAIN_EV_RUN_START:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d started machine %d; walking away while it runs",
			     t, ev->id, ev->machine);
		break;
	case BRAIN_EV_MACHINE_DONE:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d: machine %d run done, walking back to collect",
			     t, ev->id, ev->machine);
		break;
	case BRAIN_EV_INTERRUPTED:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d: machine %d BROKE mid-run; re-planning (garment %s)",
			     t, ev->id, ev->machine,
			     garmentStateName(ev->garment));
		break;
	case BRAIN_EV_COLLECT:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d collecting %.0fs in place at machine %d",
			     t, ev->id, (double)BRAIN_COLLECT_SECS,
			     ev->machine);
		break;
	case BRAIN_EV_GARMENT:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d garment is now %s",
			     t, ev->id, garmentStateName(ev->garment));
		break;
	case BRAIN_EV_ABANDON:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d abandoning the goal (garment %s)",
			     t, ev->id, garmentStateName(ev->garment));
		break;
	case BRAIN_EV_WAIT_TIMEOUT:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d wait timed out on machine %d; re-planning",
			     t, ev->id, ev->machine);
		break;
	case BRAIN_EV_LEAVE_BEGIN:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d heading for the exit",
			     t, ev->id);
		break;
	case BRAIN_EV_DEPART:
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] customer #%d left (%s)",
			     t, ev->id,
			     ev->garment == GARMENT_DRY_CLEAN
				     ? "clean dry"
				     : garmentStateName(ev->garment));
		break;
	default:
		break;
	}
}

/* Spawn the scripted divergence pair — a `closest` and a `spread_k` customer
 * together — and start the interval spawner. Called once, when washer 0's
 * first LOAD shows a cluster machine occupied: the closest customer takes the
 * next free cluster washer, the spread_k customer (penalising candidates within
 * K=2 of the occupied one) takes the isolated washer, and the next spawner
 * arrival is a `cheapest` customer so all three policies appear. */
static void levelSpawnDivergence(LevelState *st)
{
	BrainEvent ev[8];
	int n = 0;
	int k;

	(void)simSpawnCustomer(&st->sim, SEL_POLICY_CLOSEST, BRAIN_START_MONEY,
			       ev, 8, &n);
	(void)simSpawnCustomer(&st->sim, SEL_POLICY_SPREAD_K, BRAIN_START_MONEY,
			       ev, 8, &n);
	st->divergenceSpawned = true;
	st->sim.spawnCursor = SEL_POLICY_CHEAPEST;
	simSetSpawner(&st->sim, true, BRAIN_SPAWN_INTERVAL, BRAIN_SPAWN_CAP);
	SDL_Log("isomata: [t=%llums] scripted divergence pair (closest + spread_k) arrived; interval spawner on (%.0fs interval, cap %d, door (%d,%d))",
		levelMs(), (double)BRAIN_SPAWN_INTERVAL, BRAIN_SPAWN_CAP,
		LEVEL_DOOR_X, LEVEL_DOOR_Z);
	for (k = 0; k < n; k++)
		levelLogBrainEvent(&ev[k]);
}

/* Advance the customer world: the spawner, the machine timers/wakes, every
 * customer brain, the scripted divergence pair, the scripted washer-0
 * break/repair, and the brain/wake log stream. */
static void levelUpdateBrain(LevelState *st, float dt)
{
	int n = simUpdate(&st->sim, dt, st->events, BRAIN_MAX_EVENTS);
	bool spawn_divergence = false;
	int i;

	for (i = 0; i < n; i++) {
		const BrainEvent *ev = &st->events[i];

		if (!st->divergenceSpawned && ev->kind == BRAIN_EV_LOAD &&
		    ev->machine == LEVEL_SCRIPT_BREAK_MACHINE)
			spawn_divergence = true;
		if (ev->kind == BRAIN_EV_RUN_START &&
		    ev->machine == LEVEL_SCRIPT_BREAK_MACHINE) {
			st->washer0Runs++;
			if (st->washer0Runs == 2)
				st->breakTimer = LEVEL_SCRIPT_BREAK_DELAY;
		}
		levelLogBrainEvent(ev);
	}
	/* Spawn after the loop: spawning appends events and must not clobber
	 * the frame's event array while it is being walked. */
	if (spawn_divergence)
		levelSpawnDivergence(st);
	for (i = 0; i < st->sim.wakeCount; i++)
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: [t=%llums] machine %d wake %s owner %d",
			     levelMs(), st->sim.wakes[i].machine,
			     machineWakeKindName(st->sim.wakes[i].kind),
			     st->sim.wakes[i].owner);
	if (st->breakTimer > 0.0f) {
		st->breakTimer -= dt;
		if (st->breakTimer <= 0.0f) {
			if (simBreakMachine(&st->sim,
					    LEVEL_SCRIPT_BREAK_MACHINE))
				SDL_Log("isomata: [t=%llums] SCRIPTED BREAK: washer %d broke mid-run",
					levelMs(), LEVEL_SCRIPT_BREAK_MACHINE);
			st->repairTimer = LEVEL_SCRIPT_REPAIR_SECS;
			st->breakTimer = -1.0f;
		}
	}
	if (st->repairTimer > 0.0f) {
		st->repairTimer -= dt;
		if (st->repairTimer <= 0.0f) {
			Machine *m = machineGet(&st->sim.machines,
						LEVEL_SCRIPT_BREAK_MACHINE);

			if (m != NULL && machineRepair(m))
				SDL_Log("isomata: [t=%llums] SCRIPTED REPAIR: washer %d free again",
					levelMs(), LEVEL_SCRIPT_BREAK_MACHINE);
			st->repairTimer = -1.0f;
		}
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
	/* Advance the demo critters: the agent tick decides, claims and routes
	 * each body (ISO_LOG=debug shows the bands and the margin behind every
	 * move). */
	levelUpdateAgents(st, dt);
	/* Advance the T23 customers + machines: spawner, machine timers and
	 * wakes, each customer's brain, and the scripted break/repair. */
	levelUpdateBrain(st, dt);
	/* Advance every walker once, after all decisions are in for the
	 * frame. */
	entitiesUpdate(&st->sim.entities, dt);
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
	int n = entitiesDebugMarkers(&st->sim.entities, st->markers,
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
