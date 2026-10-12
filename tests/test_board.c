/*
 * Job boards: where jobs wait, and the exclusive expiring claim (CTOL rung 1:
 * unit + boundary).
 *
 * Covers: boardInit's reset (and the shared flag); boardEnqueue (lowest free
 * slot, the by-value copy, the board-owned claim fields scrubbed on entry,
 * the JOB_INVALID refusals for a NULL / over-priority / full board — a bounded
 * drop, never an overflow); boardRetire (the reclaim path for a finished job,
 * slot scrubbing, reuse, guards); the claim rules that are the framework's
 * exclusivity guarantee (first claim wins, a second agent is refused, the
 * stamp is exactly now + BOARD_RESERVE_SECS, guards); boardExpire (the
 * reservation lapses and nobody sits on a job forever, the count, an unexpired
 * claim survives); boardRelease (cancel releases the claim, the job stays
 * enqueued and is claimable again); boardBestFor (highest band, LOWEST SLOT
 * on a tie, the skip list, dead slots, another agent's live claim, a lapsed
 * claim, terminal jobs, empty boards, guards); and run-to-run determinism.
 *
 * The size pin matters here: with the personal board embedded by value in
 * Agent (agents/agent.h), Job is multiplied by BOARD_MAX and again by
 * AGENT_MAX, so a field added back to Job is a multi-megabyte regression that
 * no behavioural assertion would notice.
 *
 * Pure: links board.c + job.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_board().
 */

#include "unity.h"

#include "agents/agent.h"
#include "agents/board.h"

#include <string.h>

/* A job as a producer hands it to a board: a kind, a hand-set band (Task 4
 * computes it from the needs a job serves), and nothing else. */
static Job makeJob(int kind, int band)
{
	Job j;

	jobInit(&j, kind);
	j.band = band;
	return j;
}

/* Enqueue `band` on a fresh board and return the handle. */
static int push(JobBoard *b, int band)
{
	Job j = makeJob(JOB_WANDER, band);

	return boardEnqueue(b, &j, JOB_PRIO_FILL);
}

/* Take a job the way an agent does: boardClaim stamps the reservation and
 * jobStep(START) claims the lifecycle. The result is the lifecycle's
 * JOB_CLAIMED — "reserved, not yet walking" — which is the state a
 * reservation is ABOUT, and the only state boardExpire frees. */
static bool take(JobBoard *b, int handle, int agentId)
{
	if (!boardClaim(b, handle, agentId))
		return false;
	jobStep(&b->slots[handle], JOB_START);
	return true;
}

/* Walk a taken job forward with the lifecycle's own cells, so the expiry
 * rules can be checked against a job that is genuinely under way. The ladder
 * is explicit rather than a loop: ARRIVED is a no-op from INTERACTING, so a
 * "step until you get there" loop never terminates. */
static void stepTo(JobBoard *b, int handle, int state)
{
	Job *j = &b->slots[handle];

	jobStep(j, JOB_ARRIVED);		/* CLAIMED -> WALKING */
	if (state > JOB_WALKING)
		jobStep(j, JOB_ARRIVED);	/* WALKING -> INTERACTING */
	if (state > JOB_INTERACTING)
		jobStep(j, JOB_PHASE_DONE);	/* -> WAITING */
}

/* ---- the shared board under staffing (task 6) --------------------------- *
 *
 * A shared board is only SHARED if N agents contend for one board's jobs, so
 * these tests drive agentsClaimFromShared — the one pass that does the
 * contention — rather than boardClaim directly. The oracles are the ones the
 * framework promises: exclusivity, a reservation that lapses instead of
 * hoarding, a RUNNING job nobody steals, and one job per agent. */

/* A staff agent with no body: the claim pass is the claim half and needs no
 * entity registry or map (the MOVE is the tick's, agents/agent.h). */
static Agent *hire(AgentRegistry *r, int role)
{
	Agent *a = agentGet(r, agentCreate(r, role));

	TEST_ASSERT_NOT_NULL(a);
	return a;
}

/* How many live agents of `role` are holding a job OFF THE SHARED BOARD. The
 * invariant "one job per staff agent" is about HOLDS, so it is counted over
 * the whole registry rather than over the agents a test happens to hold
 * pointers to — and it counts shared work only, because an agent busy on its
 * own errand is busy either way. */
static int sharedHolders(const AgentRegistry *r, int role)
{
	int n = 0;
	int i;

	for (i = 0; i < AGENT_MAX; i++)
		if (r->alive[i] && r->slots[i].role == role &&
		    r->slots[i].engagedShared &&
		    r->slots[i].engagedJob != JOB_INVALID)
			n++;
	return n;
}

/* The id of the agent holding `handle`, or JOB_INVALID. */
static int claimerOf(const JobBoard *b, int handle)
{
	return b->slots[handle].claimer;
}

static void test_claim_from_shared_gives_a_contended_job_to_one_agent(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	Agent *b;
	Agent *cust;

	agentsInit(&reg);
	boardInit(&shared, true);
	a = hire(&reg, ROLE_STAFF);
	b = hire(&reg, ROLE_STAFF);
	cust = hire(&reg, ROLE_CUSTOMER);
	TEST_ASSERT_TRUE(a->id != b->id);
	TEST_ASSERT_TRUE(push(&shared, 3) == 0);

	/* ONE pass, one job, two staff: exactly one claim. Exclusivity is
	 * boardClaim's (agents/board.h) — this is the contract being driven
	 * from the staffing side rather than asserted about it. */
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	TEST_ASSERT_TRUE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));
	/* The reservation, not a start: JOB_CLAIMED is the one state
	 * boardExpire will free, which is what stops a claim from becoming a
	 * lease (agents/board.h). */
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, shared.slots[0].state);
	/* The loser's business: nobody else got it. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, cust->engagedJob);
	TEST_ASSERT_EQUAL_INT(1, sharedHolders(&reg, ROLE_STAFF));

	/* A second pass INSIDE the window changes nothing: the job is held,
	 * so it is not a candidate for anybody, and the loser does not get a
	 * consolation claim. */
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.5f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b->engagedJob);
}

