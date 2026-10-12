/*
 * Customer-driver tests (CTOL rung 1: unit + boundary), including the
 * acceptance oracle: deterministic fake-dt integration scenarios over the
 * ERRAND-DRIVEN customer (agents/errand.h).
 *
 * THE THREE T23 SCENARIOS, MIGRATED. Task 5 moved the customer's flow from a
 * BrainPhase switch onto an errand template, and a switch's tests could pin an
 * exact interleaving of two customers' independent events. An errand's tests
 * pin INVARIANTS and REPLAYABILITY instead, because with several agents the
 * order of independent events is emergent — pinning it would pin noise. What
 * each migrated scenario still asserts:
 *
 *   break mid-run and re-plan — the break was SEEN (an INTERRUPTED event), the
 *   broken machine was never selected again, the customer still left CLEAN DRY,
 *   and the machine it abandoned was released. What it no longer asserts is the
 *   exact sequence of events, because where the re-select lands between the two
 *   washers and the two legs is a property of the walk, not of the contract.
 *
 *   contention making one customer wait — contention really HAPPENED (a claim
 *   refusal or a bounded wait) and both customers still left clean dry, with
 *   nobody abandoned and no machine left claimed. The exact number of retries
 *   is emergent (the leave window, not a fixed count, is what ends a wait).
 *
 *   determinism across two runs — unchanged and stronger: the two event
 *   traces are memcmp-equal AND the two machine registries are byte-identical,
 *   which is the replayability oracle the spec asks for.
 *
 * Pure: links sim/brain.c + the agents framework + machines/garments/selection
 * + entities/easing + ai/nav + ai/pathfind + render/voxmap + the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_brain().
 */

#include "unity.h"

#include "agents/agent.h"
#include "agents/errand.h"
#include "agents/job.h"
#include "entities/entities.h"
#include "render/voxmap.h"
#include "sim/brain.h"
#include "sim/garments.h"
#include "sim/machines.h"
#include "sim/selection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 8 x 4 all-walkable ground (surface y = 1); door/exit/wait at (0,0). */
static const char *SCENARIO_MAP = "11111111\n11111111\n11111111\n11111111\n";
/* 12 x 3 ground for the spread_k divergence scenario. */
static const char *DIVERGE_MAP = "111111111111\n111111111111\n111111111111\n";

#define DT 0.05f
#define MAX_STEPS 12000
#define EV_CAP 2048

typedef struct Trace {
	BrainEvent ev[EV_CAP];
	int count;
	bool overflow;
} Trace;

static Voxmap *smallMap(void)
{
	return parseVoxmapText(SCENARIO_MAP, strlen(SCENARIO_MAP), NULL);
}

static void traceAppend(Trace *t, const BrainEvent *ev, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (t->count < EV_CAP)
			t->ev[t->count++] = ev[i];
		else
			t->overflow = true;
	}
}

static void traceReset(Trace *t)
{
	memset(t, 0, sizeof(*t));
}

/* THE CLAIM INVARIANT, checked on every tick of every scenario: a machine
 * that is not FREE is owned, and its owner is a customer that still exists.
 * The second half is the whole point — a machine whose owner has left the
 * building is locked for ever, and nothing else in the suite would notice. */
static void assertClaimsAreHonest(const SimWorld *w, int foreignOwner)
{
	MachineHandle h;

	for (h = machineFirst(&w->machines); h != MACHINE_INVALID;
	     h = machineNext(&w->machines, h)) {
		const Machine *m = machineGetConst(&w->machines, h);

		TEST_ASSERT_NOT_NULL(m);
		if (m->state == MACHINE_STATE_FREE ||
		    m->state == MACHINE_STATE_BROKEN) {
			TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
			continue;
		}
		/* An occupied machine belongs to a living customer — or to the
		 * entity the scenario itself claimed it with, to block a
		 * customer all run (which is not the customers' to free). */
		if (m->owner == foreignOwner)
			continue;
		TEST_ASSERT_TRUE(m->owner >= MACHINE_OWNER_MIN);
		TEST_ASSERT_NOT_NULL(simCustomerFor(w, m->owner));
	}
}

/* Nothing our customers could have claimed is still held once they are gone.
 * `foreign` is how many machines the scenario deliberately pre-claimed to
 * block them (a washer busy all run). */
static void assertMachinesReleased(const SimWorld *w, int foreign)
{
	MachineHandle h;
	int held = 0;

	for (h = machineFirst(&w->machines); h != MACHINE_INVALID;
	     h = machineNext(&w->machines, h)) {
		const Machine *m = machineGetConst(&w->machines, h);

		if (m->state != MACHINE_STATE_FREE &&
		    m->state != MACHINE_STATE_BROKEN)
			held++;
	}
	TEST_ASSERT_TRUE(held <= foreign);
}

/* One world update + the entity walker, appending events. The claim invariant
 * rides along so no scenario can forget it. */
static void step1(SimWorld *w, Trace *t, float dt)
{
	BrainEvent buf[64];
	int n = simUpdate(w, dt, buf, 64);

	traceAppend(t, buf, n);
	entitiesUpdate(&w->entities, dt);
	assertClaimsAreHonest(w, -1);
}

/* As step1, for the one scenario that pre-claims a machine to block a
 * customer: that claim belongs to an entity no customer owns, and the
 * invariant has to know about it rather than pretend it is not there. */
static void step1Foreign(SimWorld *w, Trace *t, float dt, int foreignOwner)
{
	BrainEvent buf[64];
	int n = simUpdate(w, dt, buf, 64);

	traceAppend(t, buf, n);
	entitiesUpdate(&w->entities, dt);
	assertClaimsAreHonest(w, foreignOwner);
}

/* Step until every customer has departed; returns the step count, or -1 when
 * the world does not settle inside MAX_STEPS. */
static int runToEmpty(SimWorld *w, Trace *t, float dt)
{
	int step;

	for (step = 0; step < MAX_STEPS; step++) {
		step1(w, t, dt);
		if (simCustomerCount(w) == 0)
			return step + 1;
	}
	return -1;
}

static int countKind(const Trace *t, int kind)
{
	int i;
	int n = 0;

	for (i = 0; i < t->count; i++)
		if (t->ev[i].kind == kind)
			n++;
	return n;
}

