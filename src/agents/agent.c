/*
 * The agent record + flat-slot registry + the tick that decides (see
 * agents/agent.h for the model).
 *
 * Mirrors EntityRegistry in src/entities/entities.c on purpose: the same
 * fixed-capacity slots + alive[] + liveCount shape, the same handle guards,
 * the same refusal-without-a-diagnostic style at capacity (the caller reads
 * AGENT_INVALID; there is no logging in a pure module).
 *
 * Pure: no SDL, no allocation, no time source. The only process-wide state is
 * g_stats, the counter pair agentsStats drains — no decision reads it.
 */

#include "agents/agent.h"

#include "ai/nav.h"

#include <string.h>

/* The perf counters agentsUpdate feeds and agentsStats drains. Process-wide
 * because a phase reports deltas for the whole frame, not per agent; kept out
 * of Agent so the registry stays a pure function of its own inputs. */
static AgentStats g_stats;

/* The per-role curve table. A role is a ROW: adding one is a value here plus
 * an AgentRole constant, and no call site changes. Order matches Need.
 *
 * panicThreshold is -1.0f on every non-survival slot: panic is reserved for
 * tier-0 survival (see agents/need.h), so the only knob a role actually has
 * here is WHERE an emptied survival need trips its alarm. */
static const RoleProfile g_roleProfiles[ROLE_COUNT] = {
	/* CUSTOMER: the baseline. Base decay rate; survival trips at 0.15. */
	[ROLE_CUSTOMER] = {
		.decayMul = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		.panicThreshold = { 0.15f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f },
	},
	/* STAFF: a shift tires you (energy 1.5), and someone is always around
	 * plus the job itself supplies the sense of purpose (social and
	 * fulfilment 0.5). Same survival threshold as the customer. */
	[ROLE_STAFF] = {
		.decayMul = { 1.0f, 1.5f, 0.5f, 1.0f, 1.0f, 0.5f },
		.panicThreshold = { 0.15f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f },
	},
	/* CRITTER: needs little company (social 0.25), is not bothered by
	 * esteem at all (0.0 — it never decays, so it can never panic), and is
	 * less annoyed by being blocked. It trips EARLIER on survival, 0.10:
	 * a critter reacts to danger sooner than a customer does. */
	[ROLE_CRITTER] = {
		.decayMul = { 1.0f, 1.0f, 0.25f, 0.5f, 0.0f, 1.0f },
		.panicThreshold = { 0.10f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f },
	},
};

void agentsInit(AgentRegistry *r)
{
	if (r == NULL)
		return;
	/* Zeroes the slots, alive[] and liveCount as well as nextId, so a
	 * re-initialised registry is byte-identical to a fresh one. */
	memset(r, 0, sizeof(*r));
}

AgentHandle agentCreate(AgentRegistry *r, int role)
{
	int i;

	if (r == NULL)
		return AGENT_INVALID;
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a;

		if (r->alive[i])
			continue;
		a = &r->slots[i];
		/* Scrub first: a reused slot must never show the previous
		 * occupant's state, even for a field nobody set. */
		*a = (Agent){ 0 };
		a->id = r->nextId++;
		a->role = role;
		a->alive = true;
		needSetInit(&a->needs);
		a->panicMask = needPanicMask(&a->needs);
		a->entity = ENTITY_INVALID;
		boardInit(&a->personal, false);
		a->engagedJob = JOB_INVALID;
		a->engagedShared = false;
		a->dwellLeft = 0.0f;
		a->pathLen = -1;
		r->alive[i] = true;
		r->liveCount++;
		return i;
	}
	return AGENT_INVALID;	/* full */
}

Agent *agentGet(AgentRegistry *r, AgentHandle h)
{
	if (r == NULL || h < 0 || h >= AGENT_MAX || !r->alive[h])
		return NULL;
	return &r->slots[h];
}