static void test_a_lapsed_reservation_goes_to_another_staffer(void)
{
	AgentRegistry reg;
	JobBoard shared;
	AgentHandle ah;
	Agent *a;
	Agent *b;
	int t;

	agentsInit(&reg);
	boardInit(&shared, true);
	ah = agentCreate(&reg, ROLE_STAFF);
	a = agentGet(&reg, ah);
	b = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_TRUE(push(&shared, 1) == 0);

	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));

	/* The window is honoured to its FULL length: at exactly the deadline
	 * the reservation still stands. */
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared,
						       BOARD_RESERVE_SECS));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));

	/* Past it, the reservation goes back on the board. That is the
	 * no-hoarding rule and it is observable on its own: nobody sits on a
	 * job they never started. */
	TEST_ASSERT_EQUAL_INT(1, boardExpire(&shared,
					     BOARD_RESERVE_SECS + 0.1f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, claimerOf(&shared, 0));

	/* And a DIFFERENT agent takes it, which is what the sweep is for: the
	 * staffer that stopped mid-reservation is gone, so the pool serves
	 * somebody who is here. (An idle staffer that lets its own
	 * reservation lapse would simply re-take the job on the next pass —
	 * the work DID go back to the board — but the case the spec names is
	 * the dead worker, and it is the one pinned here.) */
	agentDestroy(&reg, ah);
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared,
						       BOARD_RESERVE_SECS + 0.2f));
	TEST_ASSERT_EQUAL_INT(b->id, claimerOf(&shared, 0));
	TEST_ASSERT_EQUAL_INT(0, b->engagedJob);
	TEST_ASSERT_TRUE(b->engagedShared);
	TEST_ASSERT_EQUAL_INT(1, sharedHolders(&reg, ROLE_STAFF));

	/* And it is a fresh reservation, measured from the sweep that freed
	 * it — not the original deadline, which has passed. */
	for (t = 0; t < 3; t++)
		TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared,
			BOARD_RESERVE_SECS + 0.3f + (float)t * 0.1f));
	TEST_ASSERT_EQUAL_INT(b->id, claimerOf(&shared, 0));
}

static void test_a_lapsed_reservation_skips_a_staffer_that_is_busy(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	Agent *b;

	agentsInit(&reg);
	boardInit(&shared, true);
	a = hire(&reg, ROLE_STAFF);
	b = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_TRUE(push(&shared, 1) == 0);
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));

	/* Meanwhile the first staffer took on its own errand's work, which
	 * the tick may do to anybody whose dwell has expired. One job per
	 * agent means it is not offered the shared one back — and the work it
	 * abandoned goes to the staffer that is free, rather than waiting for
	 * a window it will not notice. */
	a->engagedJob = push(&a->personal, 5);
	TEST_ASSERT_TRUE(boardClaim(&a->personal, a->engagedJob, a->id));
	a->engagedShared = false;	/* its own board now, as a switch does */
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared,
						       BOARD_RESERVE_SECS + 0.1f));
	TEST_ASSERT_EQUAL_INT(b->id, claimerOf(&shared, 0));
	TEST_ASSERT_TRUE(b->engagedShared);
	TEST_ASSERT_EQUAL_INT(1, sharedHolders(&reg, ROLE_STAFF));
}

static void test_a_running_job_is_never_stolen_by_the_pass(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	Agent *b;

	agentsInit(&reg);
	boardInit(&shared, true);
	a = hire(&reg, ROLE_STAFF);
	b = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_TRUE(push(&shared, 1) == 0);

	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	/* The MOVE was spent: the job is under way, and a running claim is the
	 * owner's however long it runs (agents/board.h). Ten windows later the
	 * pass must still decline to hand it to anybody. */
	stepTo(&shared, 0, JOB_WALKING);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared,
						       BOARD_RESERVE_SECS * 10.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, 0));
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, shared.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b->engagedJob);
}

static void test_the_pass_gives_a_job_to_a_staffer_that_is_not_busy(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *busy;
	Agent *idle;
	int second;

	agentsInit(&reg);
	boardInit(&shared, true);
	busy = hire(&reg, ROLE_STAFF);
	idle = hire(&reg, ROLE_STAFF);
	/* Busy running its own errand's work on its PERSONAL board: one job
	 * per agent is an invariant of the pass, not a hope, so a staffer
	 * already engaged is not offered a second beside it. Claimed as well
	 * as engaged, because the pass RE-VERIFIES the handle before it
	 * believes it. */
	busy->engagedJob = push(&busy->personal, 5);
	TEST_ASSERT_TRUE(boardClaim(&busy->personal, busy->engagedJob,
				    busy->id));

	TEST_ASSERT_TRUE(push(&shared, 2) == 0);
	second = push(&shared, 1);

	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	/* The busy staffer took NOTHING: its one job stays its one job, and
	 * the idle one is the only claim taken. */
	TEST_ASSERT_EQUAL_INT(0, busy->engagedJob);
	TEST_ASSERT_FALSE(busy->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, idle->engagedJob);
	TEST_ASSERT_TRUE(idle->engagedShared);
	/* Both jobs are the same activity off a content agent, so both band
	 * 0 and the pick is the LOWEST SLOT — the determinism rule
	 * (agents/board.h), not a preference between the two jobs. */
	TEST_ASSERT_EQUAL_INT(idle->id, claimerOf(&shared, 0));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, claimerOf(&shared, second));
	TEST_ASSERT_EQUAL_INT(1, sharedHolders(&reg, ROLE_STAFF));
}