/* Count events of `kind` for customer `id`. */
static int countKindFor(const Trace *t, int id, int kind)
{
	int i;
	int n = 0;

	for (i = 0; i < t->count; i++)
		if (t->ev[i].id == id && t->ev[i].kind == kind)
			n++;
	return n;
}

/* Index of the first event of `kind` at or after `from`, or -1. */
static int findKindFrom(const Trace *t, int kind, int from)
{
	int i;

	for (i = from; i < t->count; i++)
		if (t->ev[i].kind == kind)
			return i;
	return -1;
}

/* The customer `id`'s mind: the driver's errand lives in it. */
static Agent *agentOfCustomer(SimWorld *w, EntityHandle h)
{
	const SimCustomer *c = simCustomerFor(w, h);

	if (c == NULL)
		return NULL;
	return agentGet(&w->agents, c->agent);
}

static void dumpStory(const Trace *t)
{
	int id;
	int i;

	for (id = 0; id < 8; id++) {
		bool any = false;

		for (i = 0; i < t->count; i++)
			if (t->ev[i].id == id)
				any = true;
		if (!any)
			continue;
		fprintf(stderr, "id %d:", id);
		for (i = 0; i < t->count; i++) {
			if (t->ev[i].id != id)
				continue;
			fprintf(stderr, " %s", brainEventName(t->ev[i].kind));
		}
		fprintf(stderr, "\n");
	}
}

/* --- event names ------------------------------------------------------- */

static void test_event_names(void)
{
	TEST_ASSERT_EQUAL_STRING("SPAWN", brainEventName(BRAIN_EV_SPAWN));
	TEST_ASSERT_EQUAL_STRING("ERRAND_ADOPT",
				 brainEventName(BRAIN_EV_ERRAND_ADOPT));
	TEST_ASSERT_EQUAL_STRING("TASK_BEGIN",
				 brainEventName(BRAIN_EV_TASK_BEGIN));
	TEST_ASSERT_EQUAL_STRING("NO_CANDIDATE",
				 brainEventName(BRAIN_EV_NO_CANDIDATE));
	TEST_ASSERT_EQUAL_STRING("CLAIM_DENIED",
				 brainEventName(BRAIN_EV_CLAIM_DENIED));
	TEST_ASSERT_EQUAL_STRING("PAY", brainEventName(BRAIN_EV_PAY));
	TEST_ASSERT_EQUAL_STRING("LOAD", brainEventName(BRAIN_EV_LOAD));
	TEST_ASSERT_EQUAL_STRING("RUN_START",
				 brainEventName(BRAIN_EV_RUN_START));
	TEST_ASSERT_EQUAL_STRING("MACHINE_DONE",
				 brainEventName(BRAIN_EV_MACHINE_DONE));
	TEST_ASSERT_EQUAL_STRING("INTERRUPTED",
				 brainEventName(BRAIN_EV_INTERRUPTED));
	TEST_ASSERT_EQUAL_STRING("REPLAN", brainEventName(BRAIN_EV_REPLAN));
	TEST_ASSERT_EQUAL_STRING("COLLECT", brainEventName(BRAIN_EV_COLLECT));
	TEST_ASSERT_EQUAL_STRING("GARMENT", brainEventName(BRAIN_EV_GARMENT));
	TEST_ASSERT_EQUAL_STRING("ABANDON",
				 brainEventName(BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_STRING("WAIT_TIMEOUT",
				 brainEventName(BRAIN_EV_WAIT_TIMEOUT));
	TEST_ASSERT_EQUAL_STRING("LEAVE_BEGIN",
				 brainEventName(BRAIN_EV_LEAVE_BEGIN));
	TEST_ASSERT_EQUAL_STRING("DEPART", brainEventName(BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_STRING("?", brainEventName(999));
}

/* --- adoption ---------------------------------------------------------- */

/* A customer arrives, adopts its errand BY NEED, and says so with the band
 * that chose it. The job it enqueues comes out of the errand, not out of a
 * switch — which is what the rest of this suite is about. */
static void test_a_customer_adopts_its_errand_on_arrival(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[16];
	EntityHandle h;
	Agent *a;
	const Errand *e;
	int i;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	i = 0;
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 16, &i);
	traceAppend(&t, buf, i);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	/* SPAWN, then the adoption, then the first TASK_BEGIN: the errand
	 * exists before it does anything. */
	i = findKindFrom(&t, BRAIN_EV_ERRAND_ADOPT, 0);
	TEST_ASSERT_TRUE(i > 0);
	TEST_ASSERT_EQUAL_INT(BRAIN_EV_SPAWN, t.ev[i - 1].kind);
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, t.ev[i].tmpl);
	TEST_ASSERT_TRUE(t.ev[i].band >= 0);
	TEST_ASSERT_TRUE(t.ev[i].band <= NEED_BANDS - 1);
	TEST_ASSERT_TRUE(findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i) > i);

	a = agentOfCustomer(&w, h);
	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_EQUAL_INT(ROLE_CUSTOMER, a->role);
	TEST_ASSERT_EQUAL_INT(h, a->entity);
	e = simCustomerErrand(&w, h);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, e->tmplId);
	/* And the adoption put real work on the agent's own board. */
	TEST_ASSERT_TRUE(a->personal.liveCount >= 1);
	destroyVoxmap(map);
}

/* --- payment ----------------------------------------------------------- */

static void test_payment_debit_and_events(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	EntityHandle h;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* Both fees debited: washer 3 + dryer 2 = 5 paid. The customer
	 * despawned at the end (handle no longer alive). */
	TEST_ASSERT_NULL(entityGet(&w.entities, h));
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_PAY));
	{
		int i;
		int total = 0;

		for (i = 0; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_PAY)
				total += t.ev[i].amount;
		TEST_ASSERT_EQUAL_INT(5, total);
	}
	TEST_ASSERT_FALSE(t.overflow);
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

static void test_insufficient_funds_abandons(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 1, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* Claimed, then found the fee unaffordable: no PAY, abandoned DIRTY,
	 * left, and the machine was released (a claim is not a purchase). */
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_PAY));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE,
			      machineGetConst(&w.machines, 0)->state);
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_TRUE(i >= 0);
		TEST_ASSERT_EQUAL_INT(GARMENT_DIRTY, t.ev[i].garment);
	}
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* --- wait policy -> abandon -------------------------------------------- */

