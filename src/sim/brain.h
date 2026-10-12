#ifndef ISOMATA_SIM_BRAIN_H
#define ISOMATA_SIM_BRAIN_H

/*
 * The customer driver: the world's half of a customer running an ERRAND, plus
 * the SimWorld that owns the entity, machine and agent registries everybody
 * lives in.
 *
 * WHAT MOVED INTO THE FRAMEWORK (task 5). The customer's old BrainPhase
 * switch — SELECT / APPROACH / ACT / RUN_WAIT / TASK_END / LEAVE — is gone,
 * and so is the T21 executor FSM it used to shadow. Both are now data and
 * jobs: "wash, then dry, then leave" is the ERR_LAUNDRY_CLEAN template
 * (agents/errand.h) and every step of it is a job on the agent's own
 * personal board, arbitrated by the want ladder (agents/agent.h). This file
 * is what is LEFT, and it is exactly the half no framework module may hold:
 *
 *   - WHICH machine (src/sim/selection.c's heuristics, untouched);
 *   - the machine's claim, run and release (src/sim/machines.h — a claimed
 *     machine IS a claimed thing, so the job's claim and the machine's claim
 *     are one lifetime, not two models);
 *   - the fee, debited from the body at the load step;
 *   - the load's own progression (src/sim/garments.h);
 *   - the door, and the event stream the level turns into log lines.
 *
 * THE BOUNDED WAIT IS A JOB'S, NOT A CLOCK'S. A machine run used to be waited
 * on with a local countdown (the old waitBudget). It is now the errand's
 * STEP_GATE_WAIT_MACHINE gate, whose deadline is carried into the wait job's
 * WAIT, so an expired bound FAILS the job through the lifecycle's JOB_TIMEOUT
 * — the producer that cell had been waiting for. A machine that never
 * delivers its phase wake therefore re-plans instead of hanging the customer,
 * and the release of its claim is the same code path every other failure
 * takes.
 *
 * THE RELEASE RULE, in one line because everything else here depends on it: a
 * customer never leaves the building still holding somebody else's washing.
 * brain.c releases a machine in three places — brainReplan (every failure, the
 * walk out, and a body or mind that vanishes), the cannot-pay hand-back, and
 * the collect leg's normal completion — and every path that can END while
 * holding one goes through one of them. The suites assert that on every tick of
 * every scenario, and pin the two paths that used to be exceptions.
 *
 * SATISFACTION AND THE WALK OUT. Blocked time decays NEED_SATISFACTION (the
 * spec's tier-1 reading: being blocked is an inconvenience), and an errand
 * that has been blocked for ERRAND_LEAVE_THRESHOLD_WINDOW with satisfaction
 * below ERRAND_LEAVE_THRESHOLD is abandoned and the customer walks to the door
 * — the "takes their business elsewhere" loop. Abandoning RELEASES: whatever
 * the abandoned step held goes back before the walk out, so a machine can
 * never be left claimed by somebody who has left the building.
 *
 * Pure: no SDL, no allocation, no time source. The whole world advances only
 * by the caller's dt (clamped to BRAIN_MAX_DT), so an integration scenario
 * stepped with a fixed fake dt is deterministic and replayable.
 *
 * One USE_MACHINE errand step, in the phases the framework's lifecycle already
 * has:
 *   the step's job is enqueued -> WALKING (agentsUpdate routes the body to
 *   the machine) -> on arrival the driver CLAIMS the machine, PAYS the fee and
 *   begins the LOAD -> the job's own WAIT is the load, and the driver starts
 *   the run when it ends -> the errand's GATE holds the bounded wait for the
 *   machine's PHASE_DONE -> a COLLECT step walks back, takes the load out and
 *   releases the machine, which is what advances the garment.
 */

#include "agents/agent.h"
#include "entities/entities.h"
#include "sim/garments.h"
#include "sim/machines.h"
#include "sim/selection.h"

#include <stdbool.h>

/* Customers alive at once (the entity registry is larger; the T21 demo critters
 * share it). */
#define BRAIN_MAX_CUSTOMERS 8
/* Wakes consumed per simUpdate (a bounded prefix if more complete). */
#define BRAIN_MAX_WAKES 16
/* Events emitted per simUpdate (a bounded prefix if more happen). */
#define BRAIN_MAX_EVENTS 128
/* simUpdate clamps a frame's dt to this, matching the entity/machine walkers
 * and agentsUpdate's own clamp. */
#define BRAIN_MAX_DT 0.1f

/* The in-place sub-step inside a USE_MACHINE step. The errand names WHAT the
 * step is (use a machine); these are the world's own phases of doing it, and
 * the one that matters is the claim, which is why the state is per customer
 * rather than on the errand. */