static void test_no_staffer_holds_more_than_one_job(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *staff[4];
	int claimed;
	int round;
	int i;

	agentsInit(&reg);
	boardInit(&shared, true);
	for (i = 0; i < 4; i++)
		staff[i] = hire(&reg, ROLE_STAFF);
	/* More jobs than staff, so the pass has to ration as well as claim. */
	for (i = 0; i < 6; i++)
		TEST_ASSERT_EQUAL_INT(i, push(&shared, i));

	/* A LONG run over the fake clock. Every pass re-verifies ownership and
	 * sweeps, so a reservation nobody started circulates until every job
	 * is legitimately held — and the two properties that make the floor
	 * safe hold on every pass of it: NO agent ever holds two jobs, and NO
	 * two agents ever hold the same one. */
	for (round = 0; round < 40; round++) {
		float now = (float)round * (BOARD_RESERVE_SECS / 4.0f);
		int live = 0;
		int k;

		claimed = agentsClaimFromShared(&reg, &shared, now);
		TEST_ASSERT_TRUE(claimed >= 0);
		/* Four staff, so at most four claims in a pass ... */
		TEST_ASSERT_TRUE(claimed <= 4);
		for (i = 0; i < 4; i++) {
			int h = staff[i]->engagedJob;

			if (h == JOB_INVALID)
				continue;
			TEST_ASSERT_TRUE(h < BOARD_MAX);
			/* Whatever an agent holds, it holds by CLAIM: the pass
			 * is the only writer of Job.claimer and it wrote this
			 * one. */
			TEST_ASSERT_EQUAL_INT(staff[i]->id,
					      claimerOf(&shared, h));
			/* ONE JOB PER AGENT, and exclusivity: no two staff
			 * may name the same slot, which is what stops one job
			 * being executed twice. */
			for (k = i + 1; k < 4; k++)
				TEST_ASSERT_TRUE(staff[k]->engagedJob != h);
		}
		for (k = 0; k < BOARD_MAX; k++)
			if (shared.alive[k] &&
			    claimerOf(&shared, k) != JOB_INVALID)
				live++;
		/* ... and the floor never has more claims than it has work. */
		TEST_ASSERT_TRUE(live <= 6);
	}

	/* The board drains: retiring every job leaves nobody engaged, because
	 * a handle whose slot no longer holds this agent's work reads as
	 * vanished rather than as work to finish. */
	for (i = 0; i < BOARD_MAX; i++)
		if (shared.alive[i])
			boardRetire(&shared, i);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 99.0f));
	TEST_ASSERT_EQUAL_INT(0, sharedHolders(&reg, ROLE_STAFF));
	for (i = 0; i < 4; i++)
		TEST_ASSERT_EQUAL_INT(JOB_INVALID, staff[i]->engagedJob);
}

static void test_the_pass_asks_about_competency_and_never_gates(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *clumsy;
	Agent *skilled;
	Job *j;

	agentsInit(&reg);
	boardInit(&shared, true);
	/* The worst hire the record can express: no repair skill at all and
	 * a clumsiness flaw. The pass takes the job anyway. */
	clumsy = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_TRUE(push(&shared, 4) == 0);
	j = &shared.slots[0];

	/* THE RULE, asked directly, across every input that could plausibly
	 * make it refuse: role, competency, deficiency, the job, and the two
	 * NULLs. */
	TEST_ASSERT_TRUE(agentCanAttempt(clumsy, j));
	TEST_ASSERT_TRUE(agentCanAttempt(NULL, j));
	TEST_ASSERT_TRUE(agentCanAttempt(clumsy, NULL));
	TEST_ASSERT_TRUE(agentCanAttempt(NULL, NULL));
	clumsy->skills[SKILL_REPAIR] = 9;
	clumsy->flaws[SKILL_SPEED] = 1;
	TEST_ASSERT_TRUE(agentCanAttempt(clumsy, j));
	clumsy->role = ROLE_CUSTOMER;
	TEST_ASSERT_TRUE(agentCanAttempt(clumsy, j));
	clumsy->role = ROLE_STAFF;

	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(clumsy->id, claimerOf(&shared, 0));

	/* A fluent hire is not preferred over a clumsy one: there is nothing
	 * to prefer ON, because the pass picks the best job for the agent it
	 * is asking, not the best agent for the job. With one job the pick is
	 * the only pick, and it goes to whoever is idle first. */
	skilled = hire(&reg, ROLE_STAFF);
	skilled->skills[SKILL_REPAIR] = 9;
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.1f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, skilled->engagedJob);
}

static void test_a_staffer_never_reclaims_a_reservation_it_holds(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	int i;
	int h = 0;

	agentsInit(&reg);
	boardInit(&shared, true);
	a = hire(&reg, ROLE_STAFF);
	for (i = 0; i < 3; i++)
		TEST_ASSERT_TRUE(push(&shared, i) == i);

	/* A RAW reservation: claimed by this agent and never engaged. Nothing
	 * in the tree does this — the pass claims and engages in the same
	 * breath, and so does the fill path — but the claim is public, so the
	 * state is reachable and has to have an answer. */
	TEST_ASSERT_TRUE(boardClaim(&shared, h, a->id));

	/* THE ANSWER: the pass declines. boardBestFor offers an agent a job
	 * it already holds (it is that agent's own work), the low slot index
	 * wins the tie against the other two jobs, and boardClaim refuses
	 * because START is not a re-claim (agents/board.h). So the pass takes
	 * nothing this round and leaves the reservation to its window — it
	 * will not double-book the agent, and it will not release work the
	 * agent legitimately holds in order to re-claim it. */
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, h));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, a->engagedJob);
	TEST_ASSERT_EQUAL_INT(0, sharedHolders(&reg, ROLE_STAFF));

	/* AND IT KEEPS DECLINING, because a claim whose lifecycle never left
	 * JOB_UNCLAIMED is the one case the sweep does NOT free (agents/
	 * board.h: the sweep only frees a RESERVATION, which is a job in
	 * JOB_CLAIMED). This is not a window, it is forever — which is
	 * exactly why nothing in the tree produces this state: the pass
	 * claims and engages in the same breath, and so does the fill path.
	 * The way out is the owner releasing it. */
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared,
						       BOARD_RESERVE_SECS * 10.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, h));
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, shared.slots[h].state);

	/* Once the window lapses the sweep hands it back and the pass takes
	 * it properly, engaged and started. (Not THIS reservation: see the
	 * next test for the case a sweep cannot free.) */
	boardRelease(&shared, h);
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 1.0f));
	TEST_ASSERT_EQUAL_INT(a->id, claimerOf(&shared, h));
	TEST_ASSERT_EQUAL_INT(h, a->engagedJob);
	TEST_ASSERT_TRUE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, shared.slots[h].state);
	TEST_ASSERT_EQUAL_INT(1, sharedHolders(&reg, ROLE_STAFF));
}