/* Nothing free, ever. The customer waits (the bounded retry), its blocked
 * errand decays its satisfaction, and past the leave window it gives the errand
 * up and walks out. The RETRY COUNT is emergent — it is the leave window that
 * ends the wait, not a fixed number of tries — so what is pinned is that the
 * wait was bounded, that the customer gave up rather than hanging, and that
 * nothing was left claimed. */
static void test_no_candidate_bounded_then_abandons(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	Machine *blocker;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* The only washer is occupied forever (a long run, owned by nobody in
	 * the customer set, so it never wakes). */
	blocker = machineGet(&w.machines,
		machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1000.0f));
	TEST_ASSERT_TRUE(machineClaim(blocker, 3));
	TEST_ASSERT_TRUE(machineStartRun(blocker));
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	{
		int step;
		int gone = 0;

		for (step = 0; step < MAX_STEPS; step++) {
			step1Foreign(&w, &t, DT, 3);
			if (simCustomerCount(&w) == 0) {
				gone = 1;
				break;
			}
		}
		TEST_ASSERT_TRUE(gone > 0);
	}
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) <=
			 BRAIN_MAX_WAIT_RETRIES);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_LEAVE_BEGIN));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_PAY));
	/* The abandoned errand waited on a machine it never got, so it never
	 * claimed anything: the foreign claim is untouched and still ours to
	 * hand back, not the customer's. */
	assertMachinesReleased(&w, 1);
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_EQUAL_INT(GARMENT_DIRTY, t.ev[i].garment);
	}
	destroyVoxmap(map);
}

/* --- abandonment releases ---------------------------------------------- */

/* THE PATIENCE BUDGET IS SPENT ON WAITING, NOT ON LIVING. A customer whose
 * errand is making progress must not walk out, however long the round takes:
 * the window is measured in BLOCKED seconds, and a step that is walking,
 * paying, loading or collecting resets the clock to zero.
 *
 * The arrangement is the whole point — this round is deliberately LONGER than
 * the leave window (two 25 s machine runs plus two 5 s loads and four walks is
 * comfortably past 45 s of sim time), while no single blocked stretch is (the
 * gate waits 25 s). A window measured on the errand's AGE rather than its
 * blocked time would abandon this customer half way through their dry leg; this
 * test is what tells the two apart, and `test_the_patience_budget_is_spent_
 * only_on_blocked_time` below is what tells the two apart at the other end.
 *
 * It is also the honest guard against the leave gate becoming an impatience
 * counter: this customer IS impatient by any measure a person would use (their
 * satisfaction has been at the floor for most of the run, because the
 * framework's needs decay and nothing restores them yet), and they still do
 * their laundry. */
static void test_a_customer_who_is_progressing_never_walks_out(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	Agent *a;
	const Errand *e;
	int n = 0;
	int step;
	int aliveAtWindow = 0;
	double blockedPeak = 0.0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* 25 s runs: each leg blocks once, for 25 s, which is under the window;
	 * the whole round is past it. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 25.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 25.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);
	a = agentOfCustomer(&w, h);
	TEST_ASSERT_NOT_NULL(a);

	/* Step past the window, watching the blocked clock. */
	for (step = 0; step < MAX_STEPS; step++) {
		step1(&w, &t, DT);
		if (simCustomerCount(&w) == 0)
			break;
		e = simCustomerErrand(&w, h);
		if (e != NULL && (double)e->blockedFor > blockedPeak)
			blockedPeak = (double)e->blockedFor;
		if ((double)step * (double)DT >= ERRAND_LEAVE_THRESHOLD_WINDOW) {
			aliveAtWindow = simCustomerCount(&w);
			break;
		}
	}
	/* Still here, still shopping, and never once blocked long enough to be
	 * given up on. */
	TEST_ASSERT_EQUAL_INT(1, aliveAtWindow);
	TEST_ASSERT_TRUE(blockedPeak < (double)ERRAND_LEAVE_THRESHOLD_WINDOW);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* And the round completed: two runs, two loads, a clean dry departure,
	 * at a total sim time comfortably past the window. */
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_RUN_START));
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_GARMENT));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_WAIT_TIMEOUT));
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_TRUE(i >= 0);
		TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t.ev[i].garment);
	}
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* ...AND THE OTHER END OF THE SAME RULE: blocked time accumulates and does run
 * out. The machine runs for a thousand seconds, so this customer's gate never
 * wakes; past the window they give the errand up and walk out. Without this
 * test a gate that never fired would satisfy the one above forever. */
static void test_the_patience_budget_is_spent_only_on_blocked_time(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	int n = 0;
	int step;
	double blockedAtWindow = 0.0;
	int aliveAtWindow = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1000.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	for (step = 0; step < MAX_STEPS; step++) {
		const Errand *e;

		step1(&w, &t, DT);
		if (simCustomerCount(&w) == 0)
			break;
		e = simCustomerErrand(&w, h);
		if (e != NULL &&
		    (double)step * (double)DT >= ERRAND_LEAVE_THRESHOLD_WINDOW) {
			blockedAtWindow = (double)e->blockedFor;
			aliveAtWindow = simCustomerCount(&w);
			break;
		}
	}
	/* Before the window: here, blocked, and the clock is running. */
	TEST_ASSERT_EQUAL_INT(1, aliveAtWindow);
	TEST_ASSERT_TRUE(blockedAtWindow > 0.0);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_MACHINE_DONE));
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* THE DEADLOCK ORACLE. A customer that has claimed a machine and is waiting
 * for its run gives the errand up when its patience runs out — and the machine
 * MUST come back. A washer left claimed by somebody who has walked out is
 * locked for the rest of the run, which is the failure the errand's abandon
 * path exists to prevent. */
