/*
 * Errand templates, adoption and step advance (CTOL rung 1: unit + boundary).
 *
 * Covers: the template table itself (ids resolve, names are present and
 * distinct, roles are real, every step names a real activity or a real gate,
 * the step count fits ERRAND_MAX_STEPS, every row serves a need, and the
 * declared floor is a real provenance class); the unknown-template error path;
 * the ROLE GATE; both adoption producers (need-driven and world-seeded) and
 * every refusal; the recorded floor and, above all, that it is a PROVENANCE
 * and never an order; a job step's bind / enqueue / advance / fail / re-select
 * cycle including the per-instance blacklist; the gate's wake, its refusal to
 * park unbounded, the deadline it carries, and the JOB_TIMEOUT that re-selects
 * behind it; the leave window's inputs; and every NULL guard.
 *
 * THE ASSERTIONS THAT MATTER MOST:
 *
 *   test_the_floor_is_provenance_never_an_order — a world-seeded errand is
 *   adopted at JOB_PRIO_ASSIGNED and its FIRST job, scored on a content
 *   agent's needs, bands 0. If the floor were leaking into the band, that job
 *   would carry the floor and this would be red — which is the whole of "a
 *   world-seeded errand is adopted at its floor, never forced".
 *
 *   test_a_gate_that_times_out_reselects — the JOB_TIMEOUT producer. The gate
 *   parks a job whose duration IS the declared deadline, and the same
 *   JOB_TIMEOUT event agentsUpdate sends is what makes it FAIL the step and
 *   re-select. Without this the laundromat can hang forever on a machine that
 *   never delivers its phase wake.
 *
 *   test_a_failed_step_blacklists_only_this_instance — a re-select must not be
 *   able to pick the thing that just failed, and a FRESH errand must be able
 *   to (which is why the blacklist is per instance, not per agent).
 *
 * Pure: links errand.c + its dependencies and the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_errand().
 */

#include "unity.h"

#include "agents/activity.h"
#include "agents/agent.h"
#include "agents/board.h"
#include "agents/errand.h"
#include "agents/job.h"
#include "sim/machines.h"

#include <stdio.h>
#include <string.h>

/* ---- fixtures ---------------------------------------------------------- */

/* An agent of `role` with fresh, content, calm needs, an empty personal board
 * and no errands — everything adoption reads, and nothing else. */
static Agent agentFor(int role)
{
	Agent a;

	memset(&a, 0, sizeof(a));
	needSetInit(&a.needs);
	a.panicMask = needPanicMask(&a.needs);
	a.role = role;
	a.alive = true;
	a.id = 7;
	a.entity = ENTITY_INVALID;
	boardInit(&a.personal, false);
	activityCooldownsInit(&a.cooldown);
	errandListInit(&a.errands);
	a.engagedJob = JOB_INVALID;
	return a;
}

/* The same, with every want EMPTY: the state a customer is actually in for
 * most of its life under the current need arithmetic. */
static Agent agentWithEmptyWants(int role)
{
	Agent a = agentFor(role);
	int n;

	for (n = 0; n < NEED_COUNT; n++)
		a.needs.value[n] = 0.0f;
	return a;
}

/* Drive a claimed board row through the whole lifecycle to DONE, the way the
 * framework's own tick would: start, arrive (walk), arrive (begin), the
 * activity's phase, then the wait ending. */
static void runJobToDone(JobBoard *b, int handle)
{
	Job *j = boardJob(b, handle);

	TEST_ASSERT_NOT_NULL(j);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, jobStep(j, JOB_START).action);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_MOVE_TO, jobStep(j, JOB_ARRIVED).action);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_BEGIN_ACTIVITY,
			      jobStep(j, JOB_ARRIVED).action);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_WAIT, jobStep(j, JOB_PHASE_DONE).action);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, jobStep(j, JOB_PHASE_DONE).state);
}

/* The live errand, asserted to exist (most tests are about one). */
static Errand *activeErrand(Agent *a)
{
	Errand *e = errandActive(a);

	TEST_ASSERT_NOT_NULL(e);
	return e;
}

/* Adopt the laundry errand and bind its first step to `machine` at
 * `tileX,tileZ`, the way the customer driver does on the customer's first
 * tick. Returns the live errand. */
static Errand *adoptLaundryBound(Agent *a, int machine, int tileX, int tileZ)
{
	Errand *e;

	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, errandAdoptByNeed(a, 0.0f));
	e = activeErrand(a);
	errandBindTarget(e, ERRAND_TARGET_MACHINE, machine,
			 JOB_TILE_PACK(tileX, tileZ));
	return e;
}

/* ---- the table --------------------------------------------------------- */