static void test_a_finished_shared_job_is_released_by_the_pass(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	Agent *b;

	agentsInit(&reg);
	boardInit(&shared, true);
	a = hire(&reg, ROLE_STAFF);
	b = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_TRUE(push(&shared, 1) == 0);
	TEST_ASSERT_EQUAL_INT(1, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);

	/* The job finishes under its owner. The claim pass is where a
	 * staffer's next look happens, so it is also where a FINISHED job is
	 * let go of — the same rule the tick applies (agents/agent.c), and the
	 * reason a staffer does not sit on a completed job. */
	stepTo(&shared, 0, JOB_WAITING);
	jobStep(&shared.slots[0], JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, shared.slots[0].state);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.1f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, claimerOf(&shared, 0));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b->engagedJob);
	/* Terminal work is not re-taken either: the job is still enqueued,
	 * but DONE is not a candidate (agents/board.h) — the WORLD retires
	 * finished work, and a pass that re-claimed it would run a job that
	 * has already run. */
	TEST_ASSERT_EQUAL_INT(JOB_DONE, shared.slots[0].state);

	/* Retired instead of finished: the slot is gone, and the agent that
	 * still names it must read it as vanished rather than as work. */
	boardRetire(&shared, 0);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.2f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, a->engagedJob);
	TEST_ASSERT_EQUAL_INT(0, sharedHolders(&reg, ROLE_STAFF));
}

static void test_the_pass_is_guarded_and_deterministic(void)
{
	AgentRegistry reg;
	JobBoard shared;
	Agent *a;
	int first;

	agentsInit(&reg);
	boardInit(&shared, true);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(NULL, &shared, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, NULL, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(NULL, NULL, 0.0f));

	/* A registry with no agents, and a board with no jobs: both are
	 * normal answers, not errors. */
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.0f));
	a = hire(&reg, ROLE_STAFF);
	TEST_ASSERT_EQUAL_INT(0, agentsClaimFromShared(&reg, &shared, 0.0f));
	TEST_ASSERT_TRUE(push(&shared, 2) == 0);
	first = agentsClaimFromShared(&reg, &shared, 0.0f);
	TEST_ASSERT_EQUAL_INT(1, first);
	/* Byte-identical run to run: the same registry, the same board and
	 * the same clock produce the same claim, the same way. */
	{
		AgentRegistry reg2;
		JobBoard shared2;
		Agent *a2;

		agentsInit(&reg2);
		boardInit(&shared2, true);
		a2 = hire(&reg2, ROLE_STAFF);
		TEST_ASSERT_TRUE(push(&shared2, 2) == 0);
		TEST_ASSERT_EQUAL_INT(first,
				      agentsClaimFromShared(&reg2, &shared2,
							    0.0f));
		TEST_ASSERT_EQUAL_MEMORY(&reg, &reg2, sizeof(reg));
		TEST_ASSERT_EQUAL_MEMORY(&shared, &shared2, sizeof(shared));
		TEST_ASSERT_EQUAL_INT(a->id, a2->id);
	}
}

/* ---- init and record size ----------------------------------------------- */

static void test_board_init_resets(void)
{
	JobBoard b;
	int i;

	boardInit(&b, true);
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);
	TEST_ASSERT_TRUE(b.shared);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.now);
	for (i = 0; i < BOARD_MAX; i++)
		TEST_ASSERT_FALSE(b.alive[i]);

	boardInit(&b, false);
	TEST_ASSERT_FALSE(b.shared);

	/* Re-init empties the board, so a fresh run is byte-identical. */
	push(&b, 3);
	TEST_ASSERT_EQUAL_INT(1, b.liveCount);
	boardInit(&b, false);
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);

	boardInit(NULL, false);
}

static void test_job_and_board_sizes_are_pinned(void)
{
	/* Task 2's deferred pin, taken here because Task 3 is what makes it
	 * load-bearing: the personal board is embedded in Agent by value, so
	 * sizeof(Job) x BOARD_MAX x AGENT_MAX is static memory. A re-added
	 * path array would pass every behavioural test and cost megabytes. */
	TEST_ASSERT_EQUAL_INT(60, sizeof(Job));
	/* One board, bounded and modest. The AGENT_MAX multiply is pinned in
	 * the agent suite, which is where Agent lives. */
	TEST_ASSERT_TRUE(sizeof(JobBoard) < 8192);
}

/* ---- enqueue ------------------------------------------------------------ */