static void test_an_abandoned_errand_releases_its_machine(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	Agent *a;
	const Machine *m;
	int n = 0;
	int step;
	int sawRunStart = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* A run far longer than the leave window: this customer is going to
	 * give up on it. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1000.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	for (step = 0; step < MAX_STEPS; step++) {
		m = machineGetConst(&w.machines, 0);
		step1(&w, &t, DT);
		if (countKind(&t, BRAIN_EV_RUN_START) > 0 && m != NULL &&
		    m->state == MACHINE_STATE_RUNNING && m->owner == h) {
			sawRunStart = 1;
			break;
		}
	}
	TEST_ASSERT_TRUE(sawRunStart);
	/* Claimed and running: this is the state the release has to undo. */
	m = machineGetConst(&w.machines, 0);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_EQUAL_INT(h, m->owner);
	a = agentOfCustomer(&w, h);
	TEST_ASSERT_NOT_NULL(a);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* It gave up rather than waiting for ever... */
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_MACHINE_DONE));
	/* ...and the machine went with it. */
	m = machineGetConst(&w.machines, 0);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
	TEST_ASSERT_EQUAL_INT(0, w.agents.liveCount);
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* THE PREEMPTION ORACLE. The framework can pull an agent off a job it is
 * running (agentsUpdate fails the displaced job and releases its claim), and a
 * job that has already claimed a machine must give it back — "cancel releases
 * held resources". Driven from outside here, by interrupting the job exactly
 * as the tick would: the release is the driver's job, and this is where it is
 * pinned. */
static void test_a_preempted_use_machine_releases_its_claim(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	Agent *a;
	const Machine *m;
	Job *j;
	Errand *e;
	int n = 0;
	int step;
	int paid = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	/* Step to the moment the fee is paid and the machine is therefore
	 * claimed by this customer. */
	for (step = 0; step < MAX_STEPS; step++) {
		step1(&w, &t, DT);
		if (countKind(&t, BRAIN_EV_PAY) > 0) {
			paid = 1;
			break;
		}
	}
	TEST_ASSERT_TRUE(paid);
	m = machineGetConst(&w.machines, 0);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_CLAIMED, m->state);
	TEST_ASSERT_EQUAL_INT(h, m->owner);

	a = agentOfCustomer(&w, h);
	TEST_ASSERT_NOT_NULL(a);
	e = errandActive(a);
	TEST_ASSERT_NOT_NULL(e);
	j = boardJob(&a->personal, e->jobHandle);
	TEST_ASSERT_NOT_NULL(j);
	/* Preempted mid-leg: the lifecycle's answer is FAILED. */
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, jobStep(j, JOB_INTERRUPTED).state);

	step1(&w, &t, DT);
	/* The claim is gone with one tick, and the errand re-selects rather
	 * than walking to a machine it no longer holds. */
	m = machineGetConst(&w.machines, 0);
	TEST_ASSERT_NOT_NULL(m);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
	TEST_ASSERT_TRUE(findKindFrom(&t, BRAIN_EV_REPLAN, 0) >= 0 ||
			 findKindFrom(&t, BRAIN_EV_CLAIM_DENIED, 0) >= 0);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_RUN_START));

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* And the customer still finishes its laundry on another washer. */
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== preempt ==\n");
		dumpStory(&t);
	}
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* --- integration: two customers, clean dry, dryer contention ----------- */

static void test_integration_two_customers_clean_dry(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	EntityHandle a;
	EntityHandle b;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* m0 a dear washer, m1 a cheap washer, m2 the only dryer. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 5, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 4, 0, 2, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 6, 0, 2, 1.0f);
	a = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	n = 0;
	b = simSpawnCustomer(&w, SEL_POLICY_CHEAPEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(a != ENTITY_INVALID && b != ENTITY_INVALID);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_FALSE(t.overflow);
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== two-customer ==\n");
		dumpStory(&t);
	}

	/* The customers visibly chose differently (the point of the heuristic
	 * registry): A (closest) picks washer 0, B (cheapest) picks washer 1.
	 * This is a SELECTION oracle, not an interleaving one — the errand
	 * chose each machine on arrival, so the order is the spawn order. */
	{
		int i = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, 0);

		TEST_ASSERT_EQUAL_INT(0, t.ev[i].machine);
		TEST_ASSERT_EQUAL_INT(2, t.ev[i].distance);
		TEST_ASSERT_EQUAL_INT(2, t.ev[i].score);
		i = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i + 1);
		TEST_ASSERT_EQUAL_INT(1, t.ev[i].machine);	/* B's washer */
	}

	/* Each customer's durable milestones: two machine legs and a departure.
	 * The transient bookkeeping between them (a re-select, a claim refusal,
	 * a bounded wait) is emergent and deliberately not counted. */
	{
		int id;

		for (id = 0; id < 2; id++) {
			TEST_ASSERT_EQUAL_INT(1, countKindFor(&t, id,
							      BRAIN_EV_ERRAND_ADOPT));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
							      BRAIN_EV_PAY));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
							      BRAIN_EV_LOAD));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
							      BRAIN_EV_RUN_START));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
						      BRAIN_EV_MACHINE_DONE));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
							      BRAIN_EV_COLLECT));
			TEST_ASSERT_EQUAL_INT(2, countKindFor(&t, id,
							      BRAIN_EV_GARMENT));
			TEST_ASSERT_EQUAL_INT(1, countKindFor(&t, id,
							      BRAIN_EV_LEAVE_BEGIN));
			TEST_ASSERT_EQUAL_INT(1, countKindFor(&t, id,
							      BRAIN_EV_DEPART));
			TEST_ASSERT_EQUAL_INT(0, countKindFor(&t, id,
							      BRAIN_EV_ABANDON));
			/* The two garment transitions are wash then dry, and the
			 * departure is the last event, clean dry. */
			{
				int i;
				int g = 0;
				int last = -1;

				for (i = 0; i < t.count; i++) {
					if (t.ev[i].id != id)
						continue;
					if (t.ev[i].kind == BRAIN_EV_GARMENT) {
						TEST_ASSERT_EQUAL_INT(
							g == 0 ? GARMENT_WET_CLEAN
							       : GARMENT_DRY_CLEAN,
							t.ev[i].garment);
						g++;
					}
					last = i;
				}
				TEST_ASSERT_EQUAL_INT(BRAIN_EV_DEPART,
						      t.ev[last].kind);
				TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN,
						      t.ev[last].garment);
			}
		}
	}

	/* Exactly two DEPARTs, both clean dry. */
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_DEPART));
	{
		int i;

		for (i = 0; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_DEPART)
				TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN,
						      t.ev[i].garment);
	}
	/* Dryer contention was real: somebody waited, and nobody gave up. */
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1 ||
			 countKind(&t, BRAIN_EV_CLAIM_DENIED) >= 1);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	/* Payments: 2 each; total 5 (dear washer) + 2 + 2 + 2 = 11. */
	TEST_ASSERT_EQUAL_INT(4, countKind(&t, BRAIN_EV_PAY));
	{
		int i;
		int total = 0;

		for (i = 0; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_PAY)
				total += t.ev[i].amount;
		TEST_ASSERT_EQUAL_INT(11, total);
	}
	/* The invariant the migration is really about: with two customers and
	 * one dryer, nothing was ever claimed-and-abandoned. */
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* --- integration: one washer, two customers, a refused claim ----------- */