static void test_template_rows_are_well_formed(void)
{
	int i;
	int j;

	TEST_ASSERT_TRUE(errandsTemplateCount() > 0);
	TEST_ASSERT_TRUE(errandsTemplateCount() <= ERRAND_MAX);
	/* Every step of the laundry round is declared, not computed. */
	TEST_ASSERT_TRUE(errandsTemplate(ERR_LAUNDRY_CLEAN)->stepCount <=
			 ERRAND_MAX_STEPS);

	for (i = 0; i < errandsTemplateCount(); i++) {
		const ErrandTemplate *t = errandsTemplateAt(i);
		int served = 0;
		int n;

		TEST_ASSERT_NOT_NULL(t);
		/* Intent-revealing names: a row reads on its own. */
		TEST_ASSERT_NOT_NULL(t->name);
		TEST_ASSERT_TRUE(t->name[0] != '\0');
		TEST_ASSERT_TRUE(t->id >= 0);
		TEST_ASSERT_TRUE(t->id < ERR_COUNT);
		/* A role is a real role: errandAdopt refuses anything else, and
		 * a row naming none would be refused by everybody. */
		TEST_ASSERT_TRUE(t->role >= 0);
		TEST_ASSERT_TRUE(t->role < ROLE_COUNT);
		/* The floor is a provenance class and is validated as one. */
		TEST_ASSERT_TRUE(t->priorityFloor >= 0);
		TEST_ASSERT_TRUE(t->priorityFloor < JOB_PRIO_COUNT);
		/* A template that does nothing is not a goal. */
		TEST_ASSERT_TRUE(t->stepCount <= ERRAND_MAX_STEPS);
		for (n = 0; n < NEED_COUNT; n++) {
			TEST_ASSERT_TRUE(t->serves[n] >= 0.0f);
			TEST_ASSERT_TRUE(t->serves[n] <= 1.0f);
			if (t->serves[n] > 0.0f)
				served++;
		}
		TEST_ASSERT_TRUE(served >= 1);
		/* Every step is either work or a wait, and each names something
		 * real: a JOB step an activity the catalogue has, a GATE step a
		 * wake this module defines. A step naming neither would enqueue
		 * nothing and wait forever. */
		for (j = 0; j < t->stepCount; j++) {
			const ErrandStep *s = &t->steps[j];

			TEST_ASSERT_TRUE(s->kind == STEP_JOB ||
					 s->kind == STEP_GATE);
			if (s->kind == STEP_JOB) {
				TEST_ASSERT_NOT_NULL(activitiesGet(s->jobKind));
			} else {
				TEST_ASSERT_TRUE(s->gate == STEP_GATE_WAIT_MACHINE ||
						 s->gate == STEP_GATE_WALK_TO);
			}
		}
		/* Ids and names are unique, or errandsTemplate could not answer
		 * a lookup at all. */
		for (j = 0; j < i; j++) {
			const ErrandTemplate *o = errandsTemplateAt(j);

			TEST_ASSERT_TRUE(t->id != o->id);
			TEST_ASSERT_TRUE(strcmp(t->name, o->name) != 0);
		}
		TEST_ASSERT_EQUAL_PTR(t, errandsTemplate(t->id));
	}
}

/* The laundry round IS the T23 flow, as data: wash, gate on the machine,
 * collect, dry, gate, collect, leave. Pinned step by step so a reviewer can
 * check the template without reading the source, and so a reordering is a
 * visible test failure rather than a subtler one in the demo. */
static void test_the_laundry_round_is_the_customer_flow(void)
{
	const ErrandTemplate *t = errandsTemplate(ERR_LAUNDRY_CLEAN);

	TEST_ASSERT_NOT_NULL(t);
	TEST_ASSERT_EQUAL_STRING("LAUNDRY_CLEAN", t->name);
	TEST_ASSERT_EQUAL_INT(ROLE_CUSTOMER, t->role);
	TEST_ASSERT_FALSE(t->worldSeeded);
	TEST_ASSERT_EQUAL_INT(7, t->stepCount);
	TEST_ASSERT_EQUAL_INT(STEP_JOB, t->steps[0].kind);
	TEST_ASSERT_EQUAL_INT(JOB_USE_MACHINE, t->steps[0].jobKind);
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_WASHER, t->steps[0].machineKind);
	TEST_ASSERT_EQUAL_INT(STEP_GATE, t->steps[1].kind);
	TEST_ASSERT_EQUAL_INT(STEP_GATE_WAIT_MACHINE, t->steps[1].gate);
	TEST_ASSERT_EQUAL_INT(STEP_JOB, t->steps[2].kind);
	TEST_ASSERT_EQUAL_INT(JOB_COLLECT, t->steps[2].jobKind);
	TEST_ASSERT_EQUAL_INT(STEP_JOB, t->steps[3].kind);
	TEST_ASSERT_EQUAL_INT(JOB_USE_MACHINE, t->steps[3].jobKind);
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_DRYER, t->steps[3].machineKind);
	TEST_ASSERT_EQUAL_INT(STEP_GATE, t->steps[4].kind);
	TEST_ASSERT_EQUAL_INT(STEP_GATE_WAIT_MACHINE, t->steps[4].gate);
	TEST_ASSERT_EQUAL_INT(STEP_JOB, t->steps[5].kind);
	TEST_ASSERT_EQUAL_INT(JOB_COLLECT, t->steps[5].jobKind);
	TEST_ASSERT_EQUAL_INT(JOB_LEAVE, t->steps[6].jobKind);
	/* Waiting for a machine is an inconvenience, not an emergency: the
	 * goal serves tier-1 satisfaction and NOTHING serves survival. */
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.9f,
				      t->serves[NEED_SATISFACTION]);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f,
				      t->serves[NEED_SURVIVAL]);
}

static void test_unknown_template_is_an_error(void)
{
	TEST_ASSERT_NULL(errandsTemplate(999));
	TEST_ASSERT_NULL(errandsTemplate(-1));
	TEST_ASSERT_NULL(errandsTemplate(ERR_COUNT));
	TEST_ASSERT_NULL(errandsTemplateAt(-1));
	TEST_ASSERT_NULL(errandsTemplateAt(errandsTemplateCount()));
	TEST_ASSERT_NOT_NULL(errandsTemplateAt(0));
	TEST_ASSERT_EQUAL_PTR(errandsTemplateAt(0),
			      errandsTemplate(errandsTemplateAt(0)->id));
}