void agentDestroy(AgentRegistry *r, AgentHandle h)
{
	if (r == NULL || h < 0 || h >= AGENT_MAX || !r->alive[h])
		return;
	r->alive[h] = false;
	r->slots[h] = (Agent){ 0 };
	r->liveCount--;
}

void agentsDecay(AgentRegistry *r, float dt)
{
	int i;

	if (r == NULL)
		return;
	if (dt > AGENT_MAX_DT)
		dt = AGENT_MAX_DT;
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a = &r->slots[i];
		const RoleProfile *p;

		if (!r->alive[i])
			continue;
		/* Read the role's row once, here, and pass it down: the decay
		 * helpers are pure functions of (state, multipliers, dt) and
		 * know nothing about roles. A NULL row is a bad data row, not
		 * a reason to crash every agent in the registry — skip this
		 * agent's tick and leave it exactly as it was. */
		p = agentRoleProfile(a->role);
		if (p == NULL)
			continue;
		needDecay(&a->needs, p->decayMul, dt);
		needPanicDecayRole(&a->needs, p->panicThreshold, dt);
		a->panicMask = needPanicMask(&a->needs);
	}
}

const RoleProfile *agentRoleProfile(int role)
{
	if (role < 0 || role >= ROLE_COUNT)
		return NULL;
	return &g_roleProfiles[role];
}

void agentsStats(AgentStats *out)
{
	if (out == NULL)
		return;
	/* Read and reset in one step: a phase reads at its start and again
	 * at its end, and the delta between the two is what it reports. */
	*out = g_stats;
	memset(&g_stats, 0, sizeof(g_stats));
}

int agentShouldSwitch(const Agent *a, int engagedBand, int challengerBand,
		      float dwellLeft)
{
	/* No agent, nothing to switch. */
	if (a == NULL)
		return 0;
	/* Idle takes the best available: a dwell protects the job you HOLD,
	 * and an agent holding nothing has nothing to protect. */
	if (engagedBand == JOB_INVALID || challengerBand == JOB_INVALID)
		return 1;
	/* Staying power outranks urgency: however much better the challenger
	 * is, the agent finishes what it just started. */
	if (dwellLeft > 0.0f)
		return 0;
	/* Only a decisive gap moves a holding agent — one band better is
	 * noise, not a reason (agents/need.h bands two near-equal wants
	 * into the same integer for exactly this). */
	return challengerBand >= engagedBand + AGENT_PREEMPT_MARGIN ? 1 : 0;
}

/* The board an agent's engaged job lives on. */
static JobBoard *agentBoardOf(Agent *a, AgentBoards *boards)
{
	if (a->engagedShared)
		return boards != NULL ? &boards->shared : NULL;
	return &a->personal;
}

/* The best job for `a` across both boards, as the JOB itself plus the board
 * it sits on and the handle that names it: the caller therefore claims
 * exactly the job it chose, with no second lookup that could disagree. The
 * personal board is scanned first and WINS a band tie (it is the agent's own
 * work), so the answer is deterministic either way — provenance never
 * outranks a band (see agents/board.h). Returns NULL when there is nothing
 * to take. */
static Job *agentBestJob(Agent *a, AgentBoards *boards, const int *skip,
			 int *outHandle, JobBoard **outBoard)
{
	int personal = boardBestFor(&a->personal, skip, a->id);
	int shared = JOB_INVALID;
	int personalBand = JOB_INVALID;
	int sharedBand = JOB_INVALID;

	*outHandle = JOB_INVALID;
	*outBoard = &a->personal;
	if (boards != NULL) {
		shared = boardBestFor(&boards->shared, skip, a->id);
		sharedBand = (shared != JOB_INVALID)
				     ? boards->shared.slots[shared].band
				     : JOB_INVALID;
	}
	personalBand = (personal != JOB_INVALID)
			       ? a->personal.slots[personal].band
			       : JOB_INVALID;
	if (shared != JOB_INVALID &&
	    (personal == JOB_INVALID || sharedBand > personalBand)) {
		*outHandle = shared;
		*outBoard = &boards->shared;
		return &boards->shared.slots[shared];
	}
	if (personal == JOB_INVALID)
		return NULL;
	*outHandle = personal;
	return &a->personal.slots[personal];
}