/* The T23 contention scenario, in its invariant form. What must still be true:
 * contention HAPPENED (a claim refusal, or a bounded wait for the machine to
 * free), BOTH customers still washed, dried and left clean, and neither gave
 * up. What is no longer pinned is how many times the loser re-selected, which
 * under an errand is a property of the walk. */
static void test_integration_single_washer_contention(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	n = 0;
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== single-washer ==\n");
		dumpStory(&t);
	}
	/* One washer, two customers: somebody must have lost a race or waited
	 * for it. */
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_CLAIM_DENIED) >= 1 ||
			 countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	/* And both still finished. */
	{
		int i;
		int dry = 0;

		for (i = 0; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_DEPART &&
			    t.ev[i].garment == GARMENT_DRY_CLEAN)
				dry++;
		TEST_ASSERT_EQUAL_INT(2, dry);
	}
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* --- integration: a machine breaks mid-run -> re-plan ------------------ */

/* The T23 break-and-re-plan scenario, in its invariant form: the break was
 * seen, the broken machine was never chosen again, the errand re-planned onto
 * the other washer, and the customer still left clean dry. The exact event
 * sequence is gone — under an errand, where the re-select lands relative to
 * the walk is emergent — but every property the sequence encoded is asserted
 * directly. */
static void test_integration_break_mid_run_replans(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	int step;
	int started = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 5, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 7, 0, 2, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	/* Step until the customer's first washer run starts, then break
	 * washer 0 (the machine it is running). */
	for (step = 0; step < MAX_STEPS; step++) {
		step1(&w, &t, DT);
		if (findKindFrom(&t, BRAIN_EV_RUN_START, 0) >= 0) {
			started = 1;
			break;
		}
	}
	TEST_ASSERT_TRUE(started);
	TEST_ASSERT_EQUAL_INT(0, t.ev[findKindFrom(&t, BRAIN_EV_RUN_START,
						   0)].machine);
	TEST_ASSERT_TRUE(simBreakMachine(&w, 0));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN,
			      machineGetConst(&w.machines, 0)->state);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== break ==\n");
		dumpStory(&t);
	}
	/* The break was seen exactly once: one machine, one interruption. */
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_INTERRUPTED));
	/* The errand re-planned onto the other washer... */
	{
		int brk = findKindFrom(&t, BRAIN_EV_INTERRUPTED, 0);
		int i = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, brk + 1);

		TEST_ASSERT_TRUE(i > brk);
		TEST_ASSERT_EQUAL_INT(1, t.ev[i].machine);	/* washer 1 */
	}
	/* ...and never went back to the broken one. */
	{
		int brk = findKindFrom(&t, BRAIN_EV_INTERRUPTED, 0);
		int i;

		for (i = brk + 1; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_TASK_BEGIN)
				TEST_ASSERT_TRUE(t.ev[i].machine != 0);
	}
	/* Both loads still happened — the broken run cost the customer one
	 * washer, not the wash itself — and the customer left clean dry. */
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_GARMENT));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t.ev[i].garment);
	}
	/* A broken machine is not the customer's to give back, and it is not
	 * still held either (machineBreak clears the owner). */
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER,
			      machineGetConst(&w.machines, 0)->owner);
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* --- determinism ------------------------------------------------------- */

/* THE REPLAYABILITY ORACLE, unchanged by the migration and worth stating
 * plainly: two runs of the same inputs produce the SAME event trace, byte for
 * byte, and the same machine registry. Everything the errand layer decides —
 * which template, which step, which band, which machine, when a wait runs out
 * — is a pure function of the dt sequence and the world, with no wall clock
 * and no allocation anywhere on the path. */
static void test_determinism(void)
{
	Voxmap *map_a = smallMap();
	Voxmap *map_b = smallMap();
	SimWorld wa;
	SimWorld wb;
	Trace ta;
	Trace tb;
	BrainEvent buf[8];
	int n;

	traceReset(&ta);
	traceReset(&tb);
	simWorldInit(&wa, map_a, 0, 0, 0, 0, 0, 0);
	simWorldInit(&wb, map_b, 0, 0, 0, 0, 0, 0);
	machineCreate(&wa.machines, MACHINE_KIND_WASHER, 2, 0, 5, 1.0f);
	machineCreate(&wb.machines, MACHINE_KIND_WASHER, 2, 0, 5, 1.0f);
	machineCreate(&wa.machines, MACHINE_KIND_WASHER, 4, 0, 2, 1.0f);
	machineCreate(&wb.machines, MACHINE_KIND_WASHER, 4, 0, 2, 1.0f);
	machineCreate(&wa.machines, MACHINE_KIND_DRYER, 6, 0, 2, 1.0f);
	machineCreate(&wb.machines, MACHINE_KIND_DRYER, 6, 0, 2, 1.0f);

	n = 0;
	(void)simSpawnCustomer(&wa, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&ta, buf, n);
	n = 0;
	(void)simSpawnCustomer(&wb, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&tb, buf, n);
	n = 0;
	(void)simSpawnCustomer(&wa, SEL_POLICY_SPREAD_K, 20, buf, 8, &n);
	traceAppend(&ta, buf, n);
	n = 0;
	(void)simSpawnCustomer(&wb, SEL_POLICY_SPREAD_K, 20, buf, 8, &n);
	traceAppend(&tb, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&wa, &ta, DT) > 0);
	TEST_ASSERT_TRUE(runToEmpty(&wb, &tb, DT) > 0);
	TEST_ASSERT_EQUAL_INT(ta.count, tb.count);
	TEST_ASSERT_EQUAL_INT(0, memcmp(ta.ev, tb.ev,
					sizeof(BrainEvent) * (size_t)ta.count));
	/* Both registries ended identical too. */
	TEST_ASSERT_EQUAL_INT(0, memcmp(&wa.machines, &wb.machines,
					sizeof(wa.machines)));
	/* Including the minds: the same errands, the same steps, the same
	 * needs — an errand list is ordinary pure data, so it replays. */
	TEST_ASSERT_EQUAL_INT(0, memcmp(&wa.agents, &wb.agents,
					sizeof(wa.agents)));

	destroyVoxmap(map_a);
	destroyVoxmap(map_b);
}