static void test_names(void)
{
	TEST_ASSERT_EQUAL_STRING("ACTIVE", errandStateName(ERRAND_ACTIVE));
	TEST_ASSERT_EQUAL_STRING("DONE", errandStateName(ERRAND_DONE));
	TEST_ASSERT_EQUAL_STRING("FAILED", errandStateName(ERRAND_FAILED));
	TEST_ASSERT_EQUAL_STRING("ABANDONED",
				 errandStateName(ERRAND_ABANDONED));
	TEST_ASSERT_EQUAL_STRING("?", errandStateName(999));
	TEST_ASSERT_EQUAL_STRING("JOB", errandStepKindName(STEP_JOB));
	TEST_ASSERT_EQUAL_STRING("GATE", errandStepKindName(STEP_GATE));
	TEST_ASSERT_EQUAL_STRING("WAIT_MACHINE",
				 errandStepKindName(STEP_GATE_WAIT_MACHINE));
	TEST_ASSERT_EQUAL_STRING("WALK_TO",
				 errandStepKindName(STEP_GATE_WALK_TO));
	TEST_ASSERT_EQUAL_STRING("JOB", errandStepKindName(999));
	TEST_ASSERT_TRUE(errandStateValid(ERRAND_ACTIVE));
	TEST_ASSERT_TRUE(errandStateValid(ERRAND_ABANDONED));
	TEST_ASSERT_FALSE(errandStateValid(ERRAND_STATE_COUNT));
	TEST_ASSERT_FALSE(errandStateValid(-1));
}

/* ---- adoption ---------------------------------------------------------- */

