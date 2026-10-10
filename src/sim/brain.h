#ifndef ISOMATA_SIM_BRAIN_H
#define ISOMATA_SIM_BRAIN_H

/*
 * The customer brain (T23 layer 3): goals, plan execution, machine selection,
 * payment and the customer lifecycle — plus the SimWorld that owns the entity
 * and machine registries the customers live in.
 *
 * Pure: no SDL, no allocation, no time source. The whole world advances only
 * by the caller's dt (clamped to BRAIN_MAX_DT), so an integration scenario
 * stepped with a fixed fake dt is deterministic and replayable.
 *
 * EXECUTOR WIRING (the T21 FSM drives decisions, this glue executes them).
 * One USE_MACHINE plan task runs as two FSM legs:
 *   LOAD leg     SELECT -> fsmStart(machine tile) [MOVE_TO] -> MOVING_TO
 *                ARRIVED -> ACTING: claim the machine (a refusal -> DENIED ->
 *                re-plan), pay the fee, LOAD 5 s in place -> machineStartRun
 *                -> PHASE_DONE -> WAITING.
 *   RUN_WAIT     the customer walks to the wait spot ("owner away") and idles
 *                until the machine's PHASE_DONE wake arrives; then a fresh
 *                fsmStart(machine) begins the collectors' walk back.
 *   COLLECT leg  ARRIVED -> COLLECT 5 s -> machineRelease + the garment
 *                transition -> a zero-length WAITING closes the task (DONE).
 * A machine that breaks mid-run delivers an INTERRUPTED wake -> the leg ends
 * with the garment untouched and the task re-plans (broken machines are not
 * FREE, so selection excludes them; the failed machine is blacklisted too).
 *
 * FAILURE / WAIT POLICY. No free candidate -> a bounded retry wait
 * (BRAIN_WAIT_RETRY_SECS x BRAIN_MAX_WAIT_RETRIES); still none -> the goal is
 * abandoned and the customer leaves with whatever load it has (DIRTY at the
 * first task), with an honest BRAIN_EV_ABANDON. Insufficient funds at the PAY
 * step abandons the same way. A machine-run wait is bounded by the run length
 * plus BRAIN_MACHINE_WAIT_SLACK; on expiry the machine is released and the task
 * re-plans.
 *
 * EVENTS. The brain writes a bounded stream of BrainEvent records (capacity
 * `cap`, a deterministic prefix when more happen). The level turns them into
 * SDL_Log lines; the acceptance suite asserts their exact order. Machines'
 * rows are not re-derived here — the level's wake loop still logs them.
 */

#include "ai/fsm.h"
#include "entities/entities.h"
#include "sim/garments.h"
#include "sim/machines.h"
#include "sim/plan.h"
#include "sim/selection.h"

#include <stdbool.h>

/* Customers alive at once (the entity registry is larger; the T21 demo critters
 * share it). */
#define BRAIN_MAX_CUSTOMERS 8
/* Machines one task instance may blacklist after failures. */
#define BRAIN_MAX_FAILED 8
/* Wakes consumed per simUpdate (a bounded prefix if more complete). */
#define BRAIN_MAX_WAKES 16
/* Events emitted per simUpdate (a bounded prefix if more happen). */
#define BRAIN_MAX_EVENTS 128
/* simUpdate clamps a frame's dt to this, matching the entity/machine walkers. */
#define BRAIN_MAX_DT 0.1f

/* Activity step ids (the FSM BEGIN_ACTIVITY / ACTING sub-step). */
#define BRAIN_STEP_PAY 0
#define BRAIN_STEP_LOAD 1
#define BRAIN_STEP_COLLECT 2

/* WAIT reasons (the FSM WAIT payload). */
#define BRAIN_WAIT_MACHINE 0	/* waiting for a machine run to finish */
#define BRAIN_WAIT_TASK_END 1	/* zero-length end-of-task wait */
#define BRAIN_WAIT_NO_CANDIDATE 2 /* no free machine; retrying */