static void test_enqueue_lands_on_the_lowest_free_slot(void)
{
	JobBoard b;
	Job *stored;

	boardInit(&b, false);
	TEST_ASSERT_EQUAL_INT(0, push(&b, 1));
	TEST_ASSERT_EQUAL_INT(1, push(&b, 1));
	TEST_ASSERT_EQUAL_INT(2, b.liveCount);

	/* Retire the middle slot: the next enqueue takes it back, not the
	 * end — the scan is lowest-first, so handles stay dense and the
	 * tie-break in boardBestFor stays meaningful. */
	TEST_ASSERT_TRUE(boardRetire(&b, 0));
	TEST_ASSERT_EQUAL_INT(0, push(&b, 1));
	TEST_ASSERT_EQUAL_INT(2, b.liveCount);

	/* The job is stored BY VALUE, verbatim, except for the fields the
	 * board owns. */
	{
		Job j = makeJob(JOB_USE_MACHINE, 7);

		TEST_ASSERT_EQUAL_INT(2, boardEnqueue(&b, &j, JOB_PRIO_ASSIGNED));
	}
	stored = boardJob(&b, 2);
	TEST_ASSERT_NOT_NULL(stored);
	TEST_ASSERT_EQUAL_INT(JOB_USE_MACHINE, stored->kind);
	TEST_ASSERT_EQUAL_INT(7, stored->band);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, stored->state);
	/* The board owns the claim: an enqueued job is nobody's, whatever
	 * the producer's record happened to carry. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, stored->claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, stored->claimExpiry);
}

static void test_enqueue_scrubs_a_reused_slot(void)
{
	JobBoard b;
	Job stale = makeJob(JOB_WANDER, 5);
	int h;

	boardInit(&b, false);
	h = boardEnqueue(&b, &stale, JOB_PRIO_FILL);
	TEST_ASSERT_EQUAL_INT(0, h);
	/* Claim it, then retire it with the claim still stamped. */
	TEST_ASSERT_TRUE(boardClaim(&b, h, 4));
	TEST_ASSERT_EQUAL_INT(4, b.slots[h].claimer);
	TEST_ASSERT_TRUE(boardRetire(&b, h));

	/* The reused slot must not show the previous occupant's claim. */
	stale.kind = JOB_NAP;
	boardEnqueue(&b, &stale, JOB_PRIO_FILL);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[0].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.slots[0].claimExpiry);
	TEST_ASSERT_EQUAL_INT(JOB_NAP, b.slots[0].kind);

	/* The scrub is the BOARD's, and it happens on every enqueue: a
	 * producer handing over a record it already stamped (a revived job, a
	 * reused template) does not get to smuggle its claim in with it. */
	stale.kind = JOB_REPAIR;
	stale.claimer = 9;
	stale.claimExpiry = 99.0f;
	h = boardEnqueue(&b, &stale, JOB_PRIO_ASSIGNED);
	TEST_ASSERT_EQUAL_INT(1, h);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[1].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.slots[1].claimExpiry);
	TEST_ASSERT_EQUAL_INT(JOB_REPAIR, b.slots[1].kind);
}

static void test_enqueue_full_board_is_a_bounded_drop(void)
{
	JobBoard b;
	int i;

	boardInit(&b, true);
	for (i = 0; i < BOARD_MAX; i++)
		TEST_ASSERT_TRUE(push(&b, i % 4) != JOB_INVALID);
	TEST_ASSERT_EQUAL_INT(BOARD_MAX, b.liveCount);

	/* One past capacity: refused, not written past the end. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, push(&b, 1));
	TEST_ASSERT_EQUAL_INT(BOARD_MAX, b.liveCount);

	/* The board is still usable: a retire frees exactly one slot. */
	TEST_ASSERT_TRUE(boardRetire(&b, 3));
	TEST_ASSERT_EQUAL_INT(BOARD_MAX - 1, b.liveCount);
	TEST_ASSERT_TRUE(push(&b, 1) != JOB_INVALID);
	TEST_ASSERT_EQUAL_INT(BOARD_MAX, b.liveCount);
}

static void test_enqueue_guards(void)
{
	JobBoard b;
	Job j = makeJob(JOB_WANDER, 1);

	boardInit(&b, false);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardEnqueue(NULL, &j, JOB_PRIO_FILL));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardEnqueue(&b, NULL, JOB_PRIO_FILL));
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);

	/* A priority class outside the enum is a data error, not a default:
	 * provenance decides who may claim, and an unknown class says
	 * nothing about that. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID,
			      boardEnqueue(&b, &j, JOB_PRIO_COUNT));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardEnqueue(&b, &j, -1));
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);

	/* Every defined class is accepted: provenance never orders. */
	TEST_ASSERT_TRUE(boardEnqueue(&b, &j, JOB_PRIO_ERRAND) != JOB_INVALID);
	TEST_ASSERT_TRUE(boardEnqueue(&b, &j, JOB_PRIO_ASSIGNED) !=
			 JOB_INVALID);
	TEST_ASSERT_TRUE(boardEnqueue(&b, &j, JOB_PRIO_FILL) != JOB_INVALID);
}

/* ---- retire ------------------------------------------------------------- */

static void test_retire_guards(void)
{
	JobBoard b;
	int h;

	boardInit(&b, false);
	h = push(&b, 4);

	TEST_ASSERT_FALSE(boardRetire(NULL, h));
	TEST_ASSERT_FALSE(boardRetire(&b, JOB_INVALID));
	TEST_ASSERT_FALSE(boardRetire(&b, -2));
	TEST_ASSERT_FALSE(boardRetire(&b, BOARD_MAX));
	TEST_ASSERT_FALSE(boardRetire(&b, 7));	/* never enqueued */
	TEST_ASSERT_EQUAL_INT(1, b.liveCount);

	TEST_ASSERT_TRUE(boardRetire(&b, h));
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);
	TEST_ASSERT_FALSE(b.alive[h]);
	/* Scrubbed, not merely unmarked: a dead slot cannot leak the
	 * previous occupant's claim to a reader that ignores alive[]. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[h].claimer);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, b.slots[h].state);
	/* A retired slot is dead, not merely unmarked. */
	TEST_ASSERT_NULL(boardJob(&b, h));
	/* Twice is a no-op, not a double decrement. */
	TEST_ASSERT_FALSE(boardRetire(&b, h));
	TEST_ASSERT_EQUAL_INT(0, b.liveCount);
}

static void test_board_job_guards(void)
{
	JobBoard b;

	boardInit(&b, false);
	TEST_ASSERT_NULL(boardJob(NULL, 0));
	TEST_ASSERT_NULL(boardJob(&b, JOB_INVALID));
	TEST_ASSERT_NULL(boardJob(&b, BOARD_MAX));
	TEST_ASSERT_NULL(boardJob(&b, 0));	/* live slot index, no job */
}

/* ---- claim: the exclusivity guarantee ----------------------------------- */

