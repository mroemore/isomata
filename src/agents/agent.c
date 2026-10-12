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
		/* Every activity pickable, and nothing the world offers: a born
		 * agent is content and has done nothing, so the only fills it
		 * can reach are the in-place ones (agents/activity.h). */
		activityCooldownsInit(&a->cooldown);
		a->places.othersInRange = 0;
		a->places.arcadeTile = JOB_INVALID;
		a->places.machineId = JOB_INVALID;
		a->clock = 0.0f;
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
 * to take.
 *
 * THE TWO SKIP LISTS ARE SEPARATE ON PURPOSE. A handle is a slot index ON
 * ITS OWN BOARD, so personal slot 0 and shared slot 0 are different jobs: the
 * job the agent already holds may only hide the same slot of the same board,
 * and each list says exactly that. One shared list would make an agent
 * engaged on personal slot 0 blind to shared slot 0, which is a coupling
 * between two boards that share nothing but a number. */
static Job *agentBestJob(Agent *a, AgentBoards *boards,
			 const int *personalSkip, const int *sharedSkip,
			 int *outHandle, JobBoard **outBoard)
{
	int personal = boardBestFor(&a->personal, personalSkip, a->id);
	int shared = JOB_INVALID;
	int personalBand = JOB_INVALID;
	int sharedBand = JOB_INVALID;

	*outHandle = JOB_INVALID;
	*outBoard = &a->personal;
	if (boards != NULL) {
		shared = boardBestFor(&boards->shared, sharedSkip, a->id);
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

/* An IN-PLACE activity binds JOB_TARGET_NONE deliberately: it happens where
 * the agent stands, so there is nothing to walk to and no route to refuse.
 * The lifecycle has exactly the cells for this — CLAIMED + ARRIVED enters
 * WALKING, and WALKING + ARRIVED binds the activity — so two ARRIVEDs in a
 * row land in INTERACTING with no MOVE_TO ever issued and no body needed. The
 * job then parks on the activity's duration, exactly as the demo's timed wait
 * always did. */
static void agentStartInPlaceJob(Job *j)
{
	jobStep(j, JOB_START);
	jobStep(j, JOB_ARRIVED);	/* CLAIMED -> WALKING */
	jobStep(j, JOB_ARRIVED);	/* WALKING  -> INTERACTING */
}

static void agentStartJob(Agent *a, EntityRegistry *ents, Job *j,
			  const Voxmap *map)
{
	Entity *e = (ents != NULL) ? entityGet(ents, a->entity) : NULL;
	int tx = 0;
	int tz = 0;
	int len;

	/* An in-place activity is never walked to. */
	if (j->targetKind == JOB_TARGET_NONE) {
		agentStartInPlaceJob(j);
		return;
	}
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

/* REFRESH ONE BOARD'S BANDS, for the agent about to decide. A job's urgency
 * is COMPUTED from the needs it serves (agents/activity.h) — never assigned
 * by whoever queued it — and the needs moved this tick, so every cached band
 * is stale by construction. A freshly enqueued job arrives with band -1
 * ("dirty"), so skipping this would make new work permanently ineligible
 * rather than merely mis-banded.
 *
 * THE COST, stated honestly: this runs EVERY TICK, for EVERY live agent,
 * over BOTH boards it can read — up to BOARD_MAX slots each — whether or
 * not that agent has any candidate at all. It is not a decision-point cost
 * and should not be described as one. The spec's "decision points, not
 * per-tick rescoring" applies to the DECISION (agentShouldSwitch) and still
 * holds; the band refresh is a separate, unconditional walk.
 *
 * Measured (task 4, .tmp_files/bench_agents.c, headless -O2, 250 agents):
 * the whole agent tick peaks at ~470 us on a 16666 us frame — 2.8 % — and
 * the profile says the DEAD-SLOT SCAN, not the jobUrgency arithmetic, is
 * the dominant term (~374 us of that, against ~96 us of live scoring at 64
 * jobs per board). A dirty-flagged skip list is the spec's own anticipated
 * fix and the numbers say it is worth doing before the agent count grows,
 * but at AGENT_MAX today it is not a 60fps problem.
 *
 * A job whose activity is unknown scores -1, which is an ERROR rather than a
 * boring band, so it is failed here and now through the lifecycle: the
 * caller sees the release and no claim is left hanging. That is the Review
 * Focus #5 contract, enforced at the only place a band is written.
 *
 * WHY A SHARED BOARD'S BANDS ARE THE READER'S. Band is a function of WHO is
 * asking — the same job is band 8 to one agent and band 2 to another — so a
 * shared job has no single true band, only one true band PER AGENT. The
 * personal board sidesteps this by being exclusively owned. Here the band is
 * refreshed immediately before this agent's scan and consumed by that same
 * scan, so the decision is always made on this agent's own arithmetic; the
 * stored value afterwards describes whoever read it last and no decision
 * ever trusts a stored band it did not just compute. Giving a shared job a
 * per-agent band that persists would need a second data structure, and that
 * is a staffing question for the shared-board task, not this one. */
static void agentRefreshBands(Agent *a, JobBoard *b)
{
	int slot;

	for (slot = 0; slot < BOARD_MAX; slot++) {
		Job *j;
		int band;

		if (!b->alive[slot])
			continue;
		j = &b->slots[slot];
		/* Terminal work is finished business: leave it be. */
		if (j->state == JOB_DONE || j->state == JOB_FAILED)
			continue;
		band = jobUrgency(a, j);
		if (band < 0) {
			/* A BROKEN JOB, not an urgent one. */
			jobStep(j, JOB_DENIED);
			continue;
		}
		j->band = band;
	}
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
		int pskip[2];
		int sskip[2];
		JobBoard *cb = &a->personal;
		Job *cj;

		if (!r->alive[i])
			continue;

		/* Step 2: a finished job is released and the agent is idle
		 * again. The job itself stays on its board — retiring it is
		 * the owner's call (boardRetire), not the tick's.
		 *
		 * engagedJob ASSERTS OWNERSHIP, it is not a hint: a handle
		 * names a SLOT, and a slot can be retired and refilled
		 * between two ticks. Anything in that slot this agent does
		 * not hold a claim on is therefore not its job — stepping it
		 * would execute somebody else's work and releasing it would
		 * clear their live claim. Gone, refilled or sold, it reads
		 * as vanished, and a stale handle must never wedge an agent
		 * nor cost another agent its claim. */
		eb = agentBoardOf(a, boards);
		j = boardJob(eb, a->engagedJob);
		if (a->engagedJob != JOB_INVALID &&
		    (j == NULL || j->claimer != a->id)) {
			a->engagedJob = JOB_INVALID;
			a->engagedShared = false;
			j = NULL;
		} else if (j != NULL &&
			   (j->state == JOB_DONE || j->state == JOB_FAILED)) {
			/* A job that COMPLETED puts its activity on
			 * cooldown for this agent (agents/activity.h), so
			 * an agent that just napped does not immediately nap
			 * again. A FAILED job did not happen and stamps
			 * nothing: a nap refused by the world must not cost
			 * the agent the real one. */
			if (j->state == JOB_DONE)
				activityMarkDone(&a->cooldown, j->kind,
						 a->clock);
			boardRelease(eb, a->engagedJob);
			a->engagedJob = JOB_INVALID;
			a->engagedShared = false;
			j = NULL;
		}

		/* Step 3: staying power runs down, never below zero, and the
		 * agent's own cooldown clock advances with it. The clock is
		 * accumulated dt, never a wall clock, so the same dt sequence
		 * always produces the same cooldowns (agents/activity.h). */
		a->clock += dt;
		if (a->dwellLeft > 0.0f) {
			a->dwellLeft -= dt;
			if (a->dwellLeft < 0.0f)
				a->dwellLeft = 0.0f;
		}

		/* Step 3b: refresh this agent's own bands before anything
		 * reads one (see agentRefreshBands for why the shared board
		 * is refreshed later, immediately before its scan). */
		agentRefreshBands(a, &a->personal);

		/* Step 4: the one question an agent may ask per tick. Nothing to
		 * switch TO is not a decision at all — an agent that holds a
		 * job and has no challenger keeps it, so the rule is only
		 * consulted once there is a candidate to move to. Note that
		 * "no decision" is NOT "do nothing": the job it already
		 * holds still advances below, decision or not. */
		/* The shared board's bands are refreshed HERE, immediately
		 * before the scan that consumes them, so a job queued there
		 * by a producer that never computed a band is still eligible
		 * and still compared on this agent's own arithmetic. */
		if (boards != NULL)
			agentRefreshBands(a, &boards->shared);
		pskip[0] = a->engagedShared ? JOB_INVALID : a->engagedJob;
		pskip[1] = JOB_INVALID;
		sskip[0] = a->engagedShared ? a->engagedJob : JOB_INVALID;
		sskip[1] = JOB_INVALID;
		cj = agentBestJob(a, boards, pskip, sskip, &challenger, &cb);
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

		/* Step 6b: IF NOTHING ELSE IS AVAILABLE, DO SOMETHING. An agent
		 * with an empty board and nothing to pick would stand still
		 * forever, and "an agent does secondary activities when
		 * blocked" is the spec's whole second half.
		 *
		 * A GENERATED FILL ONLY FILLS A GENUINELY EMPTY BOARD. The
		 * condition is liveCount == 0, not merely "the agent is
		 * idle": a personal board holding queued or running work is
		 * being fed by a PRODUCER — an errand advancing, or the
		 * demo's own patrol queueing the next leg — and an agent must
		 * never talk over its producer by inventing filler beside
		 * work that is already coming. A board with nothing on it is
		 * the one case where standing still is the wrong answer.
		 *
		 * The pick is agentBestFill: the same jobUrgency fold every
		 * other job is scored with, so a self-chosen fill competes on
		 * identical terms with an errand's job (agents/activity.h).
		 * Nothing here is a per-role behaviour switch; adding a
		 * behaviour is adding a row. */
		if (a->engagedJob == JOB_INVALID &&
		    a->personal.liveCount == 0) {
			int fill = agentBestFill(a, &a->personal, a->clock);

			if (fill != JOB_INVALID) {
				Job fj;
				int h;

				/* NONE OF THESE THREE CALLS CAN FAIL, and none is
				 * guarded, because every precondition is this
				 * block's own invariant:
				 *
				 *   activityBindJob — agentBestFill returns
				 *   only ids the catalogue actually has.
				 *
				 *   boardEnqueue — liveCount == 0 is precisely
				 *   the condition that means the board has
				 *   room; a refusal needs all BOARD_MAX slots
				 *   alive, which is liveCount == BOARD_MAX.
				 *
				 *   boardClaim — the job was enqueued on an
				 *   EMPTY board one statement ago, and
				 *   boardEnqueue forces `claimer` to
				 *   JOB_INVALID (board.c), which is the only
				 *   state from which boardClaim proceeds.
				 *   Its two refusals are a handle naming no
				 *   live job, and a job somebody already
				 *   holds; neither can hold here. A contended
				 *   setup would need a job on the board —
				 *   which is exactly what makes liveCount
				 *   non-zero and the gate false.
				 *
				 * A guard would be a branch that can never be
				 * false, which is worse than the invariant
				 * being stated — and the invariant is TESTED:
				 * test_update_generates_a_fill_on_an_empty_
				 * board asserts the claimer is this agent's,
				 * which is what would break first. */
				activityBindJob(&fj, fill);
				{
					int band = jobUrgency(a, &fj);

					/* Deliberately UNCHECKED, and the
					 * (void) says so. The id came from
					 * agentBestFill over the very catalogue
					 * this resolves against, so the -1 error
					 * is structurally unreachable; a real
					 * check would be a branch that can never
					 * be false. Were a row ever to go
					 * missing, the failure is not lost:
					 * the NEXT tick's refresh DENIES any job
					 * it cannot score, so it fails loudly
					 * rather than sitting on a board
					 * looking merely boring. */
					(void)band;
					fj.band = band;
				}
				h = boardEnqueue(&a->personal, &fj, JOB_PRIO_FILL);
				/* Enqueued AND claimed in the same breath,
				 * exactly as a job taken off a board is: a fill
				 * the agent had to wait a tick for would be a
				 * visible stall in the running demo. */
				boardClaim(&a->personal, h, a->id);
				a->engagedJob = h;
				a->engagedShared = false;
				a->dwellLeft = AGENT_MIN_DWELL;
				agentStartJob(a, ents,
					       boardJob(&a->personal, h), map);
				decisions++;
				g_stats.agentDecisions++;
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