#define BRAIN_STEP_NONE 0
#define BRAIN_STEP_PAY 1	/* claim + pay have not happened yet */
#define BRAIN_STEP_LOAD 2	/* claimed, paid, loading */
#define BRAIN_STEP_COLLECT 3	/* at the machine, taking the load out */

/* Load / collect durations (seconds, in place). */
#define BRAIN_LOAD_SECS 5.0f
#define BRAIN_COLLECT_SECS 5.0f
/* No-candidate retry: wait this long, retry, at most this many times. The
 * errand's own leave window (ERRAND_LEAVE_THRESHOLD_WINDOW) is the bound that
 * normally fires first; this is the belt-and-braces cap on a world that never
 * frees a machine. */
#define BRAIN_WAIT_RETRY_SECS 1.0f
#define BRAIN_MAX_WAIT_RETRIES 120
/* A machine-run wait is bounded by the run length plus this slack. This is the
 * gate deadline handed to the errand, which carries it into the wait job, so
 * a machine that never delivers its wake fails that job (JOB_TIMEOUT) instead
 * of hanging the customer. The slack is kept shorter than an errand's leave
 * window (ERRAND_LEAVE_THRESHOLD_WINDOW) so the bounded wait always ends
 * BEFORE the customer walks out — see the note there. */
#define BRAIN_MACHINE_WAIT_SLACK 30.0f

/* Spawner defaults. */
#define BRAIN_START_MONEY 20
#define BRAIN_SPAWN_INTERVAL 4.0f
#define BRAIN_SPAWN_CAP 4
/* Customer walk speed (world units / second). */
#define BRAIN_CUSTOMER_SPEED 2.5f

/* WHICH HALF OF THE ERRAND THE CUSTOMER IS IN. Not a plan and not a
 * BrainPhase: the errand's step index says this, and this enum exists only so
 * the driver can name the phase in a log line and pick the sub-step it runs. */
typedef enum BrainLeg {
	BRAIN_LEG_NONE = 0,	/* no errand step in flight */
	BRAIN_LEG_LOAD,		/* USE_MACHINE: claim, pay, load, start the run */
	BRAIN_LEG_GATE,		/* the machine's run: the errand's gate */
	BRAIN_LEG_COLLECT,	/* COLLECT: walk back, take the load, release */
	BRAIN_LEG_LEAVE		/* LEAVE: walking to the exit */
} BrainLeg;

typedef struct SimCustomer {
	EntityHandle handle;
	/* THE MIND. The customer is an Agent (agents/agent.h) and its errand
	 * lives in that record; this is only the link back, because the level
	 * still wants to look a customer up by its entity handle. */
	AgentHandle agent;
	int id;			/* monotonic spawn id (logs) */
	int policy;		/* SelPolicy */
	int garment;		/* GarmentState */
	MachineHandle target;	/* the machine the current step is about */
	int leg;		/* BrainLeg, derived from the errand's step */
	int actStep;		/* BRAIN_STEP_* inside a USE_MACHINE step */
	int leaveJob;		/* the job walking to the exit, or -1 */
	int pendingWake;	/* wake delivered this frame, or -1 */
	int waitRetries;	/* no-candidate retries for the current step */
	float timer;		/* retry countdown */
	/* The world-clock second at which the machine currently running for
	 * this customer stops being worth waiting for (0 when none is). The
	 * errand carries the same bound as the gate job's DURATION; this is the
	 * deadline itself, measured from the moment the run started, and it is
	 * what fires JOB_TIMEOUT on the wait job — the framework's producer for
	 * a phase that never completes. */
	float gateDeadline;
	bool wantDespawn;	/* leave: despawn after this step */
	bool abandoned;		/* gave up the errand (leaves as-is) */
	bool reported;		/* the errand's terminal state was acted on */
	bool reportedFill;	/* the current secondary activity was announced */
} SimCustomer;

typedef enum BrainEventKind {
	BRAIN_EV_SPAWN = 0,	/* a customer arrived at the door */
	BRAIN_EV_ERRAND_ADOPT,	/* it adopted an errand, and at what band */
	BRAIN_EV_FILL,		/* blocked: doing something of its own accord */
	BRAIN_EV_TASK_BEGIN,	/* a step's machine was chosen */
	BRAIN_EV_NO_CANDIDATE,	/* no free machine (a bounded retry) */
	BRAIN_EV_CLAIM_DENIED,	/* claim refused at arrival -> re-plan */
	BRAIN_EV_PAY,		/* fee debited */
	BRAIN_EV_LOAD,		/* load-in-place started */
	BRAIN_EV_RUN_START,	/* machineStartRun */
	BRAIN_EV_MACHINE_DONE,	/* PHASE_DONE wake consumed */
	BRAIN_EV_INTERRUPTED,	/* INTERRUPTED wake consumed (broke) */
	BRAIN_EV_REPLAN,	/* the step's job failed: released, re-selecting */
	BRAIN_EV_COLLECT,	/* collect-in-place started */
	BRAIN_EV_GARMENT,	/* garment transition applied */
	BRAIN_EV_ABANDON,	/* gave up the errand and will walk out */
	BRAIN_EV_WAIT_TIMEOUT,	/* the gate's bound expired */
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
	int amount;		/* fee paid (PAY) / start funds (SPAWN) / the
				 * activity it filled time with (FILL) */
	int score;		/* winning heuristic score (TASK_BEGIN) */
	int distance;		/* tile distance to the machine (TASK_BEGIN) */
	int garment;		/* garment state at the event */
	/* The errand an adoption named, and the band that chose it. These two
	 * are what makes "why is this customer doing that" a log line rather
	 * than archaeology (the spec's why-this-job artifact). */
	int tmpl;
	int band;
} BrainEvent;