/* Load / collect durations (seconds, in place). */
#define BRAIN_LOAD_SECS 5.0f
#define BRAIN_COLLECT_SECS 5.0f
/* No-candidate retry: wait this long, retry, at most this many times. */
#define BRAIN_WAIT_RETRY_SECS 1.0f
#define BRAIN_MAX_WAIT_RETRIES 120
/* A machine-run wait is bounded by the run length plus this slack. */
#define BRAIN_MACHINE_WAIT_SLACK 60.0f

/* Spawner defaults. */
#define BRAIN_START_MONEY 20
#define BRAIN_SPAWN_INTERVAL 4.0f
#define BRAIN_SPAWN_CAP 4
/* Customer walk speed (world units / second). */
#define BRAIN_CUSTOMER_SPEED 2.5f

/* The glue-level task-leg phase. (The FSM's own state is the executor view;
 * this is what the glue is doing within the current plan task.) */
typedef enum BrainPhase {
	BRAIN_PH_INACTIVE = 0,	/* slot unused */
	BRAIN_PH_SELECT,	/* choose a machine for the current task */
	BRAIN_PH_NO_CANDIDATE,	/* no free machine: bounded retry wait */
	BRAIN_PH_APPROACH,	/* FSM MOVING_TO the bound machine */
	BRAIN_PH_ACT,		/* FSM ACTING (pay+load, or collect) */
	BRAIN_PH_RUN_WAIT,	/* FSM WAITING for the machine run */
	BRAIN_PH_TASK_END,	/* zero-length WAITING that closes a task */
	BRAIN_PH_LEAVE,		/* LEAVE task: walking to the exit */
	BRAIN_PH_DONE,		/* departed (reclaimed this update) */
} BrainPhase;

/* The two FSM legs of one USE_MACHINE task. */
typedef enum BrainLeg {
	BRAIN_LEG_LOAD = 0,	/* approach + claim + pay + load + run wait */
	BRAIN_LEG_COLLECT = 1,	/* walk back + collect + release */
} BrainLeg;

typedef struct SimCustomer {
	EntityHandle handle;
	Fsm fsm;
	int id;			/* monotonic spawn id (logs) */
	int policy;		/* SelPolicy */
	int garment;		/* GarmentState */
	Plan plan;
	int taskIndex;		/* current plan task */
	int phase;		/* BrainPhase */
	int leg;		/* BrainLeg, valid during USE_MACHINE */
	int actStep;		/* BRAIN_STEP_* during BRAIN_PH_ACT */
	MachineHandle target;	/* bound / attempted machine */
	MachineHandle failed[BRAIN_MAX_FAILED];
	int failedCount;
	int waitRetries;	/* no-candidate retries for the current task */
	float timer;		/* activity / retry countdown */
	float waitBudget;	/* machine-run wait countdown */
	int pendingWake;	/* wake delivered this frame, or -1 */
	bool wantDespawn;	/* leave: despawn after this step */
	bool abandoned;		/* gave up the goal (leaves as-is) */
} SimCustomer;

typedef enum BrainEventKind {
	BRAIN_EV_SPAWN = 0,	/* a customer arrived at the door */
	BRAIN_EV_TASK_BEGIN,	/* a plan task's machine was chosen */
	BRAIN_EV_NO_CANDIDATE,	/* no free machine (a bounded retry) */
	BRAIN_EV_CLAIM_DENIED,	/* claim refused at arrival -> re-plan */
	BRAIN_EV_PAY,		/* fee debited */
	BRAIN_EV_LOAD,		/* load-in-place started */
	BRAIN_EV_RUN_START,	/* machineStartRun */
	BRAIN_EV_MACHINE_DONE,	/* PHASE_DONE wake consumed */
	BRAIN_EV_INTERRUPTED,	/* INTERRUPTED wake consumed (broke) */
	BRAIN_EV_COLLECT,	/* collect-in-place started */
	BRAIN_EV_GARMENT,	/* garment transition applied */
	BRAIN_EV_ABANDON,	/* gave up (no funds / no machine) */
	BRAIN_EV_WAIT_TIMEOUT,	/* machine-run wait budget exhausted */
	BRAIN_EV_LEAVE_BEGIN,	/* heading to the exit */
	BRAIN_EV_DEPART,	/* left the building */
	BRAIN_EV_COUNT,
} BrainEventKind;