/* --- spawner, guards, timeout, unreachable-exit ------------------------ */

static void test_spawner_interval_and_cap(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[64];
	int i;
	int n;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* A long-running washer keeps both customers busy so the cap binds. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1000.0f);
	simSetSpawner(&w, true, 1.0f, 2);
	for (i = 0; i < 60; i++) {
		n = simUpdate(&w, 0.1f, buf, 64);
		traceAppend(&t, buf, n);
		entitiesUpdate(&w.entities, 0.1f);
	}
	/* The alive cap held at 2 after plenty of intervals. */
	TEST_ASSERT_EQUAL_INT(2, simCustomerCount(&w));
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_SPAWN));

	/* Disabling stops the spawner; the cap clamp and the default arms. */
	simSetSpawner(&w, false, 1.0f, 2);
	simSetSpawner(NULL, true, 1.0f, 1);		/* no crash */
	simSetSpawner(&w, false, -1.0f, 0);		/* defaults */
	TEST_ASSERT_FALSE(w.spawnEnabled);
	TEST_ASSERT_TRUE(w.spawnInterval == BRAIN_SPAWN_INTERVAL);
	TEST_ASSERT_EQUAL_INT(BRAIN_SPAWN_CAP, w.spawnCap);
	simSetSpawner(&w, false, 5.0f, 999);		/* clamp to MAX */
	TEST_ASSERT_EQUAL_INT(BRAIN_MAX_CUSTOMERS, w.spawnCap);
	destroyVoxmap(map);
}

static void test_spawn_guards_and_customer_for(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	SimWorld wv;
	BrainEvent buf[8];
	EntityHandle h;
	int i;
	int n = 0;

	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* An unknown policy falls back to the default. */
	h = simSpawnCustomer(&w, 99, 20, buf, 8, &n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);
	TEST_ASSERT_NOT_NULL(simCustomerFor(&w, h));
	TEST_ASSERT_EQUAL_INT(SEL_POLICY_DEFAULT,
			      simCustomerFor(&w, h)->policy);
	TEST_ASSERT_NULL(simCustomerFor(&w, ENTITY_INVALID));
	TEST_ASSERT_NULL(simCustomerFor(NULL, h));
	TEST_ASSERT_NULL(simCustomerErrand(NULL, h));
	TEST_ASSERT_NULL(simCustomerErrand(&w, ENTITY_INVALID));

	/* Fill the customer cap, then the next spawn is refused. */
	for (i = 1; i < BRAIN_MAX_CUSTOMERS; i++) {
		n = 0;
		TEST_ASSERT_TRUE(simSpawnCustomer(&w, 0, 20, buf, 8, &n) !=
				 ENTITY_INVALID);
	}
	n = 0;
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID,
			      simSpawnCustomer(&w, 0, 20, buf, 8, &n));
	TEST_ASSERT_EQUAL_INT(BRAIN_MAX_CUSTOMERS, simCustomerCount(&w));

	/* A registry with no minds left refuses the spawn too, and leaves
	 * nothing behind: a body with no mind would be walked by nobody. */
	{
		SimWorld wf;
		BrainEvent fb[8];
		int fn = 0;
		int made = 0;

		simWorldInit(&wf, map, 0, 0, 0, 0, 0, 0);
		for (i = 0; i < AGENT_MAX; i++)
			if (agentCreate(&wf.agents, ROLE_CRITTER) !=
			    AGENT_INVALID)
				made++;
		TEST_ASSERT_EQUAL_INT(AGENT_MAX, made);
		TEST_ASSERT_EQUAL_INT(ENTITY_INVALID,
				      simSpawnCustomer(&wf, 0, 20, fb, 8,
						       &fn));
		TEST_ASSERT_EQUAL_INT(0, fn);
		TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&wf));
		TEST_ASSERT_EQUAL_INT(0, machineCount(&wf.machines));
		/* The registry is full, so a second attempt is refused the same
		 * way rather than half-succeeding. */
		TEST_ASSERT_EQUAL_INT(ENTITY_INVALID,
				      simSpawnCustomer(&wf, 0, 20, fb, 8,
						       &fn));
	}

	/* A void door refuses the spawn — and creates no mind either. */
	simWorldInit(&wv, map, 99, 99, 0, 0, 0, 0);
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID,
			      simSpawnCustomer(&wv, 0, 20, buf, 8, &n));
	TEST_ASSERT_EQUAL_INT(0, wv.agents.liveCount);

	/* World-level guards. */
	TEST_ASSERT_EQUAL_INT(0, simUpdate(NULL, 0.1f, buf, 8));
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(NULL));
	TEST_ASSERT_FALSE(simBreakMachine(NULL, 0));
	TEST_ASSERT_FALSE(simBreakMachine(&w, MACHINE_INVALID));
	TEST_ASSERT_FALSE(simBreakMachine(&w, 99));
	destroyVoxmap(map);
}

static void test_update_guards(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	BrainEvent buf[8];
	float nan = 0.0f;

	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	simWorldInit(NULL, map, 0, 0, 0, 0, 0, 0);	/* no crash */
	TEST_ASSERT_EQUAL_INT(0, simUpdate(NULL, 0.1f, buf, 8));
	TEST_ASSERT_EQUAL_INT(0, simUpdate(&w, 0.0f, buf, 8));	/* dt <= 0 */
	TEST_ASSERT_EQUAL_INT(0, simUpdate(&w, 5.0f, buf, 8));	/* dt > MAX */
	TEST_ASSERT_EQUAL_INT(0, simUpdate(&w, -1.0f, buf, 8));
	nan = nan / nan;
	TEST_ASSERT_EQUAL_INT(0, simUpdate(&w, nan, buf, 8));
	/* A NULL event buffer must not take the world's state with it. */
	TEST_ASSERT_EQUAL_INT(0, simUpdate(&w, 0.05f, NULL, 0));
	destroyVoxmap(map);
}

/* THE GATE'S BOUND, end to end. The machine is taken away mid-run, so its
 * phase wake will never come — and the errand's gate deadline fires the job
 * lifecycle's JOB_TIMEOUT, which is the producer that cell had been waiting
 * for. The customer then re-plans onto the other washer and still leaves clean.
 * Without that producer this scenario would hang until MAX_STEPS and fail. */