static void test_need_driven_adoption(void)
{
	Agent customer = agentFor(ROLE_CUSTOMER);
	Agent critter = agentFor(ROLE_CRITTER);

	/* A dirty-garment customer adopts the wash errand on their own: the
	 * template is registered for their role and nothing else is live. */
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN,
			      errandAdoptByNeed(&customer, 0.0f));
	TEST_ASSERT_EQUAL_INT(1, customer.errands.count);
	TEST_ASSERT_EQUAL_INT(ERRAND_ACTIVE, activeErrand(&customer)->state);
	/* The critical bit: adopting is not DOING. Nothing is on the board
	 * until the errand advances, and no need was written. */
	TEST_ASSERT_EQUAL_INT(0, customer.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, customer.engagedJob);

	/* A critter has no registered template: refused, not given one. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdoptByNeed(&critter, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, critter.errands.count);

	/* One goal at a time: the same template is not adopted twice. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID,
			      errandAdoptByNeed(&customer, 1.0f));
	TEST_ASSERT_EQUAL_INT(1, customer.errands.count);
}

static void test_adoption_is_role_gated(void)
{
	Agent critter = agentFor(ROLE_CRITTER);
	Agent customer = agentFor(ROLE_CUSTOMER);
	Agent staff = agentFor(ROLE_STAFF);
	const ErrandTemplate *laundry = errandsTemplate(ERR_LAUNDRY_CLEAN);
	const ErrandTemplate *fix = errandsTemplate(ERR_FIX_MACHINE);

	/* A customer's laundry round is not something a critter may adopt,
	 * and a staffer's fault round is not something a customer may. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdopt(&critter, laundry, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdopt(&customer, fix, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, critter.errands.count);
	TEST_ASSERT_EQUAL_INT(0, customer.errands.count);
	TEST_ASSERT_EQUAL_INT(ERR_FIX_MACHINE, errandAdopt(&staff, fix, 0.0f));
	/* The role is also what the need scan filters on: a FRESH staffer's scan
	 * finds the fault round (and never the laundry one), the customer's
	 * finds the laundry round (and never the fault one), and the critter
	 * has no row of its own at all. The staffer who already holds the fault
	 * round is offered nothing — one goal at a time. */
	{
		Agent fresh = agentFor(ROLE_STAFF);

		TEST_ASSERT_EQUAL_INT(ERR_FIX_MACHINE,
				      errandAdoptByNeed(&fresh, 0.0f));
	}
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdoptByNeed(&staff, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, errandAdoptByNeed(&customer, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdoptByNeed(&critter, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdopt(NULL, laundry, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdopt(&customer, NULL, 0.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandAdoptByNeed(NULL, 0.0f));
}

/* THE WORLD-SEEDED PRODUCER, and the whole of "adopted at its declared
 * priority floor, never forced". */
static void test_offer_to_agent_is_adopted_at_its_floor(void)
{
	/* CONTENT needs, deliberately: the point of the assertion below is that
	 * adoption changes nothing an agent WANTS, and an agent whose wants are
	 * already empty could not tell. */
	Agent staff = agentFor(ROLE_STAFF);
	const ErrandTemplate *t = errandsTemplate(ERR_FIX_MACHINE);
	Errand *e;

	TEST_ASSERT_TRUE(t->worldSeeded);
	/* The declared floor is a real provenance class, and a world event's
	 * errand is offered at one a self-chosen errand never carries. */
	TEST_ASSERT_TRUE(t->priorityFloor >= JOB_PRIO_ASSIGNED);

	{
		/* ADOPTION WRITES NOTHING BUT THE ERRAND LIST. Not a need, not a
		 * panic, not a job: an offer is a GOAL, and a goal that reached
		 * for the want ladder to make itself urgent would be exactly the
		 * forcing this contract forbids. Snapshotted across the offer
		 * because a framework that quietly edits an agent's needs is
		 * invisible from the outside until something else reads them. */
		NeedSet before = staff.needs;

		TEST_ASSERT_EQUAL_INT(ERR_FIX_MACHINE,
				      errandOfferToAgent(&staff,
							  ERR_FIX_MACHINE,
							  0.0f));
		TEST_ASSERT_EQUAL_MEMORY(&before, &staff.needs,
					 sizeof(NeedSet));
	}
	e = activeErrand(&staff);
	TEST_ASSERT_EQUAL_INT(t->priorityFloor, e->priorityFloor);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, e->startedAt);
	/* Offered does not mean acting: the offer enqueues nothing. */
	TEST_ASSERT_EQUAL_INT(0, staff.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, staff.engagedJob);

	/* An offer is an adoption, so it refuses everything an adoption
	 * refuses: the role gate and the already-active rule. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID,
			      errandOfferToAgent(&staff, ERR_FIX_MACHINE, 1.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID,
			      errandOfferToAgent(&staff, ERR_LAUNDRY_CLEAN, 1.0f));
	/* An unknown offer is reported, not adopted. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandOfferToAgent(&staff, 999,
							       1.0f));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandOfferToAgent(NULL,
							       ERR_FIX_MACHINE,
							       1.0f));
}

/* THE WORLD-SEEDED PRODUCER AT REGISTRY SCALE: one offer reaches EVERY agent
 * of the template's role, nobody else, and still forces nothing. The floor is
 * carried by each adoption and by no other field (the "never forced" contract
 * is the snapshot below it, not a comment). */
static void test_the_world_offers_to_every_agent_of_the_role(void)
{
	static AgentRegistry reg;
	AgentHandle dead;
	AgentHandle h1;
	AgentHandle custH;
	Agent *staff0;
	Agent *staff1;
	Agent *cust;
	NeedSet before;

	agentsInit(&reg);
	staff0 = agentGet(&reg, agentCreate(&reg, ROLE_STAFF));
	dead = agentCreate(&reg, ROLE_STAFF);
	h1 = agentCreate(&reg, ROLE_STAFF);
	staff1 = agentGet(&reg, h1);
	custH = agentCreate(&reg, ROLE_CUSTOMER);
	cust = agentGet(&reg, custH);
	TEST_ASSERT_NOT_NULL(staff0);
	TEST_ASSERT_NOT_NULL(staff1);
	TEST_ASSERT_NOT_NULL(cust);
	/* A dead slot BETWEEN two staff: the sweep must step over it rather
	 * than offer a ghost, and must reach the agent behind it. */
	agentDestroy(&reg, dead);
	TEST_ASSERT_NULL(agentGet(&reg, dead));

	before = staff0->needs;
	TEST_ASSERT_EQUAL_INT(2, errandsOfferToRole(&reg, ERR_FIX_MACHINE,
						    7.0f));
	/* Both staff adopted, at the template's DECLARED floor ... */
	TEST_ASSERT_NOT_NULL(activeErrand(staff0));
	TEST_ASSERT_NOT_NULL(activeErrand(staff1));
	TEST_ASSERT_EQUAL_INT(JOB_PRIO_ASSIGNED,
			      activeErrand(staff0)->priorityFloor);
	TEST_ASSERT_EQUAL_INT(JOB_PRIO_ASSIGNED,
			      activeErrand(staff1)->priorityFloor);
	TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.0f,
				 activeErrand(staff0)->startedAt);
	/* ... and a customer was not offered a staffer's fault round. The role
	 * comes from the row, so a caller cannot mis-cast the sweep. */
	TEST_ASSERT_NULL(errandActive(cust));
	TEST_ASSERT_EQUAL_INT(0, cust->errands.count);
	/* ADOPTION STILL WRITES NOTHING ELSE: the snapshot from the single-agent
	 * offer, applied to the sweep. */
	TEST_ASSERT_EQUAL_MEMORY(&before, &staff0->needs, sizeof(NeedSet));
	/* And nothing was enqueued for anybody — an offer is a goal, not a
	 * job. */
	TEST_ASSERT_EQUAL_INT(0, staff0->personal.liveCount);
	TEST_ASSERT_EQUAL_INT(0, staff1->personal.liveCount);

	/* Offering again adopts nothing: one instance of a goal at a time, so
	 * a world event that fires every frame cannot fill the floor with
	 * copies of the same errand. */
	TEST_ASSERT_EQUAL_INT(0, errandsOfferToRole(&reg, ERR_FIX_MACHINE,
						    8.0f));

	/* THE GUARDS. No registry is nothing offered; an id no row claims is a
	 * broken data row, reported by errandsTemplate and addressed to nobody
	 * — NOT silently adopted by whoever has the nearest role. */
	TEST_ASSERT_EQUAL_INT(0, errandsOfferToRole(NULL, ERR_FIX_MACHINE,
						    0.0f));
	TEST_ASSERT_EQUAL_INT(0, errandsOfferToRole(&reg, 999, 0.0f));
	/* ... and the refusals above changed nobody. */
	TEST_ASSERT_NOT_NULL(activeErrand(staff0));
	TEST_ASSERT_NULL(errandActive(cust));
}

/* THE FLOOR IS PROVENANCE, NEVER AN ORDER. This is the assertion that says a
 * world event cannot put itself on top of the agent's own wants: the seeded
 * errand carries JOB_PRIO_ASSIGNED on every job it enqueues, and its first
 * job scored against a CONTENT staffer bands 0 — a real band, computed from
 * the needs the activity serves. Had the floor leaked into the band, this
 * would read 1 (or 15) instead of 0. */
static void test_the_floor_is_provenance_never_an_order(void)
{
	Agent content = agentFor(ROLE_STAFF);
	Agent urgent = agentWithEmptyWants(ROLE_STAFF);
	Errand *e;
	Job *j;

	TEST_ASSERT_EQUAL_INT(ERR_FIX_MACHINE,
			      errandOfferToAgent(&content, ERR_FIX_MACHINE, 0.0f));
	e = activeErrand(&content);
	TEST_ASSERT_EQUAL_INT(JOB_PRIO_ASSIGNED, e->priorityFloor);
	errandBindTarget(e, ERRAND_TARGET_MACHINE, 3, JOB_TILE_PACK(4, 0));
	errandAdvance(e, &content, 0.0f, &content.personal);
	TEST_ASSERT_EQUAL_INT(1, content.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(0, e->jobHandle);
	j = boardJob(&content.personal, 0);
	TEST_ASSERT_NOT_NULL(j);
	TEST_ASSERT_EQUAL_INT(JOB_REPAIR, j->kind);
	TEST_ASSERT_EQUAL_INT(0, e->jobHandle);
	/* A content agent wants nothing this errand serves: band 0. The floor
	 * of 1 it was adopted at does not appear anywhere on the job. */
	TEST_ASSERT_EQUAL_INT(0, j->band);

	/* The same offer to a staffer whose ESTEEM is empty bands high —
	 * because the NEED moved, not because the floor did. */
	TEST_ASSERT_EQUAL_INT(ERR_FIX_MACHINE,
			      errandOfferToAgent(&urgent, ERR_FIX_MACHINE, 0.0f));
	e = activeErrand(&urgent);
	errandBindTarget(e, ERRAND_TARGET_MACHINE, 3, JOB_TILE_PACK(4, 0));
	errandAdvance(e, &urgent, 0.0f, &urgent.personal);
	j = boardJob(&urgent.personal, 0);
	TEST_ASSERT_NOT_NULL(j);
	TEST_ASSERT_TRUE(j->band > 0);
	TEST_ASSERT_TRUE(j->band <= NEED_BANDS - 1);
}

static void test_the_list_is_bounded_and_compacts(void)
{
	Agent a = agentFor(ROLE_CUSTOMER);
	Errand *e;

	/* The bound is structural: at most one live instance per template, so
	 * a list cannot hold more than the table has rows. Pinned because the
	 * overflow guard is NOT in the code — a guard that can never be false
	 * is worse than the invariant being written down and tested. */
	TEST_ASSERT_TRUE(errandsTemplateCount() <= ERRAND_MAX);

	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, errandAdoptByNeed(&a, 0.0f));
	e = activeErrand(&a);
	TEST_ASSERT_EQUAL_INT(0, a.errands.count - 1);
	/* Terminal business is dropped at the next adoption, so a long-lived
	 * agent reuses its slots. */
	e->state = ERRAND_DONE;
	TEST_ASSERT_NULL(errandActive(&a));
	TEST_ASSERT_NULL(errandFind(&a, ERR_LAUNDRY_CLEAN));
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, errandAdoptByNeed(&a, 1.0f));
	TEST_ASSERT_EQUAL_INT(1, a.errands.count);
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN,
			      activeErrand(&a)->tmplId);
	/* The reused slot carries none of the previous instance's state. */
	TEST_ASSERT_EQUAL_INT(0, activeErrand(&a)->failureCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, activeErrand(&a)->jobHandle);

	/* A fresh errand also finds a blank list on a born agent. */
	{
		Agent fresh = agentFor(ROLE_CUSTOMER);

		errandListInit(&fresh.errands);
		TEST_ASSERT_EQUAL_INT(0, fresh.errands.count);
		TEST_ASSERT_NULL(errandActive(&fresh));
		TEST_ASSERT_NULL(errandFind(&fresh, ERR_LAUNDRY_CLEAN));
	}
	errandListInit(NULL);
	TEST_ASSERT_NULL(errandActive(NULL));
	TEST_ASSERT_NULL(errandFind(NULL, ERR_LAUNDRY_CLEAN));
	TEST_ASSERT_NULL(errandFind(&a, -1));
}