/* Take a job and route the body to it: START claims the lifecycle, the
 * ARRIVED that follows is the cell that says "go there", and the target is
 * resolved and walked. Those two cells are pinned by the job suite
 * (UNCLAIMED + START -> CLAIMED, CLAIMED + ARRIVED -> WALKING / MOVE_TO), so
 * there is no action left to re-check here: what can still be refused is a
 * target that does not resolve, a tile the world has no floor for, a body
 * that is not reachable, and a route A* will not produce. Each of those
 * DENIES the job — the lifecycle fails it and the next tick releases the
 * claim — rather than leaving the agent holding work it can never run. */
static void agentDenyJob(Agent *a, Job *j)
{
	a->pathLen = -1;
	jobStep(j, JOB_DENIED);
}

static void agentStartJob(Agent *a, EntityRegistry *ents, Job *j,
			  const Voxmap *map)
{
	Entity *e = (ents != NULL) ? entityGet(ents, a->entity) : NULL;
	int tx = 0;
	int tz = 0;
	int len;

	jobStep(j, JOB_START);
	jobStep(j, JOB_ARRIVED);
	if (!jobTargetTile(j, VOXMAP_MAX_DIM, &tx, &tz)) {
		agentDenyJob(a, j);
		return;
	}
	/* The tile resolves inside the map's bounds; the map itself says
	 * whether there is ground to stand on (a NULL map says no). */
	if (voxmapIsVoid(map, tx, tz)) {
		agentDenyJob(a, j);
		return;
	}
	len = entityPathTo(e, tx, tz);
	g_stats.pathfindCalls++;
	a->pathLen = len;
	if (len < 0)
		jobStep(j, JOB_DENIED);
}

/* The execute half for a job the agent already holds. Arrival is the body's
 * business: the whole route walked (no active segment and an empty queue) is
 * what "I am there" means, and it is the only place that says so. */
static void agentAdvanceJob(Agent *a, EntityRegistry *ents, Job *j, float dt)
{
	Entity *e = (ents != NULL) ? entityGet(ents, a->entity) : NULL;

	switch (j->state) {
	case JOB_WALKING:
		/* A body that has gone (destroyed mid-walk) never arrives:
		 * completing a walk nobody made would be a lie, so the job
		 * is denied and the claim released next tick. */
		if (e == NULL) {
			jobStep(j, JOB_DENIED);
			break;
		}
		if (e->moving || e->pathCount > 0)
			break;	/* still walking */
		/* Arrived: bind the activity. */
		jobStep(j, JOB_ARRIVED);
		break;
	case JOB_INTERACTING:
		/* The demo's activity has no runtime of its own, so its step
		 * completes on the tick it is entered and the job parks on
		 * `duration` as a timed WAIT; the activity catalogue (task
		 * 4) gives this state a real runtime. */
		jobStep(j, JOB_PHASE_DONE);
		j->timer = j->duration;
		break;
	case JOB_WAITING:
		j->timer -= dt;
		if (j->timer <= 0.0f) {
			j->timer = 0.0f;
			jobStep(j, JOB_PHASE_DONE);
		}
		break;
	default:
		/* UNCLAIMED and CLAIMED cannot reach here (the tick claims and
		 * starts a job in the same breath), and DONE/FAILED are
		 * retired before the decision. */
		break;
	}
}

/* Fail and release whatever the agent was running. Preemption releases held
 * resources: the job goes back to the board as a dead row rather than sitting
 * claimed, and it is never RETARGETED — re-planning is a new job. */
static void agentDropJob(Agent *a, JobBoard *b, Job *j)
{
	jobStep(j, JOB_INTERRUPTED);
	boardRelease(b, a->engagedJob);
	a->engagedJob = JOB_INVALID;
	a->engagedShared = false;
}