static void test_claim_is_exclusive(void)
{
	JobBoard b;
	int jh;
	float stamped;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 10.0f);	/* the board's clock reads 10s */

	TEST_ASSERT_TRUE(boardClaim(&b, jh, 7));
	/* THE property the whole framework leans on: a second agent cannot
	 * take a job somebody holds, and the holder is unchanged. */
	TEST_ASSERT_FALSE(boardClaim(&b, jh, 8));
	TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);

	/* The reservation is exactly BOARD_RESERVE_SECS past the board's
	 * clock — a stamp, not a renewal, so a claim cannot creep. */
	stamped = b.slots[jh].claimExpiry;
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 10.0f + BOARD_RESERVE_SECS, stamped);

	/* Re-claiming is refused too: START is not a re-claim, and neither
	 * is a second claim by the holder. */
	TEST_ASSERT_FALSE(boardClaim(&b, jh, 7));
	TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, stamped, b.slots[jh].claimExpiry);
}

static void test_claim_guards(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);

	TEST_ASSERT_FALSE(boardClaim(NULL, jh, 1));
	TEST_ASSERT_FALSE(boardClaim(&b, JOB_INVALID, 1));
	TEST_ASSERT_FALSE(boardClaim(&b, -1, 1));
	TEST_ASSERT_FALSE(boardClaim(&b, BOARD_MAX, 1));
	TEST_ASSERT_FALSE(boardClaim(&b, 9, 1));	/* never enqueued */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
}

static void test_expiry_releases_a_lapsed_reservation(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 10.0f);
	TEST_ASSERT_TRUE(take(&b, jh, 7));
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, b.slots[jh].state);

	/* Not yet: the window is still open, nothing is released. */
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 10.0f + BOARD_RESERVE_SECS));
	TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);

	/* The window lapses: the claim goes back on the board, so nobody
	 * sits on a job forever. */
	TEST_ASSERT_EQUAL_INT(1,
			      boardExpire(&b, 10.0f + BOARD_RESERVE_SECS + 0.1f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.slots[jh].claimExpiry);
	/* ...and the job is enqueued throughout: expiry releases the claim,
	 * it never drops the job. */
	TEST_ASSERT_EQUAL_INT(1, b.liveCount);

	TEST_ASSERT_TRUE(boardClaim(&b, jh, 8));
	TEST_ASSERT_EQUAL_INT(8, b.slots[jh].claimer);

	/* A NULL board and a NaN clock are both no-ops: a broken clock
	 * evicts nothing, advances nothing, and never leaves the board
	 * believing it has no claims. */
	TEST_ASSERT_EQUAL_INT(0, boardExpire(NULL, 1.0f));
	{
		float stamped = b.now;

		TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 0.0f / 0.0f));
		TEST_ASSERT_EQUAL_INT(8, b.slots[jh].claimer);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, stamped, b.now);
	}
}

static void test_expiry_counts_every_lapsed_claim(void)
{
	JobBoard b;
	int i;

	boardInit(&b, true);
	for (i = 0; i < 5; i++)
		push(&b, 1);
	/* Five claims taken one second apart, so they lapse one second
	 * apart (deadline = claim time + BOARD_RESERVE_SECS). */
	for (i = 0; i < 5; i++) {
		boardExpire(&b, (float)i);
		TEST_ASSERT_TRUE(take(&b, i, i + 1));
	}

	/* The count is the number RELEASED, not the number swept. */
	TEST_ASSERT_EQUAL_INT(2, boardExpire(&b, BOARD_RESERVE_SECS + 1.1f));
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, BOARD_RESERVE_SECS + 1.2f));
	TEST_ASSERT_EQUAL_INT(2, boardExpire(&b, BOARD_RESERVE_SECS + 3.1f));
	TEST_ASSERT_EQUAL_INT(1, boardExpire(&b, BOARD_RESERVE_SECS * 3.0f));
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, BOARD_RESERVE_SECS * 4.0f));
	for (i = 0; i < 5; i++)
		TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[i].claimer);
}

/* THE RESERVATION, NOT THE LEASE. A job that is claimed and never leaves
 * JOB_CLAIMED is freed by the window however long ago it was taken, and goes
 * back on the board for anybody — that is the whole point of a reservation:
 * nobody sits on a job forever without starting it. */
static void test_expiry_frees_a_claim_that_never_started(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 100.0f);
	TEST_ASSERT_TRUE(take(&b, jh, 7));
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, b.slots[jh].state);

	/* The window is honoured to its full length: nothing is freed at the
	 * deadline itself, and the job is freed just after it. */
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 100.0f + BOARD_RESERVE_SECS));
	TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
	TEST_ASSERT_EQUAL_INT(1,
			      boardExpire(&b, 100.0f + BOARD_RESERVE_SECS + 0.1f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.slots[jh].claimExpiry);

	/* ...and the freed job is claimable, not merely unmarked: the
	 * lifecycle still reads CLAIMED, which is a job somebody started,
	 * so the new owner revives nothing — it just walks it (START is a
	 * no-op on a live job, then ARRIVED issues the move). */
	TEST_ASSERT_TRUE(boardClaim(&b, jh, 8));
	TEST_ASSERT_EQUAL_INT(8, b.slots[jh].claimer);
	jobStep(&b.slots[jh], JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, b.slots[jh].state);
	jobStep(&b.slots[jh], JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, b.slots[jh].state);
}

/* THE LEASE. A job that has left JOB_CLAIMED is the owner's: no sweep, at
 * any elapsed time, frees it, and nobody else can take it. This is what keeps
 * a long job (a 30-second machine wash) exclusive. */
