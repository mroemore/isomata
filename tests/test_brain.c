/*
 * Customer-brain tests (CTOL rung 1: unit + boundary), including the T23
 * acceptance oracle: deterministic fake-dt integration scenarios.
 *
 * Coverage:
 *   - event names;
 *   - payment: debit, exact fee, insufficient funds -> abandon (leave DIRTY);
 *   - the no-candidate wait policy: bounded retries -> abandon;
 *   - the integration story: two customers wash+dry and leave clean-dry under
 *     dryer contention (a NO_CANDIDATE retry then success);
 *   - one washer, two customers: a refused claim, a bounded wait, then success;
 *   - a machine broken mid-run: INTERRUPTED -> re-plan -> succeeds on another;
 *   - determinism: two identical runs produce byte-identical event traces.
 *
 * Pure: links sim/brain.c + machines/garments/plan/selection + entities/easing
 * + ai/fsm + ai/nav + ai/pathfind + render/voxmap + the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_brain().
 */

#include "unity.h"

#include "ai/fsm.h"
#include "entities/entities.h"
#include "render/voxmap.h"
#include "sim/brain.h"
#include "sim/garments.h"
#include "sim/machines.h"
#include "sim/plan.h"
#include "sim/selection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 8 x 4 all-walkable ground (surface y = 1); door/exit/wait at (0,0). */
static const char *SCENARIO_MAP = "11111111\n11111111\n11111111\n11111111\n";

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

/* One world update + the entity walker, appending events. */
static void step1(SimWorld *w, Trace *t, float dt)
{
	BrainEvent buf[64];
	int n = simUpdate(w, dt, buf, 64);

	traceAppend(t, buf, n);
	entitiesUpdate(&w->entities, dt);
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

static const char *kindNameOf(int kind)
{
	return brainEventName(kind);
}

/* --- event names ------------------------------------------------------- */

static void test_event_names(void)
{
	TEST_ASSERT_EQUAL_STRING("SPAWN", brainEventName(BRAIN_EV_SPAWN));
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

/* --- deterministic sequence check ------------------------------------- */

/* Verify the per-customer event-kind story matches `expect` exactly (in order).
 * Returns 0 when it matches, else prints and returns 1. */
static int checkStory(const Trace *t, int id, const int *expect, int n)
{
	int i;
	int k = 0;

	for (i = 0; i < t->count; i++) {
		if (t->ev[i].id != id)
			continue;
		if (k >= n || t->ev[i].kind != expect[k]) {
			fprintf(stderr,
				"story mismatch for id %d at position %d: got %s\n",
				id, k, kindNameOf(t->ev[i].kind));
			return 1;
		}
		k++;
	}
	if (k != n) {
		fprintf(stderr,
			"story mismatch for id %d: got %d events, expected %d\n",
			id, k, n);
		return 1;
	}
	return 0;
}

/* Print a per-customer story as a C initialiser (development aid). */
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
			fprintf(stderr, " %s", kindNameOf(t->ev[i].kind));
		}
		fprintf(stderr, "\n");
	}
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
	 * left, and the machine was released. */
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_PAY));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE,
			      machineGetConst(&w.machines, 0)->state);
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_EQUAL_INT(GARMENT_DIRTY, t.ev[i].garment);
	}
	destroyVoxmap(map);
}

/* --- wait policy -> abandon -------------------------------------------- */

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

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	/* Exactly the retry cap, then an honest abandon leaving DIRTY. */
	TEST_ASSERT_EQUAL_INT(BRAIN_MAX_WAIT_RETRIES,
			      countKind(&t, BRAIN_EV_NO_CANDIDATE));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_PAY));
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
	 * registry): A (closest) picks washer 0, B (cheapest) picks washer 1. */
	{
		int i = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, 0);

		TEST_ASSERT_EQUAL_INT(0, t.ev[i].machine);
		TEST_ASSERT_EQUAL_INT(2, t.ev[i].distance);
		TEST_ASSERT_EQUAL_INT(2, t.ev[i].score);
		i = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i + 1);
		TEST_ASSERT_EQUAL_INT(1, t.ev[i].machine);	/* B's washer */
	}

	/* Each customer's story: wash (one leg) then dry (one leg) then leave.
	 * Transient contention bookkeeping (extra TASK_BEGIN / CLAIM_DENIED /
	 * NO_CANDIDATE on the dryer race) is not part of the pinned counts, so
	 * assert the durable per-customer milestones instead. */
	{
		int id;

		for (id = 0; id < 2; id++) {
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
	/* Dryer contention was real: at least one bounded wait happened, and
	 * nobody abandoned. */
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
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
	destroyVoxmap(map);
}

/* --- integration: one washer, two customers, a refused claim ----------- */

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
	/* The loser of the claim re-plans (refusal or a bounded wait), then
	 * both wash and dry. */
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== single-washer ==\n");
		dumpStory(&t);
	}
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_CLAIM_DENIED));
	TEST_ASSERT_TRUE(countKind(&t, BRAIN_EV_NO_CANDIDATE) >= 1);
	TEST_ASSERT_EQUAL_INT(0, countKind(&t, BRAIN_EV_ABANDON));

	{
		int i;
		int dry = 0;

		for (i = 0; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_DEPART &&
			    t.ev[i].garment == GARMENT_DRY_CLEAN)
				dry++;
		TEST_ASSERT_EQUAL_INT(2, dry);
	}
	destroyVoxmap(map);
}