int agentsUpdate(AgentRegistry *r, EntityRegistry *ents, AgentBoards *boards,
		 const Voxmap *map, float dt, int *outDecisions)
{
	int decisions = 0;
	int i;

	if (r == NULL)
		return 0;
	/* NaN-safe clamp, the same shape as entitiesUpdate's. */
	if (!(dt > 0.0f))
		dt = 0.0f;
	else if (dt > AGENT_MAX_DT)
		dt = AGENT_MAX_DT;

	/* Step 1: needs fall, so the bands the decision reads are fresh. */
	agentsDecay(r, dt);

	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a = &r->slots[i];
		JobBoard *eb;
		Job *j;
		int engagedBand = JOB_INVALID;
		int challenger = JOB_INVALID;
		int skip[2];
		JobBoard *cb = &a->personal;
		Job *cj;

		if (!r->alive[i])
			continue;

		/* Step 2: a finished job is released and the agent is idle
		 * again. The job itself stays on its board — retiring it is
		 * the owner's call (boardRetire), not the tick's.
		 *
		 * A handle whose slot has gone (the owner retired the job)
		 * is the same case: the work is gone, so the agent lets go
		 * rather than holding a dead slot forever. A stale handle
		 * must never wedge an agent. */
		eb = agentBoardOf(a, boards);
		j = boardJob(eb, a->engagedJob);
		if (a->engagedJob != JOB_INVALID && j == NULL) {
			a->engagedJob = JOB_INVALID;
			a->engagedShared = false;
		} else if (j != NULL &&
			   (j->state == JOB_DONE || j->state == JOB_FAILED)) {
			boardRelease(eb, a->engagedJob);
			a->engagedJob = JOB_INVALID;
			a->engagedShared = false;
			j = NULL;
		}

		/* Step 3: staying power runs down, never below zero. */
		if (a->dwellLeft > 0.0f) {
			a->dwellLeft -= dt;
			if (a->dwellLeft < 0.0f)
				a->dwellLeft = 0.0f;
		}

		/* Step 4: the one question an agent may ask per tick. Nothing to
		 * switch TO is not a decision at all — an agent that holds a
		 * job and has no challenger keeps it, so the rule is only
		 * consulted once there is a candidate to move to. Note that
		 * "no decision" is NOT "do nothing": the job it already
		 * holds still advances below, decision or not. */
		skip[0] = a->engagedJob;
		skip[1] = JOB_INVALID;
		cj = agentBestJob(a, boards, skip, &challenger, &cb);
		if (cj != NULL) {
			engagedBand = (j != NULL) ? j->band : JOB_INVALID;
			/* The claim is the race: a job another agent took
			 * between the scan and here is simply not ours, and
			 * that is not a decision. (With the scan and the
			 * claim adjacent in one tick nothing can interleave;
			 * the guard is what keeps that true if a caller ever
			 * batches the scan.) */
			if (agentShouldSwitch(a, engagedBand, cj->band,
					      a->dwellLeft) &&
			    boardClaim(cb, challenger, a->id)) {
				/* Step 5: on a switch, the job being left is
				 * failed and released BEFORE anything new is
				 * claimed — re-planning is a new job, never a
				 * retarget of a live one. */
				if (j != NULL)
					agentDropJob(a, eb, j);
				a->engagedJob = challenger;
				a->engagedShared = (cb != &a->personal);
				a->dwellLeft = AGENT_MIN_DWELL;
				decisions++;
				g_stats.agentDecisions++;
				/* Step 6: claim the lifecycle, then route the
				 * body. */
				agentStartJob(a, ents, cj, map);
			}
		}

		/* Step 7: whatever this agent is running advances one tick,
		 * re-resolved because a switch may have changed both the job
		 * and the board it lives on. */
		eb = agentBoardOf(a, boards);
		j = boardJob(eb, a->engagedJob);
		if (j != NULL)
			agentAdvanceJob(a, ents, j, dt);
	}

	if (outDecisions != NULL)
		*outDecisions += decisions;
	return decisions;
}