static void test_expiry_never_frees_a_running_job(void)
{
	JobBoard b;
	int states[3];
	int i;

	states[0] = JOB_WALKING;
	states[1] = JOB_INTERACTING;
	states[2] = JOB_WAITING;
	boardInit(&b, true);
	for (i = 0; i < 3; i++) {
		int jh = push(&b, 5);
		float deadline;

		boardExpire(&b, 10.0f * (float)i);
		TEST_ASSERT_TRUE(take(&b, jh, 7));
		stepTo(&b, jh, states[i]);
		TEST_ASSERT_EQUAL_INT(states[i], b.slots[jh].state);
		deadline = b.slots[jh].claimExpiry;

		/* Far past the deadline — an hour, say — and still held. */
		TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 3600.0f));
		TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, deadline,
					 b.slots[jh].claimExpiry);
		/* Nobody else can have it while it runs. */
		TEST_ASSERT_FALSE(boardClaim(&b, jh, 8));
		TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
	}
}

/* ...so a running job's claim ends the way every other one does: the owner
 * gives it up (preemption, cancel, abandon), or the job finishes and its
 * owner releases it. */
static void test_release_is_how_a_running_job_frees_its_claim(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 10.0f);
	TEST_ASSERT_TRUE(take(&b, jh, 7));
	stepTo(&b, jh, JOB_WALKING);

	/* Minutes later, still exclusively the owner's... */
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 400.0f));
	TEST_ASSERT_FALSE(boardClaim(&b, jh, 8));

	/* ...until the owner releases it, which is the preempt/cancel path. */
	boardRelease(&b, jh);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, b.slots[jh].state);  /* not reset */
	TEST_ASSERT_TRUE(boardClaim(&b, jh, 8));
	TEST_ASSERT_EQUAL_INT(8, b.slots[jh].claimer);
}

/* A long job stepped through a minute of simulated time is never stolen: the
 * sweep runs every second and the claimer never changes. */
static void test_a_long_running_job_is_never_stolen(void)
{
	JobBoard b;
	int jh;
	int sec;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 0.0f);
	TEST_ASSERT_TRUE(take(&b, jh, 7));
	stepTo(&b, jh, JOB_INTERACTING);

	for (sec = 1; sec <= 60; sec++) {
		/* The job is under way the whole time (it spends 20s
		 * interacting and 40s waiting on its machine phase). */
		if (sec == 20)
			jobStep(&b.slots[jh], JOB_PHASE_DONE);
		TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, (float)sec));
		TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
		TEST_ASSERT_FALSE(boardClaim(&b, jh, 8));
		TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
	}
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, b.slots[jh].state);
}

/* The window is honoured for its FULL length: a claim taken at 10s is still
 * held at exactly 15s and is released only after it. */
static void test_expiry_boundary_is_the_window_end(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 10.0f);
	TEST_ASSERT_TRUE(take(&b, jh, 7));
	TEST_ASSERT_EQUAL_INT(0, boardExpire(&b, 15.0f));
	TEST_ASSERT_EQUAL_INT(7, b.slots[jh].claimer);
	TEST_ASSERT_EQUAL_INT(1, boardExpire(&b, 15.0f + 0.05f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
}

static void test_release_makes_a_claim_able_again(void)
{
	JobBoard b;
	int jh;

	boardInit(&b, true);
	jh = push(&b, 5);
	boardExpire(&b, 2.0f);
	TEST_ASSERT_TRUE(boardClaim(&b, jh, 7));

	/* Cancel releases held resources and keeps the job: the work is
	 * still on the board, only its owner is gone. */
	boardRelease(&b, jh);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, b.slots[jh].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b.slots[jh].claimExpiry);
	TEST_ASSERT_EQUAL_INT(1, b.liveCount);
	TEST_ASSERT_NOT_NULL(boardJob(&b, jh));
	TEST_ASSERT_TRUE(boardClaim(&b, jh, 8));

	/* Releasing nothing is a no-op, not a corruption. */
	boardRelease(&b, JOB_INVALID);
	boardRelease(&b, 5);
	boardRelease(&b, BOARD_MAX);
	boardRelease(NULL, jh);
	TEST_ASSERT_EQUAL_INT(8, b.slots[jh].claimer);
}

/* ---- best-for ----------------------------------------------------------- */

static void test_best_for_takes_the_highest_band(void)
{
	JobBoard b;

	boardInit(&b, true);
	push(&b, 3);
	push(&b, 9);
	push(&b, 5);

	TEST_ASSERT_EQUAL_INT(1, boardBestFor(&b, NULL, 1));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardBestFor(NULL, NULL, 1));
}

static void test_best_for_breaks_ties_on_the_lowest_slot(void)
{
	JobBoard b;

	boardInit(&b, false);
	push(&b, 7);
	push(&b, 7);
	push(&b, 7);

	/* Deterministic, not arbitrary: the scan keeps the FIRST winner, so
	 * a tie always reads as the lowest slot index. */
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 1));
	TEST_ASSERT_TRUE(boardRetire(&b, 0));
	TEST_ASSERT_EQUAL_INT(1, boardBestFor(&b, NULL, 1));
}

static void test_best_for_honours_the_skip_list(void)
{
	JobBoard b;
	static const int skip[] = { 0, 2, JOB_INVALID };

	boardInit(&b, false);
	push(&b, 9);
	push(&b, 4);
	push(&b, 7);

	/* The skip list is sentinel-terminated: it names handles the caller
	 * is already handling (the engaged job), never a count. */
	TEST_ASSERT_EQUAL_INT(1, boardBestFor(&b, skip, 1));
	{
		/* Skipping the best leaves the second best. The list is
		 * SENTINEL-TERMINATED, so the terminator is part of the
		 * test data: without it boardBestFor reads past the end. */
		static const int onlyFirst[] = { 0, JOB_INVALID };
		TEST_ASSERT_EQUAL_INT(2, boardBestFor(&b, onlyFirst, 1));
	}
	{
		/* An empty skip list is the whole board. */
		static const int none[] = { JOB_INVALID };
		TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, none, 1));
	}
	{
		/* Out-of-range and dead handles in the list are inert: a stale
		 * skip must never make the scan return a dead slot. */
		static const int stale[] = { 3, -7, 999, JOB_INVALID };
		TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, stale, 1));
	}
	{
		static const int all[] = { 0, 1, 2, JOB_INVALID };
		TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardBestFor(&b, all, 1));
	}
}