/* Put ONE hand-built errand on the agent, bypassing adoption — the only way
 * to reach a record no honest sequence produces (an id no row claims, a step
 * past the end of the table). */
static void forceErrand(Agent *a, int tmplId, int step)
{
	errandListInit(&a->errands);
	a->errands.count = 1;
	a->errands.items[0].tmplId = tmplId;
	a->errands.items[0].step = step;
	a->errands.items[0].state = ERRAND_ACTIVE;
}

/* ---- job steps --------------------------------------------------------- */

static void test_a_step_with_nothing_bound_enqueues_nothing(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	/* The binding is cleared: the world has not found a free machine, so
	 * the step waits rather than walking to the origin. */
	errandBindTarget(e, ERRAND_TARGET_NONE, ERRAND_INVALID, ERRAND_INVALID);
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_TRUE(errandBlocked(e));

	/* Bound, the same step enqueues exactly one job of its declared kind,
	 * walking to the bound tile. */
	errandBindTarget(e, ERRAND_TARGET_MACHINE, 2, JOB_TILE_PACK(4, 0));
	TEST_ASSERT_FALSE(errandBlocked(e));
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(0, e->jobHandle);
	{
		Job *j = boardJob(&a.personal, 0);

		TEST_ASSERT_NOT_NULL(j);
		TEST_ASSERT_EQUAL_INT(JOB_USE_MACHINE, j->kind);
		TEST_ASSERT_EQUAL_INT(JOB_TARGET_TILE, j->targetKind);
		TEST_ASSERT_EQUAL_INT(JOB_TILE_PACK(4, 0), j->targetId);
		TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j->state);
		/* The band is COMPUTED, from the needs the activity serves. */
		TEST_ASSERT_TRUE(j->band > 0);
		TEST_ASSERT_TRUE(j->band <= NEED_BANDS - 1);
		/* The serve strengths came from the catalogue row, not from the
		 * template: an errand names WHAT to do, the catalogue says what
		 * it is worth. */
		TEST_ASSERT_EQUAL_INT(
			(int)(activitiesGet(JOB_USE_MACHINE)->serves[NEED_SATISFACTION] *
			      ACTIVITY_SERVE_SCALE + 0.5f),
			j->served[NEED_SATISFACTION]);
	}
	/* A live step is not advanced by another advance: the framework's tick
	 * is running it. */
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(0, e->step);
}