typedef struct BrainEvent {
	int kind;		/* BrainEventKind */
	EntityHandle customer;
	int id;			/* spawn id */
	int policy;
	MachineHandle machine;
	int amount;		/* fee paid (PAY) / start funds (SPAWN) */
	int score;		/* winning heuristic score (TASK_BEGIN) */
	int distance;		/* tile distance to the machine (TASK_BEGIN) */
	int garment;		/* garment state at the event */
} BrainEvent;

typedef struct SimWorld {
	EntityRegistry entities;
	MachineRegistry machines;
	SimCustomer customers[BRAIN_MAX_CUSTOMERS];
	int customerCount;
	const Voxmap *map;
	int doorX, doorZ;	/* where customers arrive */
	int exitX, exitZ;	/* where they leave (and reach the goal) */
	int waitX, waitZ;	/* where they park during a run ("owner away") */
	bool spawnEnabled;	/* interval spawner on/off */
	float spawnInterval;
	float spawnTimer;
	int spawnCap;
	int spawnCursor;	/* spawner policy-mix cursor */
	int nextId;		/* monotonic spawn id */
	MachineWake wakes[BRAIN_MAX_WAKES];
	int wakeCount;		/* wakes produced by the last simUpdate */
	/* Customer billboard spec (set by the level; applied on spawn). */
	float customerWidth;
	float customerHeight;
	int16_t customerMaterial;
	uint32_t policyTint[SEL_POLICY_COUNT];	/* per-policy tint */
	SelWorld select;	/* built over `machines` */
} SimWorld;

/* Reset the world: bind `map`, record the door / exit / wait tiles, empty both
 * registries. Machines are added afterwards with machineCreate on
 * `w->machines`. NULL `w` is a no-op. */
void simWorldInit(SimWorld *w, const Voxmap *map, int doorX, int doorZ,
		  int exitX, int exitZ, int waitX, int waitZ);

/* Configure the interval spawner (rate + alive cap) and reset its timer so the
 * first customer arrives on the next update when enabled. NULL-safe. */
void simSetSpawner(SimWorld *w, bool enabled, float interval, int cap);

/* Spawn one customer at the door with policy `policy` and `money` (the funds
 * live on the entity's `money` field), starting LEAVE_WITH_CLEAN_DRY. Appends
 * events to `out` (capacity `cap`, count in `*nOut`). Returns the entity
 * handle, or ENTITY_INVALID when the customer cap is reached / the world is
 * NULL / the door tile is void. */
EntityHandle simSpawnCustomer(SimWorld *w, int policy, int money,
			      BrainEvent *out, int cap, int *nOut);

/* Break machine `h` (any state -> BROKEN) and deliver the INTERRUPTED wake to
 * the owning customer (if any) so the next simUpdate re-plans it. Returns true
 * when the machine was broken. The regular PHASE_DONE wake path stays inside
 * simUpdate; this exists for the level's scripted demo break. */
bool simBreakMachine(SimWorld *w, MachineHandle h);

/* Advance the world by `dt` (clamped to [0, BRAIN_MAX_DT]): run the spawner,
 * advance the machines and collect wakes, deliver each wake to its owner's
 * customer, step every customer brain, then reclaim departed customers. Writes
 * up to `cap` events to `out` and returns the number written. The caller
 * advances entity movement afterwards with entitiesUpdate(&w->entities, dt),
 * kept separate so the level steps the shared registry (critters + customers)
 * exactly once. Returns 0 for a NULL world. */
int simUpdate(SimWorld *w, float dt, BrainEvent *out, int cap);

/* Live customer count (0 for NULL). */
int simCustomerCount(const SimWorld *w);

/* The customer owning `h`, or NULL. */
const SimCustomer *simCustomerFor(const SimWorld *w, EntityHandle h);

/* Stable event log name: "SPAWN"/"TASK_BEGIN"/... ; "?" for an unknown kind. */
const char *brainEventName(int kind);

#endif /* ISOMATA_SIM_BRAIN_H */