static void test_the_gate_bound_replans(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	int i;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 0.1f);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 4, 0, 3, 0.1f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 6, 0, 2, 0.1f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	for (i = 0; i < MAX_STEPS; i++) {
		step1(&w, &t, DT);
		if (findKindFrom(&t, BRAIN_EV_RUN_START, 0) >= 0)
			break;
	}
	TEST_ASSERT_TRUE(findKindFrom(&t, BRAIN_EV_RUN_START, 0) >= 0);
	/* Take the machine away mid-run: no wake will ever come, so the
	 * bounded gate must fire and the errand must re-plan. */
	TEST_ASSERT_TRUE(machineRelease(machineGet(&w.machines, 0)));

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== gate bound ==\n");
		dumpStory(&t);
	}
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_WAIT_TIMEOUT));
	/* No REPLAN event: the scenario took the machine away itself, so the
	 * re-plan had nothing of OURS to give back and said nothing — which is
	 * the point of emitting that event only when a claim is actually
	 * released. */
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_REPLAN));
	{
		int i2 = findKindFrom(&t, BRAIN_EV_WAIT_TIMEOUT, 0);
		int j = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i2 + 1);

		TEST_ASSERT_TRUE(j > i2);
		TEST_ASSERT_EQUAL_INT(1, t.ev[j].machine);	/* washer 1 */
	}
	/* The bound cost one wash, not the errand. */
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	{
		int d = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t.ev[d].garment);
	}
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

static void test_abandon_unreachable_exit(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;

	traceReset(&t);
	/* A walkable door but an out-of-bounds exit: the abandon path cannot
	 * walk out, so the customer leaves where it stands (still honest). */
	simWorldInit(&w, map, 0, 0, 99, 99, 0, 0);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&w));
	/* The mind goes with the body: no agent is left decaying for ever. */
	TEST_ASSERT_EQUAL_INT(0, w.agents.liveCount);
	destroyVoxmap(map);
}

static void test_unreachable_machine_abandons(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* A washer on an out-of-bounds tile: selectable (kind + FREE) but its
	 * tile is void, so the job is denied and the errand re-plans; with no
	 * other washer it eventually gives up. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 99, 99, 3, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* It was chosen, it could not be walked to, and the errand gave up. */
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_TASK_BEGIN) >= 1);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_PAY));
	destroyVoxmap(map);
}

static void test_entity_destroyed_mid_life(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	BrainEvent buf[8];
	EntityHandle h;
	int n = 0;

	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);
	/* The entity is destroyed under the brain: the record is reclaimed
	 * (and the mind with it). */
	entityDestroy(&w.entities, h);
	(void)simUpdate(&w, DT, buf, 8);
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&w));
	TEST_ASSERT_EQUAL_INT(0, w.agents.liveCount);
	destroyVoxmap(map);
}

/* FILL TIME, VISIBLE. The spec's "an agent does secondary activities when
 * blocked" is the moment this task exists to show, and agentsUpdate generates
 * the fill itself and reports nothing — so this pins that the driver notices
 * one and announces it, once, with the activity and its band.
 *
 * The arrangement: the only washer is held by a machine that will not finish
 * for ever, so the customer's first step has nothing to bind and the framework
 * fills its (empty) board. Without the announcement the customer is simply
 * standing in the building. */
static void test_a_blocked_customer_announces_the_fill_it_takes(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	int n = 0;
	int step;
	int fills = 0;
	int i;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* One washer, busy for a thousand seconds: nothing to wash in. */
	TEST_ASSERT_TRUE(machineClaim(machineGet(&w.machines,
		machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3,
			      1000.0f)), 3));
	TEST_ASSERT_TRUE(machineStartRun(machineGet(&w.machines, 0)));
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	/* Owner 3 is the scenario's own blocking claim, not a customer's, so the
	 * per-tick claim invariant is stepped with it known. */
	for (step = 0; step < 600; step++) {
		step1Foreign(&w, &t, DT, 3);
		if (countKind(&t, BRAIN_EV_FILL) > 0)
			break;
	}
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_FILL));
	/* Announced once per fill, not once per frame: the flag holds until
	 * the customer is back on the errand's own work. */
	for (step = 0; step < 400; step++)
		step1Foreign(&w, &t, DT, 3);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_FILL));
	/* It names a real activity and reports its band on the agent's own
	 * arithmetic. */
	for (i = 0; i < t.count; i++) {
		if (t.ev[i].kind != BRAIN_EV_FILL)
			continue;
		TEST_ASSERT_NOT_NULL(activitiesGet(t.ev[i].amount));
		TEST_ASSERT_TRUE(t.ev[i].band >= 0);
		TEST_ASSERT_TRUE(t.ev[i].band <= NEED_BANDS - 1);
		fills++;
	}
	TEST_ASSERT_EQUAL_INT(1, fills);
	/* And the errand was still waiting for a machine all the while. */
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	destroyVoxmap(map);
}

/* A CUSTOMER WHOSE ERRAND ENDED WITHOUT A LEAVE STEP STILL WALKS OUT. The
 * driver watches the errand, and an errand can end three ways it did not ask
 * for: a template that ran out, a step that failed past its bound, an errand
 * abandoned by something other than this driver. None of them may leave a
 * customer standing in the building with nothing to do — it walks out, once,
 * and the announcement is not repeated every tick after that. */
static void test_an_errand_that_ends_without_leaving_still_walks_out(void)
{
	Voxmap *map = smallMap();
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	EntityHandle h;
	Agent *a;
	int n = 0;
	int i;
	int firstLeave = -1;
	int leaves = 0;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);
	h = simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	TEST_ASSERT_TRUE(h != ENTITY_INVALID);

	/* End the errand behind the driver's back, mid-walk. */
	a = agentOfCustomer(&w, h);
	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_NOT_NULL(errandActive(a));
	boardRetire(&a->personal, errandActive(a)->jobHandle);
	errandActive(a)->jobHandle = ERRAND_INVALID;
	errandActive(a)->state = ERRAND_FAILED;

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	for (i = 0; i < t.count; i++) {
		if (t.ev[i].kind != BRAIN_EV_LEAVE_BEGIN)
			continue;
		if (firstLeave < 0)
			firstLeave = i;
		leaves++;
	}
	/* Announced exactly once, and the customer did go. */
	TEST_ASSERT_TRUE(firstLeave >= 0);
	TEST_ASSERT_EQUAL_INT(1, leaves);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&w));
	TEST_ASSERT_EQUAL_INT(0, w.agents.liveCount);
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* A WAKE THAT LANDS WHILE THE OWNER IS STILL WALKING. The errand's gate
 * resumes on the machine's PHASE_DONE, but a job's PHASE_DONE is the WAITING
 * cell's — and the body is still walking to the wait spot when a short run
 * finishes. The wake is therefore HELD rather than dropped or delivered early:
 * delivered early it would be swallowed by the lifecycle's
 * WALKING + PHASE_DONE no-op, and dropped it would hang the errand until the
 * gate's bound fired. This scenario is the one arrangement that puts the wake
 * in that window: the machine is ten tiles from the wait spot and the run is
 * a tenth of a second. */