static void test_a_finished_job_advances_the_errand(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	const ErrandTemplate *t = errandsTemplate(ERR_LAUNDRY_CLEAN);

	errandAdvance(e, &a, 0.0f, &a.personal);
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	/* Retired, not merely released: a finished job is not left on the board
	 * for a long-lived agent to fill up with. */
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(1, e->step);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_EQUAL_INT(ERRAND_ACTIVE, e->state);

	/* Walk the whole template to its end, binding whatever each step
	 * needs, and pin that the errand finishes DONE rather than running off
	 * the end of the step table. */
	{
		int guard;

		for (guard = 0; guard < 64; guard++) {
			const ErrandStep *s = &t->steps[e->step];

			TEST_ASSERT_TRUE(e->step < t->stepCount);
			errandBindTarget(e, ERRAND_TARGET_MACHINE, 1,
					 JOB_TILE_PACK(3, 0));
			if (s->kind == STEP_GATE)
				errandGateSeconds(e, 30.0f);
			errandAdvance(e, &a, 0.0f, &a.personal);
			TEST_ASSERT_TRUE(e->jobHandle != ERRAND_INVALID);
			runJobToDone(&a.personal, e->jobHandle);
			errandAdvance(e, &a, 0.0f, &a.personal);
			if (e->state != ERRAND_ACTIVE)
				break;
		}
		TEST_ASSERT_EQUAL_INT(ERRAND_DONE, e->state);
		TEST_ASSERT_EQUAL_INT(0, e->step);
		TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
		TEST_ASSERT_NULL(errandActive(&a));
	}
}

static void test_a_failed_job_reselects_and_blacklists(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	Job *j;

	errandAdvance(e, &a, 0.0f, &a.personal);
	j = boardJob(&a.personal, e->jobHandle);
	TEST_ASSERT_NOT_NULL(j);
	/* DENIED from the state it is in is the real shape of a failure: a
	 * claim or target refused. */
	jobStep(j, JOB_DENIED);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, j->state);
	errandAdvance(e, &a, 0.0f, &a.personal);
	/* Re-select IN PLACE: the same step, nothing bound, the failed machine
	 * blacklisted so the next bind cannot pick it again. */
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(1, e->failureCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_TARGET_NONE, e->targetKind);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->targetId);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(1, e->blacklistCount);
	TEST_ASSERT_EQUAL_INT(2, e->blacklist[0]);

	/* The next binding is a FRESH chance: the blacklist bounded the old
	 * candidate, and clearing it here is what stops one failure from
	 * blacklisting every machine a re-select has left. */
	errandBindTarget(e, ERRAND_TARGET_MACHINE, 5, JOB_TILE_PACK(7, 0));
	TEST_ASSERT_EQUAL_INT(0, e->blacklistCount);
	TEST_ASSERT_EQUAL_INT(5, e->targetId);
}

static void test_a_vanished_job_reselects(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, a.personal.liveCount);
	/* Somebody retired the row between two calls. It is not this errand's
	 * to read the ending of, so the step re-selects rather than advancing
	 * on a result nobody reported. */
	TEST_ASSERT_TRUE(boardRetire(&a.personal, e->jobHandle));
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(1, e->failureCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
}

static void test_repeated_failures_fail_the_errand(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	int i;

	for (i = 0; i < ERRAND_MAX_STEP_FAILURES + 1; i++) {
		Job *j;

		errandBindTarget(e, ERRAND_TARGET_MACHINE, 1,
				 JOB_TILE_PACK(2, 0));
		errandAdvance(e, &a, 0.0f, &a.personal);
		j = boardJob(&a.personal, e->jobHandle);
		TEST_ASSERT_NOT_NULL(j);
		jobStep(j, JOB_DENIED);
		errandAdvance(e, &a, 0.0f, &a.personal);
	}
	/* Bounded: a failure that is not a machine (a route that will not
	 * compute, say) would otherwise retry forever. */
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, e->state);
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_NULL(errandActive(&a));
	/* A failed errand never advances again, whatever is asked of it. */
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, e->state);
}

/* ---- gates ------------------------------------------------------------- */

/* Every gate resumes on exactly the right wake, and the errand reports which
 * one its current step is waiting for — so the caller (the world) knows what
 * to deliver. */
static void test_gates_report_their_wake(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	const ErrandTemplate *t = errandsTemplate(ERR_LAUNDRY_CLEAN);
	int i;

	/* A job step has no gate: it ends on its own lifecycle. */
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandStepGate(e));
	/* Every gate in the table names one of the two wakes this module
	 * defines — the table check in the suite above pins the same thing at
	 * the data level; this pins it through the accessor. */
	for (i = 0; i < t->stepCount; i++) {
		if (t->steps[i].kind != STEP_GATE)
			continue;
		TEST_ASSERT_TRUE(t->steps[i].gate == STEP_GATE_WAIT_MACHINE ||
				 t->steps[i].gate == STEP_GATE_WALK_TO);
	}

	/* Finish the first step and the errand is ON a gate. */
	errandAdvance(e, &a, 0.0f, &a.personal);
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, e->step);
	TEST_ASSERT_EQUAL_INT(STEP_GATE_WAIT_MACHINE, errandStepGate(e));
	TEST_ASSERT_TRUE(errandBlocked(e));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandStepGate(NULL));
}