typedef struct SimWorld {
	EntityRegistry entities;
	MachineRegistry machines;
	/* The customers' MINDS, plus the one board they share. Every customer
	 * is an agent, so this file can put its errand's jobs on the personal
	 * board embedded in the Agent record and have agentsUpdate run them. */
	AgentRegistry agents;
	AgentBoards boards;
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
	/* Seconds of ticked dt the world has lived: the board clock the
	 * reservations are swept against (agents/board.h), accumulated from
	 * the caller's dt and never a wall clock. */
	float now;
	MachineWake wakes[BRAIN_MAX_WAKES];
	int wakeCount;		/* wakes produced by the last simUpdate */
	/* Customer billboard spec (set by the level; applied on spawn). */
	float customerWidth;
	float customerHeight;
	int16_t customerMaterial;
	uint32_t policyTint[SEL_POLICY_COUNT];	/* per-policy tint */
	SelWorld select;	/* built over `machines` */
} SimWorld;

/* Reset the world: bind `map`, record the door / exit / wait tiles, empty all
 * three registries. Machines are added afterwards with machineCreate on
 * `w->machines`. NULL `w` is a no-op. */
void simWorldInit(SimWorld *w, const Voxmap *map, int doorX, int doorZ,
		  int exitX, int exitZ, int waitX, int waitZ);

/* Configure the interval spawner (rate + alive cap) and reset its timer so the
 * first customer arrives on the next update when enabled. NULL-safe. */
void simSetSpawner(SimWorld *w, bool enabled, float interval, int cap);

/* Spawn one customer at the door with policy `policy` and `money` (the funds
 * live on the entity's `money` field). The customer is an agent, and adopts
 * its first errand by need (agents/errand.h) before this returns — the
 * adoption is what makes its first job exist, and BRAIN_EV_ERRAND_ADOPT says
 * so. Appends events to `out` (capacity `cap`, count in `*nOut`). Returns the
 * entity handle, or ENTITY_INVALID when the customer cap is reached / the
 * world is NULL / the door tile is void. */
EntityHandle simSpawnCustomer(SimWorld *w, int policy, int money,
			      BrainEvent *out, int cap, int *nOut);

/* Break machine `h` (any state -> BROKEN) and deliver the INTERRUPTED wake to
 * the owning customer (if any) so the next simUpdate re-plans it. Returns true
 * when the machine was broken. The regular PHASE_DONE wake path stays inside
 * simUpdate; this exists for the level's scripted demo break. */
bool simBreakMachine(SimWorld *w, MachineHandle h);

/* Advance the world by `dt` (clamped to [0, BRAIN_MAX_DT]): run the spawner,
 * sweep the boards' reservations, advance the machines and collect wakes,
 * deliver each wake to its owner's customer, run the agent tick
 * (agentsUpdate — decisions and the job lifecycle), then drive every customer's
 * errand: the world's effects on the step in flight, the errand's own advance,
 * the blocked-time satisfaction decay and the walk out. Writes up to `cap`
 * events to `out` and returns the number written. The caller advances entity
 * movement afterwards with entitiesUpdate(&w->entities, dt), kept separate so
 * the level steps the shared registry (critters + customers) exactly once.
 * Returns 0 for a NULL world. */
int simUpdate(SimWorld *w, float dt, BrainEvent *out, int cap);

/* Live customer count (0 for NULL). */
int simCustomerCount(const SimWorld *w);

/* The customer owning `h`, or NULL. */
const SimCustomer *simCustomerFor(const SimWorld *w, EntityHandle h);

/* The errand a customer is on right now, or NULL: the live errand record for
 * `h`, which is what a debug overlay reads (template id, step, blocked time).
 * The returned pointer is into the agent registry and is invalidated by the
 * next simUpdate. */
const Errand *simCustomerErrand(const SimWorld *w, EntityHandle h);

/* Stable event log name: "SPAWN"/"TASK_BEGIN"/... ; "?" for an unknown kind. */
const char *brainEventName(int kind);

#endif /* ISOMATA_SIM_BRAIN_H */