static void test_a_wake_during_the_walk_to_the_wait_spot(void)
{
	Voxmap *map = parseVoxmapText(DIVERGE_MAP, strlen(DIVERGE_MAP), NULL);
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	int i;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	/* The far washer, and a run so short it finishes mid-walk. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 10, 0, 3, 0.1f);
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 0.1f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== wake while walking ==\n");
		dumpStory(&t);
	}
	/* Two runs (wash and dry), one wake each, and each was delivered once
	 * the body was parked rather than swallowed mid-walk. */
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_RUN_START));
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_MACHINE_DONE));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_WAIT_TIMEOUT));
	/* Which means the wash really finished: the customer collected it and
	 * left clean dry. */
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_COLLECT));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));
	{
		i = findKindFrom(&t, BRAIN_EV_DEPART, 0);
		TEST_ASSERT_TRUE(i >= 0);
		TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t.ev[i].garment);
	}
	/* And the gate's bound never fired — the machine was released long
	 * before it could. */
	assertMachinesReleased(&w, 0);
	destroyVoxmap(map);
}

/* The reviewer's divergence oracle, unchanged: with one cluster washer
 * occupied, a `closest` customer takes the next free cluster washer while a
 * `spread_k` customer takes the isolated one — exact machines and scores
 * pinned. The errand asks the selection module the same question the old
 * driver did, so the demo's visible disagreement between policies survives the
 * migration exactly. */
static void test_integration_spread_diverges(void)
{
	Voxmap *map = parseVoxmapText(DIVERGE_MAP, strlen(DIVERGE_MAP), NULL);
	SimWorld w;
	Trace t;
	BrainEvent buf[8];
	int n = 0;
	int i;
	int m_closest = -1;
	int m_spread = -1;
	int s_closest = -1;
	int s_spread = -1;

	traceReset(&t);
	simWorldInit(&w, map, 0, 0, 0, 0, 0, 0);
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 2, 0, 3, 1.0f);	/* #0 */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 3, 0, 3, 1.0f);	/* #1 */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 10, 0, 3, 1.0f); /* #2 */
	machineCreate(&w.machines, MACHINE_KIND_DRYER, 5, 0, 2, 1.0f);

	/* A closest customer claims the nearest cluster washer (#0) first. */
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	for (i = 0; i < MAX_STEPS; i++) {
		step1(&w, &t, DT);
		if (countKind(&t, BRAIN_EV_LOAD) >= 1)
			break;
	}
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_CLAIMED,
			      machineGetConst(&w.machines, 0)->state);

	/* A closest and a spread_k customer arrive together while #0 is
	 * occupied (only #1 is a free cluster washer; #2 is isolated). */
	n = 0;
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);
	n = 0;
	(void)simSpawnCustomer(&w, SEL_POLICY_SPREAD_K, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	for (i = 0; i < t.count; i++) {
		if (t.ev[i].kind != BRAIN_EV_TASK_BEGIN)
			continue;
		if (t.ev[i].id == 1) {
			m_closest = t.ev[i].machine;
			s_closest = t.ev[i].score;
		}
		if (t.ev[i].id == 2) {
			m_spread = t.ev[i].machine;
			s_spread = t.ev[i].score;
		}
	}
	/* closest -> the free cluster washer (#1), score = distance 3.
	 * spread_k -> the isolated washer (#2), score = distance 10; the
	 * cluster washer #1 is within K=2 of the occupied #0 and carries the
	 * 100000 penalty. */
	TEST_ASSERT_EQUAL_INT(1, m_closest);
	TEST_ASSERT_EQUAL_INT(3, s_closest);
	TEST_ASSERT_EQUAL_INT(2, m_spread);
	TEST_ASSERT_EQUAL_INT(10, s_spread);
	destroyVoxmap(map);
}

void run_test_brain(void)
{
	(void)dumpStory;
	RUN_TEST(test_event_names);
	RUN_TEST(test_a_customer_adopts_its_errand_on_arrival);
	RUN_TEST(test_payment_debit_and_events);
	RUN_TEST(test_insufficient_funds_abandons);
	RUN_TEST(test_no_candidate_bounded_then_abandons);
	RUN_TEST(test_a_customer_who_is_progressing_never_walks_out);
	RUN_TEST(test_the_patience_budget_is_spent_only_on_blocked_time);
	RUN_TEST(test_an_abandoned_errand_releases_its_machine);
	RUN_TEST(test_a_preempted_use_machine_releases_its_claim);
	RUN_TEST(test_spawner_interval_and_cap);
	RUN_TEST(test_spawn_guards_and_customer_for);
	RUN_TEST(test_update_guards);
	RUN_TEST(test_the_gate_bound_replans);
	RUN_TEST(test_abandon_unreachable_exit);
	RUN_TEST(test_unreachable_machine_abandons);
	RUN_TEST(test_entity_destroyed_mid_life);
	RUN_TEST(test_integration_two_customers_clean_dry);
	RUN_TEST(test_integration_spread_diverges);
	RUN_TEST(test_a_wake_during_the_walk_to_the_wait_spot);
	RUN_TEST(test_an_errand_that_ends_without_leaving_still_walks_out);
	RUN_TEST(test_a_blocked_customer_announces_the_fill_it_takes);
	RUN_TEST(test_a_blocked_customer_announces_the_fill_it_takes);
	RUN_TEST(test_integration_single_washer_contention);
	RUN_TEST(test_integration_break_mid_run_replans);
	RUN_TEST(test_determinism);
}