static void test_a_gate_without_a_deadline_enqueues_nothing(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	/* Onto the gate. */
	errandAdvance(e, &a, 0.0f, &a.personal);
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(STEP_GATE, errandsTemplate(ERR_LAUNDRY_CLEAN)
					      ->steps[e->step].kind);

	/* A gate with no declared budget is a HANG, so it parks nothing: only
	 * the world knows how long a machine runs, and the caller has not said
	 * yet. */
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_TRUE(errandBlocked(e));

	/* With the deadline declared, the gate parks exactly one stand-by job
	 * whose duration IS that deadline. */
	errandGateSeconds(e, 90.0f);
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, a.personal.liveCount);
	/* The gate resumes on the wake of the step before it — which is the
	 * USE_MACHINE step that started the machine. */
	TEST_ASSERT_EQUAL_INT(0, e->retryStep);
	{
		Job *j = boardJob(&a.personal, e->jobHandle);

		TEST_ASSERT_NOT_NULL(j);
		TEST_ASSERT_EQUAL_INT(ERRAND_GATE_JOB, j->kind);
		TEST_ASSERT_FLOAT_WITHIN(0.0001f, 90.0f, j->duration);
		TEST_ASSERT_EQUAL_INT(JOB_TILE_PACK(4, 0), j->targetId);
	}
	/* A sub-second budget rounds UP: shortening a deadline the world
	 * promised is the one rounding direction that can break something. */
	errandGateSeconds(e, 0.2f);
	TEST_ASSERT_EQUAL_INT(0, e->gateSecs);
	errandGateSeconds(e, 0.0f);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->gateSecs);
	errandGateSeconds(NULL, 5.0f);
}

/* THE JOB_TIMEOUT PRODUCER. The gate's wait runs out of time, the framework
 * sends the same JOB_TIMEOUT agentsUpdate sends, the wait job FAILS, and the
 * errand re-selects rather than hanging on a machine that never delivers its
 * phase wake. This is the cell agents/job.h documents and the framework had no
 * producer for before errands. */
static void test_a_gate_that_times_out_reselects(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	Job *j;

	errandAdvance(e, &a, 0.0f, &a.personal);
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	errandGateSeconds(e, 90.0f);
	errandAdvance(e, &a, 0.0f, &a.personal);
	j = boardJob(&a.personal, e->jobHandle);
	TEST_ASSERT_NOT_NULL(j);
	jobStep(j, JOB_START);
	jobStep(j, JOB_ARRIVED);
	jobStep(j, JOB_ARRIVED);
	jobStep(j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, j->state);
	/* The deadline expires and the framework fires its timeout. */
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, jobStep(j, JOB_TIMEOUT).state);
	errandAdvance(e, &a, 0.0f, &a.personal);
	/* Back at the machine step that bound the machine that never woke,
	 * with that machine blacklisted and nothing bound to walk to. */
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(1, e->failureCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_TARGET_NONE, e->targetKind);
	TEST_ASSERT_EQUAL_INT(1, e->blacklistCount);
	TEST_ASSERT_EQUAL_INT(2, e->blacklist[0]);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);
	TEST_ASSERT_EQUAL_INT(ERRAND_ACTIVE, e->state);
}

static void test_a_gate_that_wakes_advances(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	errandAdvance(e, &a, 0.0f, &a.personal);
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	errandGateSeconds(e, 90.0f);
	errandAdvance(e, &a, 0.0f, &a.personal);
	/* The machine's PHASE_DONE arrives: the world completes the wait. */
	runJobToDone(&a.personal, e->jobHandle);
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(2, e->step);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_EQUAL_INT(ERRAND_ACTIVE, e->state);
	/* Onto the collect step, which still knows its machine: the gate does
	 * not throw away the thing it was waiting on. */
	TEST_ASSERT_EQUAL_INT(ERRAND_TARGET_MACHINE, e->targetKind);
	TEST_ASSERT_EQUAL_INT(2, e->targetId);
	TEST_ASSERT_FALSE(errandBlocked(e));
}

/* ---- abandonment ------------------------------------------------------- */

static void test_abandoning_stops_the_errand(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(1, a.personal.liveCount);
	errandAbandon(e);
	TEST_ASSERT_EQUAL_INT(ERRAND_ABANDONED, e->state);
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	/* The target is KEPT: abandoning is a fact about the errand, and the
	 * caller releasing whatever the target holds is its own business — the
	 * errand layer has no claim on a machine and must not pretend to. */
	TEST_ASSERT_EQUAL_INT(ERRAND_TARGET_MACHINE, e->targetKind);
	TEST_ASSERT_EQUAL_INT(2, e->targetId);
	/* An abandoned errand enqueues nothing more and is not "blocked". */
	errandAdvance(e, &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
	TEST_ASSERT_FALSE(errandBlocked(e));
	TEST_ASSERT_NULL(errandActive(&a));
	errandAbandon(NULL);
}

/* THE REWIND: a machine that broke between its run finishing and its load
 * being collected sends the errand back to the USE_MACHINE step that produced
 * it — the old driver's "re-drive the whole plan task", expressed as data. */
static void test_a_rewind_returns_to_the_job_step_that_produced_the_loss(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);

	/* Walk to the collect step (2) with the gate passed. */
	TEST_ASSERT_TRUE(errandRewindToJob(e, JOB_USE_MACHINE));
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(0, e->failureCount);

	/* A rewound step keeps its blacklist: the machine that was lost is
	 * still a machine this errand must not pick again. */
	errandBlacklist(e, 2);
	TEST_ASSERT_EQUAL_INT(1, e->blacklistCount);
	/* ...but it does not keep a job handle, or it would go on running a job
	 * that belongs to the step it has left. */
	e->jobHandle = 7;
	TEST_ASSERT_TRUE(errandRewindToJob(e, JOB_USE_MACHINE));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);

	/* The search runs BACKWARDS from where the errand is, so a rewind from
	 * the very last step finds the dryer's USE_MACHINE step rather than
	 * jumping past it to the washer's. */
	e->step = 6;		/* the LEAVE step */
	TEST_ASSERT_TRUE(errandRewindToJob(e, JOB_USE_MACHINE));
	TEST_ASSERT_EQUAL_INT(3, e->step);

	/* With nothing of that kind left to go back to, "carry on" would be a
	 * step this template does not contain: the errand fails and says so. */
	e->step = 0;
	TEST_ASSERT_FALSE(errandRewindToJob(e, JOB_COLLECT));
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, e->state);
	TEST_ASSERT_EQUAL_INT(0, e->step);
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, e->jobHandle);
}