static void test_best_for_skips_another_agents_claim(void)
{
	JobBoard b;

	boardInit(&b, true);
	push(&b, 9);
	push(&b, 4);
	boardExpire(&b, 1.0f);

	/* Somebody else holds the best job: it is not a candidate. */
	TEST_ASSERT_TRUE(take(&b, 0, 3));
	TEST_ASSERT_EQUAL_INT(1, boardBestFor(&b, NULL, 8));
	/* The holder may still see it (it is its own best candidate). */
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 3));

	/* The reservation lapses, the sweep frees it, and the job is back on
	 * the board for whoever is next — a claim cannot hide a job forever. */
	boardExpire(&b, 1.0f + BOARD_RESERVE_SECS + 0.5f);
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 8));
}

static void test_best_for_ignores_dead_and_terminal_jobs(void)
{
	JobBoard b;

	boardInit(&b, false);
	push(&b, 2);
	push(&b, 9);
	push(&b, 7);

	/* A DONE job is nobody's to pick: re-running one is the owner's
	 * revive (a START on the record), never a fresh selection. Same for
	 * a FAILED one. */
	jobStep(&b.slots[1], JOB_START);
	jobStep(&b.slots[1], JOB_ARRIVED);
	jobStep(&b.slots[1], JOB_ARRIVED);
	jobStep(&b.slots[1], JOB_PHASE_DONE);
	jobStep(&b.slots[1], JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, b.slots[1].state);
	TEST_ASSERT_EQUAL_INT(2, boardBestFor(&b, NULL, 1));

	jobStep(&b.slots[2], JOB_DENIED);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, b.slots[2].state);
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 1));

	/* Holding a job is exclusive whatever state it is in: with both live
	 * jobs claimed by agent 5 there is nothing left for agent 6... */
	boardExpire(&b, 0.0f);
	TEST_ASSERT_TRUE(boardClaim(&b, 1, 5));
	TEST_ASSERT_TRUE(boardClaim(&b, 0, 5));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardBestFor(&b, NULL, 6));
	/* ...and releasing them does NOT bring the terminal ones back: a
	 * DONE/FAILED job is nobody's to pick, which is why an owner retires
	 * it (boardRetire) rather than releasing it. */
	boardRelease(&b, 1);
	boardRelease(&b, 0);
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 6));
}

static void test_best_for_on_an_empty_board(void)
{
	JobBoard b;

	boardInit(&b, false);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardBestFor(&b, NULL, 1));
	/* Emptying the board empties the answer. */
	push(&b, 15);
	TEST_ASSERT_EQUAL_INT(0, boardBestFor(&b, NULL, 1));
	TEST_ASSERT_TRUE(boardRetire(&b, 0));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardBestFor(&b, NULL, 1));
}

/* ---- determinism -------------------------------------------------------- */

static void test_board_is_deterministic(void)
{
	JobBoard a, b;
	int i;

	boardInit(&a, true);
	boardInit(&b, true);
	for (i = 0; i < 16; i++) {
		push(&a, i % 5);
		push(&b, i % 5);
	}
	boardExpire(&a, 3.0f);
	boardExpire(&b, 3.0f);
	boardClaim(&a, 4, 2);
	boardClaim(&b, 4, 2);
	boardRelease(&a, 9);
	boardRelease(&b, 9);
	boardRetire(&a, 1);
	boardRetire(&b, 1);

	TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

void run_test_board(void);

void run_test_board(void)
{
	RUN_TEST(test_board_init_resets);
	RUN_TEST(test_job_and_board_sizes_are_pinned);
	RUN_TEST(test_enqueue_lands_on_the_lowest_free_slot);
	RUN_TEST(test_enqueue_scrubs_a_reused_slot);
	RUN_TEST(test_enqueue_full_board_is_a_bounded_drop);
	RUN_TEST(test_enqueue_guards);
	RUN_TEST(test_retire_guards);
	RUN_TEST(test_board_job_guards);
	RUN_TEST(test_claim_is_exclusive);
	RUN_TEST(test_claim_guards);
	RUN_TEST(test_expiry_releases_a_lapsed_reservation);
	RUN_TEST(test_expiry_counts_every_lapsed_claim);
	RUN_TEST(test_expiry_frees_a_claim_that_never_started);
	RUN_TEST(test_expiry_never_frees_a_running_job);
	RUN_TEST(test_release_is_how_a_running_job_frees_its_claim);
	RUN_TEST(test_a_long_running_job_is_never_stolen);
	RUN_TEST(test_expiry_boundary_is_the_window_end);
	RUN_TEST(test_release_makes_a_claim_able_again);
	RUN_TEST(test_best_for_takes_the_highest_band);
	RUN_TEST(test_best_for_breaks_ties_on_the_lowest_slot);
	RUN_TEST(test_best_for_honours_the_skip_list);
	RUN_TEST(test_best_for_skips_another_agents_claim);
	RUN_TEST(test_best_for_ignores_dead_and_terminal_jobs);
	RUN_TEST(test_best_for_on_an_empty_board);
	RUN_TEST(test_board_is_deterministic);
	RUN_TEST(test_claim_from_shared_gives_a_contended_job_to_one_agent);
	RUN_TEST(test_a_lapsed_reservation_goes_to_another_staffer);
	RUN_TEST(test_a_lapsed_reservation_skips_a_staffer_that_is_busy);
	RUN_TEST(test_a_running_job_is_never_stolen_by_the_pass);
	RUN_TEST(test_the_pass_gives_a_job_to_a_staffer_that_is_not_busy);
	RUN_TEST(test_no_staffer_holds_more_than_one_job);
	RUN_TEST(test_the_pass_asks_about_competency_and_never_gates);
	RUN_TEST(test_a_staffer_never_reclaims_a_reservation_it_holds);
	RUN_TEST(test_a_finished_shared_job_is_released_by_the_pass);
	RUN_TEST(test_the_pass_is_guarded_and_deterministic);
}