/* --- integration: a machine breaks mid-run -> re-plan ------------------ */

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
	/* The break was seen, the second washer was chosen, and the customer
	 * still left clean dry. */
	if (getenv("BRAIN_DUMP") != NULL) {
		fprintf(stderr, "== break ==\n");
		dumpStory(&t);
	}
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_INTERRUPTED));
	/* The full exact story for the single customer, break included. */
	{
		static const int story[] = {
			BRAIN_EV_SPAWN,
			BRAIN_EV_TASK_BEGIN, BRAIN_EV_PAY, BRAIN_EV_LOAD,
			BRAIN_EV_RUN_START,
			BRAIN_EV_INTERRUPTED,
			BRAIN_EV_TASK_BEGIN, BRAIN_EV_PAY, BRAIN_EV_LOAD,
			BRAIN_EV_RUN_START, BRAIN_EV_MACHINE_DONE,
			BRAIN_EV_COLLECT, BRAIN_EV_GARMENT,
			BRAIN_EV_TASK_BEGIN, BRAIN_EV_PAY, BRAIN_EV_LOAD,
			BRAIN_EV_RUN_START, BRAIN_EV_MACHINE_DONE,
			BRAIN_EV_COLLECT, BRAIN_EV_GARMENT,
			BRAIN_EV_LEAVE_BEGIN, BRAIN_EV_DEPART,
		};

		TEST_ASSERT_EQUAL_INT(0, checkStory(&t, 0, story,
				(int)(sizeof(story) / sizeof(story[0]))));
	}
	{
		int i = findKindFrom(&t, BRAIN_EV_INTERRUPTED, 0);
		int j = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i + 1);

		TEST_ASSERT_TRUE(j > i);
		TEST_ASSERT_EQUAL_INT(1, t.ev[j].machine);	/* washer 1 */
	}
	TEST_ASSERT_EQUAL_INT(2, countKind(&t, BRAIN_EV_GARMENT));
	{
		int i = findKindFrom(&t, BRAIN_EV_DEPART, 0);

		TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t.ev[i].garment);
	}
	/* After the break the customer never re-selects the broken washer. */
	{
		int brk = findKindFrom(&t, BRAIN_EV_INTERRUPTED, 0);
		int i;

		for (i = brk + 1; i < t.count; i++)
			if (t.ev[i].kind == BRAIN_EV_TASK_BEGIN)
				TEST_ASSERT_TRUE(t.ev[i].machine != 0);
	}
	destroyVoxmap(map);
}

/* --- determinism ------------------------------------------------------- */

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

	/* A void door refuses the spawn. */
	simWorldInit(&wv, map, 99, 99, 0, 0, 0, 0);
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID,
			      simSpawnCustomer(&wv, 0, 20, buf, 8, &n));

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
	destroyVoxmap(map);
}

static void test_wait_timeout_replans(void)
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
	/* Take the machine away mid-run: no wake will ever come, so the
	 * bounded wait must fire and the task must re-plan. */
	TEST_ASSERT_TRUE(machineRelease(machineGet(&w.machines, 0)));

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_WAIT_TIMEOUT));
	{
		int i2 = findKindFrom(&t, BRAIN_EV_WAIT_TIMEOUT, 0);
		int j = findKindFrom(&t, BRAIN_EV_TASK_BEGIN, i2 + 1);

		TEST_ASSERT_EQUAL_INT(1, t.ev[j].machine);	/* washer 1 */
	}
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
	 * path out, so the customer despawns in place (still honest). */
	simWorldInit(&w, map, 0, 0, 99, 99, 0, 0);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_EQUAL_INT(BRAIN_MAX_WAIT_RETRIES,
			      countKind(&t, BRAIN_EV_NO_CANDIDATE));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&w));
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
	 * tile is void, so the A* route is refused and the task re-plans; with
	 * no other washer the goal is abandoned. */
	machineCreate(&w.machines, MACHINE_KIND_WASHER, 99, 99, 3, 1.0f);
	(void)simSpawnCustomer(&w, SEL_POLICY_CLOSEST, 20, buf, 8, &n);
	traceAppend(&t, buf, n);

	TEST_ASSERT_TRUE(runToEmpty(&w, &t, DT) > 0);
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_TASK_BEGIN));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_ABANDON));
	TEST_ASSERT_EQUAL_INT(1, countKind(&t, BRAIN_EV_DEPART));
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
	/* The entity is destroyed under the brain: the record is reclaimed. */
	entityDestroy(&w.entities, h);
	(void)simUpdate(&w, DT, buf, 8);
	TEST_ASSERT_EQUAL_INT(0, simCustomerCount(&w));
	destroyVoxmap(map);
}

void run_test_brain(void)
{
	(void)dumpStory;
	RUN_TEST(test_event_names);
	RUN_TEST(test_payment_debit_and_events);
	RUN_TEST(test_insufficient_funds_abandons);
	RUN_TEST(test_no_candidate_bounded_then_abandons);
	RUN_TEST(test_spawner_interval_and_cap);
	RUN_TEST(test_spawn_guards_and_customer_for);
	RUN_TEST(test_update_guards);
	RUN_TEST(test_wait_timeout_replans);
	RUN_TEST(test_abandon_unreachable_exit);
	RUN_TEST(test_unreachable_machine_abandons);
	RUN_TEST(test_entity_destroyed_mid_life);
	RUN_TEST(test_integration_two_customers_clean_dry);
	RUN_TEST(test_integration_single_washer_contention);
	RUN_TEST(test_integration_break_mid_run_replans);
	RUN_TEST(test_determinism);
}