/* THE BLACKLIST IS BOUNDED AND IDEMPOTENT. Blacklisting the same machine
 * twice must not cost two slots (a step that fails on the same machine
 * repeatedly is the case), and a caller with nothing bound must not be able
 * to blacklist slot -1 into the list. */
static void test_the_blacklist_is_bounded_and_idempotent(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand *e = adoptLaundryBound(&a, 2, 4, 0);
	int i;

	for (i = 0; i < ERRAND_MAX_FAILED; i++)
		errandBlacklist(e, i);
	TEST_ASSERT_EQUAL_INT(ERRAND_MAX_FAILED, e->blacklistCount);
	/* Past the bound it is a bounded drop, not an overflow. */
	errandBlacklist(e, 99);
	errandBlacklist(e, 100);
	TEST_ASSERT_EQUAL_INT(ERRAND_MAX_FAILED, e->blacklistCount);
	/* A repeat costs nothing, and neither does a negative id. */
	for (i = 0; i < ERRAND_MAX_FAILED; i++)
		errandBlacklist(e, i);
	errandBlacklist(e, -1);
	errandBlacklist(NULL, 3);
	TEST_ASSERT_EQUAL_INT(ERRAND_MAX_FAILED, e->blacklistCount);
	/* Binding something fresh clears the list (see errandBindTarget). */
	errandBindTarget(e, ERRAND_TARGET_MACHINE, 5, JOB_TILE_PACK(7, 0));
	TEST_ASSERT_EQUAL_INT(0, e->blacklistCount);
}

/* ---- guards ------------------------------------------------------------ */

static void test_advance_guards(void)
{
	Agent a = agentWithEmptyWants(ROLE_CUSTOMER);
	Errand e;

	errandAdvance(NULL, &a, 0.0f, &a.personal);
	errandAdvance(&e, NULL, 0.0f, &a.personal);
	errandAdvance(&e, &a, 0.0f, NULL);
	TEST_ASSERT_EQUAL_INT(0, a.personal.liveCount);

	/* A record whose template id no row claims cannot advance, and saying
	 * so is the point: pretending it is waiting would hang the caller. */
	forceErrand(&a, 4242, 0);
	errandAdvance(&a.errands.items[0], &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, a.errands.items[0].state);

	/* A step index outside the template is the same answer. */
	forceErrand(&a, ERR_LAUNDRY_CLEAN, 99);
	errandAdvance(&a.errands.items[0], &a, 0.0f, &a.personal);
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, a.errands.items[0].state);

	/* A rewind asked for a kind no template step declares anywhere before
	 * it, and one asked for from a step past the end of the table (which
	 * searches from the LAST step rather than reading out of bounds). */
	forceErrand(&a, ERR_LAUNDRY_CLEAN, 0);
	TEST_ASSERT_FALSE(errandRewindToJob(&a.errands.items[0], JOB_COLLECT));
	TEST_ASSERT_EQUAL_INT(ERRAND_FAILED, a.errands.items[0].state);
	forceErrand(&a, ERR_LAUNDRY_CLEAN, 99);
	TEST_ASSERT_TRUE(errandRewindToJob(&a.errands.items[0],
					   JOB_USE_MACHINE));
	TEST_ASSERT_EQUAL_INT(3, a.errands.items[0].step);
	/* And a job kind that is not one at all is refused rather than walked. */
	TEST_ASSERT_FALSE(errandRewindToJob(&a.errands.items[0], -1));

	/* Every binding guard is NULL-safe. */
	errandBindTarget(NULL, ERRAND_TARGET_MACHINE, 0, 0);
	errandAbandon(NULL);
	TEST_ASSERT_FALSE(errandRewindToJob(NULL, JOB_USE_MACHINE));
	TEST_ASSERT_FALSE(errandBlocked(NULL));
	TEST_ASSERT_EQUAL_INT(ERRAND_INVALID, errandStepGate(NULL));
}

void run_test_errand(void);

void run_test_errand(void)
{
	RUN_TEST(test_template_rows_are_well_formed);
	RUN_TEST(test_the_laundry_round_is_the_customer_flow);
	RUN_TEST(test_unknown_template_is_an_error);
	RUN_TEST(test_names);
	RUN_TEST(test_need_driven_adoption);
	RUN_TEST(test_adoption_is_role_gated);
	RUN_TEST(test_offer_to_agent_is_adopted_at_its_floor);
	RUN_TEST(test_the_world_offers_to_every_agent_of_the_role);
	RUN_TEST(test_the_floor_is_provenance_never_an_order);
	RUN_TEST(test_the_list_is_bounded_and_compacts);
	RUN_TEST(test_a_step_with_nothing_bound_enqueues_nothing);
	RUN_TEST(test_a_finished_job_advances_the_errand);
	RUN_TEST(test_a_failed_job_reselects_and_blacklists);
	RUN_TEST(test_a_vanished_job_reselects);
	RUN_TEST(test_repeated_failures_fail_the_errand);
	RUN_TEST(test_gates_report_their_wake);
	RUN_TEST(test_a_gate_without_a_deadline_enqueues_nothing);
	RUN_TEST(test_a_gate_that_times_out_reselects);
	RUN_TEST(test_a_gate_that_wakes_advances);
	RUN_TEST(test_abandoning_stops_the_errand);
	RUN_TEST(test_a_rewind_returns_to_the_job_step_that_produced_the_loss);
	RUN_TEST(test_the_blacklist_is_bounded_and_idempotent);
	RUN_TEST(test_advance_guards);
}
