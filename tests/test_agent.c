/*
 * Agent registry + role profiles (CTOL rung 1: unit + boundary).
 *
 * Covers: agentsInit's reset (including the monotonic id stamp restarting);
 * the flat-slot registry (create to AGENT_MAX, the AGENT_INVALID overflow,
 * destroy, the AGENT_INVALID get guards, slot reuse never aliasing a live
 * agent, a reused slot getting a FRESH id); agentCreate's defaults (content
 * needs, every panic channel CLEAR, zeroed skills/flaws, unlinked entity, no
 * board / job, dwell 0, empty panicMask); the exact RoleProfile row for every
 * role (every need slot, both columns) plus the reservation that confines a
 * real panic threshold to NEED_SURVIVAL; agentRoleProfile's NULL for an
 * unknown role; agentsDecay (per-role decay multipliers, live agents only,
 * panicMask refresh, the panic RAISE below a role threshold and the BLEED back
 * to zero above it, AGENT_MAX_DT clamp, NaN-safe dt, the unknown-role skip);
 * agentsStats; and run-to-run determinism.
 *
 * dt values are dyadic (1/16 s) with dyadic role multipliers, so the expected
 * floats are exact — TEST_ASSERT_FLOAT_WITHIN(0.0f, ...) is a bit-exact pin.
 *
 * Pure: links agent.c + need.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_agent().
 */

#include "unity.h"

#include "agents/agent.h"
#include "entities/entities.h"

#include <string.h>

#define EPS 1e-6f

/* ---- helpers ------------------------------------------------------------ */

/* Pin every decay multiplier / panic threshold of a profile: the role's base
 * decay everywhere except the slots named in exceptNeeds, and a threshold
 * that is real on NEED_SURVIVAL only — every other slot is reserved at -1.0f,
 * because panic is the tier-0 alarm. Naming the exceptions (rather than "all
 * rows are the base") is what makes a STAFF row checkable at all — it has
 * three decay exceptions. */
static void assertProfileRow(const RoleProfile *p, float decay,
			     float survivalThreshold, int exceptCount,
			     const int *exceptNeeds, const float *exceptDecays)
{
	int n, i;

	TEST_ASSERT_NOT_NULL(p);
	for (n = 0; n < NEED_COUNT; n++) {
		float wantDecay = decay;
		float wantThreshold = (n == NEED_SURVIVAL) ? survivalThreshold
							   : -1.0f;

		for (i = 0; i < exceptCount; i++)
			if (exceptNeeds[i] == n)
				wantDecay = exceptDecays[i];
		TEST_ASSERT_FLOAT_WITHIN(0.0f, wantDecay, p->decayMul[n]);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, wantThreshold,
					 p->panicThreshold[n]);
	}
}

static void assertAgentDefaults(const Agent *a, int role, int id)
{
	int n;

	TEST_ASSERT_EQUAL_INT(id, a->id);
	TEST_ASSERT_EQUAL_INT(role, a->role);
	TEST_ASSERT_TRUE(a->alive);
	for (n = 0; n < NEED_COUNT; n++) {
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, a->needs.value[n]);
		/* needSetInit starts every panic channel CLEAR, so a fresh
		 * agent is born content and calm — the mask mirrors that. */
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[n]);
	}
	/* Competencies are their own (shorter) vector: pinned over SKILL_COUNT,
	 * not NEED_COUNT, so this never reads past skills[] / flaws[]. */
	for (n = 0; n < SKILL_COUNT; n++) {
		TEST_ASSERT_EQUAL_UINT(0, a->skills[n]);
		TEST_ASSERT_EQUAL_UINT(0, a->flaws[n]);
	}
	TEST_ASSERT_EQUAL_INT(0, needPanicMask(&a->needs));
	TEST_ASSERT_EQUAL_INT(0, a->panicMask);
	TEST_ASSERT_EQUAL_INT(needPanicMask(&a->needs), a->panicMask);
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, a->entity);
	TEST_ASSERT_EQUAL_INT(-1, a->personalBoard);
	TEST_ASSERT_EQUAL_INT(-1, a->engagedJob);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->dwellLeft);
}

/* ---- registry ----------------------------------------------------------- */

static void test_registry_init_resets(void)
{
	AgentRegistry reg;
	AgentHandle h;

	agentsInit(&reg);
	TEST_ASSERT_EQUAL_INT(0, reg.liveCount);

	h = agentCreate(&reg, ROLE_CUSTOMER);
	TEST_ASSERT_TRUE(h != AGENT_INVALID);
	TEST_ASSERT_EQUAL_INT(1, reg.liveCount);

	/* Re-init empties the registry and restarts the id stamp, so a fresh
	 * run produces a byte-identical registry. */
	agentsInit(&reg);
	TEST_ASSERT_EQUAL_INT(0, reg.liveCount);
	TEST_ASSERT_NULL(agentGet(&reg, h));
	h = agentCreate(&reg, ROLE_CUSTOMER);
	TEST_ASSERT_EQUAL_INT(0, agentGet(&reg, h)->id);

	agentsInit(NULL);
}

static void test_registry_create_to_capacity(void)
{
	AgentRegistry reg;
	int i;

	agentsInit(&reg);
	for (i = 0; i < AGENT_MAX; i++) {
		AgentHandle h = agentCreate(&reg, ROLE_CUSTOMER);

		TEST_ASSERT_EQUAL_INT(i, h);	/* first free slot, lowest first */
		TEST_ASSERT_EQUAL_INT(i + 1, reg.liveCount);
	}
	/* Full: the next create refuses cleanly and changes nothing. */
	TEST_ASSERT_EQUAL_INT(AGENT_INVALID, agentCreate(&reg, ROLE_CUSTOMER));
	TEST_ASSERT_EQUAL_INT(AGENT_MAX, reg.liveCount);
	TEST_ASSERT_EQUAL_INT(AGENT_INVALID, agentCreate(NULL, ROLE_CUSTOMER));
	TEST_ASSERT_EQUAL_INT(AGENT_MAX, reg.liveCount);
}

static void test_registry_get_guards(void)
{
	AgentRegistry reg;
	AgentHandle h;

	agentsInit(&reg);
	h = agentCreate(&reg, ROLE_CUSTOMER);
	TEST_ASSERT_NOT_NULL(agentGet(&reg, h));
	TEST_ASSERT_NULL(agentGet(NULL, h));
	TEST_ASSERT_NULL(agentGet(&reg, -1));
	TEST_ASSERT_NULL(agentGet(&reg, AGENT_MAX));
	TEST_ASSERT_NULL(agentGet(&reg, AGENT_MAX + 99));
	TEST_ASSERT_NULL(agentGet(&reg, h + 1));	/* never created */

	agentDestroy(&reg, h);
	TEST_ASSERT_NULL(agentGet(&reg, h));

	/* Destroying twice, or destroying nonsense, is a no-op. */
	agentDestroy(&reg, h);
	agentDestroy(&reg, -1);
	agentDestroy(&reg, AGENT_MAX);
	agentDestroy(NULL, h);
	TEST_ASSERT_EQUAL_INT(0, reg.liveCount);
}

static void test_registry_destroy_frees_slot(void)
{
	AgentRegistry reg;
	AgentHandle a, b, c;

	agentsInit(&reg);
	a = agentCreate(&reg, ROLE_CUSTOMER);
	b = agentCreate(&reg, ROLE_STAFF);
	c = agentCreate(&reg, ROLE_CRITTER);
	TEST_ASSERT_EQUAL_INT(3, reg.liveCount);

	agentDestroy(&reg, b);
	TEST_ASSERT_EQUAL_INT(2, reg.liveCount);
	TEST_ASSERT_NULL(agentGet(&reg, b));
	/* The slot is scrubbed, so a dead handle can never read a live agent. */
	TEST_ASSERT_FALSE(reg.alive[b]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, reg.slots[b].needs.value[NEED_SURVIVAL]);
	/* The neighbours are untouched. */
	TEST_ASSERT_NOT_NULL(agentGet(&reg, a));
	TEST_ASSERT_NOT_NULL(agentGet(&reg, c));
	TEST_ASSERT_EQUAL_INT(ROLE_CUSTOMER, agentGet(&reg, a)->role);
	TEST_ASSERT_EQUAL_INT(ROLE_CRITTER, agentGet(&reg, c)->role);
}

/* A reused slot is a NEW agent: fresh id, content needs, no aliasing. */
static void test_registry_slot_reuse_is_fresh(void)
{
	AgentRegistry reg;
	AgentHandle a, b, reused;
	int i, j;
	int liveIds[AGENT_MAX];
	int n = 0;

	agentsInit(&reg);
	a = agentCreate(&reg, ROLE_CUSTOMER);
	b = agentCreate(&reg, ROLE_STAFF);
	TEST_ASSERT_EQUAL_INT(0, a);
	TEST_ASSERT_EQUAL_INT(1, b);

	/* Pollute the agent we are about to kill, so reuse would be visible. */
	agentGet(&reg, a)->skills[SKILL_REPAIR] = 900;
	agentGet(&reg, a)->needs.value[NEED_SURVIVAL] = 0.1f;

	agentDestroy(&reg, a);
	reused = agentCreate(&reg, ROLE_CUSTOMER);
	TEST_ASSERT_EQUAL_INT(a, reused);	/* same slot */
	TEST_ASSERT_EQUAL_INT(1, b);		/* the live agent kept its slot */

	/* The new occupant is not the old one: new id, default needs. */
	TEST_ASSERT_TRUE(agentGet(&reg, b)->id != agentGet(&reg, reused)->id);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f,
				 agentGet(&reg, reused)->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_EQUAL_UINT(0, agentGet(&reg, reused)->skills[SKILL_REPAIR]);

	/* No two live agents share an id or a slot. */
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *ag;

		if (!reg.alive[i])
			continue;
		ag = &reg.slots[i];
		TEST_ASSERT_EQUAL_PTR(&reg.slots[i], agentGet(&reg, i));
		TEST_ASSERT_TRUE(ag->alive);
		liveIds[n] = ag->id;
		for (j = 0; j < n; j++)
			TEST_ASSERT_TRUE(liveIds[j] != ag->id);
		n++;
	}
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_INT(2, reg.liveCount);
}

static void test_registry_ids_are_monotonic(void)
{
	AgentRegistry reg;
	int first, second, third;

	agentsInit(&reg);
	first = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER))->id;
	second = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER))->id;
	third = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER))->id;
	TEST_ASSERT_EQUAL_INT(0, first);
	TEST_ASSERT_EQUAL_INT(1, second);
	TEST_ASSERT_EQUAL_INT(2, third);

	/* A destroyed slot's id is never reissued — the stable tie-break key. */
	agentDestroy(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	TEST_ASSERT_EQUAL_INT(4, agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER))->id);
}

static void test_create_defaults(void)
{
	AgentRegistry reg;
	Agent *cust, *staff, *critter;

	agentsInit(&reg);
	cust = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	staff = agentGet(&reg, agentCreate(&reg, ROLE_STAFF));
	critter = agentGet(&reg, agentCreate(&reg, ROLE_CRITTER));
	TEST_ASSERT_NOT_NULL(cust);
	TEST_ASSERT_NOT_NULL(staff);
	TEST_ASSERT_NOT_NULL(critter);

	assertAgentDefaults(cust, ROLE_CUSTOMER, 0);
	assertAgentDefaults(staff, ROLE_STAFF, 1);
	assertAgentDefaults(critter, ROLE_CRITTER, 2);

	/* Agency fields the framework fills in later are writable and hold.
	 * entityCreate on a throwaway registry hands back slot 0 — a real
	 * handle, distinct from the ENTITY_INVALID birth value pinned above. */
	cust->entity = entityCreate(&(EntityRegistry){ 0 });
	cust->personalBoard = 3;
	cust->engagedJob = 7;
	cust->dwellLeft = 1.5f;
	TEST_ASSERT_EQUAL_INT(0, cust->entity);
	TEST_ASSERT_TRUE(cust->entity != ENTITY_INVALID);
	TEST_ASSERT_EQUAL_INT(3, cust->personalBoard);
	TEST_ASSERT_EQUAL_INT(7, cust->engagedJob);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.5f, cust->dwellLeft);
}

/* ---- role profiles ------------------------------------------------------ */

static void test_role_profile_customer(void)
{
	/* The baseline row: nothing tuned, survival trips at 0.15. */
	assertProfileRow(agentRoleProfile(ROLE_CUSTOMER), 1.0f, 0.15f, 0, NULL,
			 NULL);
}

static void test_role_profile_staff(void)
{
	/* A staffer runs on social contact and burns out faster on the floor. */
	static const int exceptNeeds[] = { NEED_SOCIAL, NEED_FULFILMENT,
					   NEED_ENERGY };
	static const float exceptDecays[] = { 0.5f, 0.5f, 1.5f };

	assertProfileRow(agentRoleProfile(ROLE_STAFF), 1.0f, 0.15f, 3, exceptNeeds,
			 exceptDecays);
}

static void test_role_profile_critter(void)
{
	/* Needs little company, is not bothered by esteem (0.0 = never decays),
	 * and trips EARLIER on survival: 0.10. */
	static const int exceptNeeds[] = { NEED_SOCIAL, NEED_SATISFACTION,
					   NEED_ESTEEM };
	static const float exceptDecays[] = { 0.25f, 0.5f, 0.0f };

	assertProfileRow(agentRoleProfile(ROLE_CRITTER), 1.0f, 0.10f, 3,
			 exceptNeeds, exceptDecays);
}

/* Panic is reserved for tier-0 survival in every shipped row, so the spec's
 * "a dryer is on fire" can reprioritise everyone without an emptied
 * SATISFACTION silencing the whole ladder. */
static void test_role_panic_is_reserved_to_survival(void)
{
	int role;
	int n;

	for (role = 0; role < ROLE_COUNT; role++) {
		const RoleProfile *p = agentRoleProfile(role);

		TEST_ASSERT_TRUE(p->panicThreshold[NEED_SURVIVAL] > 0.0f);
		for (n = 0; n < NEED_COUNT; n++)
			if (n != NEED_SURVIVAL)
				TEST_ASSERT_FLOAT_WITHIN(0.0f, -1.0f,
							 p->panicThreshold[n]);
	}
}

static void test_role_profile_unknown_is_null(void)
{
	TEST_ASSERT_NULL(agentRoleProfile(ROLE_COUNT));
	TEST_ASSERT_NULL(agentRoleProfile(-1));
	TEST_ASSERT_NULL(agentRoleProfile(99));
	/* The table is static: the same row every call, no allocation. */
	TEST_ASSERT_EQUAL_PTR(agentRoleProfile(ROLE_CUSTOMER),
			      agentRoleProfile(ROLE_CUSTOMER));
	TEST_ASSERT_TRUE(agentRoleProfile(ROLE_CUSTOMER) !=
			 agentRoleProfile(ROLE_STAFF));
	TEST_ASSERT_EQUAL_INT(3, ROLE_COUNT);
	TEST_ASSERT_EQUAL_INT(3, SKILL_COUNT);
}

/* ---- agentsDecay -------------------------------------------------------- */

static void test_decay_applies_role_multipliers(void)
{
	AgentRegistry reg;
	Agent *cust, *staff, *critter;

	agentsInit(&reg);
	cust = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	staff = agentGet(&reg, agentCreate(&reg, ROLE_STAFF));
	critter = agentGet(&reg, agentCreate(&reg, ROLE_CRITTER));

	agentsDecay(&reg, 0.0625f);

	/* Customer: base rate, 1.0 - 1 * (1/16). */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, cust->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, staff->needs.value[NEED_SURVIVAL]);
	/* Staff: the shift tires you (1.5) and company matters less (0.5). */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.90625f, staff->needs.value[NEED_ENERGY]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.96875f, staff->needs.value[NEED_SOCIAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.96875f, staff->needs.value[NEED_FULFILMENT]);
	/* Critter: needs little company, cares not for esteem. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.984375f, critter->needs.value[NEED_SOCIAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.96875f,
				 critter->needs.value[NEED_SATISFACTION]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, critter->needs.value[NEED_ESTEEM]);
	/* A need already below 1.0 ramps, so it loses more than the same need
	 * on a content agent — the per-need curve is the role's, the ramp is
	 * shared. */
	TEST_ASSERT_TRUE(critter->needs.value[NEED_ESTEEM] >
			 staff->needs.value[NEED_ESTEEM]);
}

static void test_decay_advances_only_live_agents(void)
{
	AgentRegistry reg;
	AgentHandle h;
	Agent *live;

	agentsInit(&reg);
	h = agentCreate(&reg, ROLE_CUSTOMER);
	live = agentGet(&reg, h);
	agentDestroy(&reg, agentCreate(&reg, ROLE_STAFF));

	agentsDecay(&reg, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, live->needs.value[NEED_SURVIVAL]);
	/* The dead slot stays scrubbed at zero — a dead agent never decays. */
	TEST_ASSERT_EQUAL_INT(1, reg.liveCount);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, reg.slots[1].needs.value[NEED_SURVIVAL]);
}

static void test_decay_refreshes_panic_mask(void)
{
	AgentRegistry reg;
	Agent *a;

	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	/* needSetInit starts every channel clear: the born state is content,
	 * not panic, so the mask starts empty. */
	TEST_ASSERT_EQUAL_INT(0, a->panicMask);
	TEST_ASSERT_EQUAL_INT(needPanicMask(&a->needs), a->panicMask);

	/* Emptying SURVIVAL trips its threshold: the alarm rises and the mask
	 * follows on the next decay. */
	a->needs.value[NEED_SURVIVAL] = 0.0f;
	agentsDecay(&reg, NEED_MAX_DT);
	TEST_ASSERT_TRUE(a->needs.panic[NEED_SURVIVAL] > 0.0f);
	TEST_ASSERT_EQUAL_INT(1 << NEED_SURVIVAL, a->panicMask);
	TEST_ASSERT_EQUAL_INT(needPanicMask(&a->needs), a->panicMask);

	/* Restoring it above the threshold bleeds the panic back to zero and
	 * the bit clears with it. */
	a->needs.value[NEED_SURVIVAL] = 1.0f;
	agentsDecay(&reg, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[NEED_SURVIVAL]);
	TEST_ASSERT_EQUAL_INT(0, a->panicMask);
}

static void test_decay_panic_rises_below_role_threshold(void)
{
	AgentRegistry reg;
	Agent *a;

	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CRITTER));
	a->needs.value[NEED_SURVIVAL] = 0.05f;	/* under the 0.10 threshold */
	agentsDecay(&reg, NEED_MAX_DT);
	/* RISING, not held: the cause is live, so the alarm climbs a step
	 * per tick instead of merely staying put. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, NEED_PANIC_RISE * NEED_MAX_DT,
				 a->needs.panic[NEED_SURVIVAL]);
	agentsDecay(&reg, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 2.0f * NEED_PANIC_RISE * NEED_MAX_DT,
				 a->needs.panic[NEED_SURVIVAL]);
	TEST_ASSERT_EQUAL_INT(1 << NEED_SURVIVAL, a->panicMask);
}

/* A fresh agent is calm, so its wants are NOT suppressed; and only survival
 * is allowed to raise an alarm. */
static void test_decay_only_survival_panics(void)
{
	AgentRegistry reg, ref;
	Agent *a, *b;
	int n;

	agentsInit(&reg);
	agentsInit(&ref);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	b = agentGet(&ref, agentCreate(&ref, ROLE_CUSTOMER));

	/* Born calm, and one decay tick does not manufacture a panic out of a
	 * content agent: its needs are still well above their thresholds. */
	for (n = 0; n < NEED_COUNT; n++)
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[n]);
	agentsDecay(&reg, NEED_MAX_DT);
	TEST_ASSERT_EQUAL_INT(0, a->panicMask);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9f, a->needs.value[NEED_SURVIVAL]);

	/* Emptied SURVIVAL (armed) vs emptied SATISFACTION (reserved). */
	a->needs.value[NEED_SURVIVAL] = 0.0f;
	a->needs.value[NEED_SATISFACTION] = 0.0f;
	b->needs.value[NEED_SATISFACTION] = 0.0f;
	agentsDecay(&reg, NEED_MAX_DT);

	TEST_ASSERT_FLOAT_WITHIN(0.0f, NEED_PANIC_RISE * NEED_MAX_DT,
				 a->needs.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[NEED_SATISFACTION]);
	TEST_ASSERT_EQUAL_INT(1 << NEED_SURVIVAL, a->panicMask);
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&a->needs, NEED_SURVIVAL));

	/* Reserved, yet still fully urgent through its tier weight: band 9 on
	 * its own, stepped down to 7 only because survival is panicking —
	 * exactly the NEED_PANIC_SUPPRESS rounding measured against the same
	 * need with no panic active. */
	TEST_ASSERT_EQUAL_INT(9, needBand(&a->needs, NEED_SATISFACTION));
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&b->needs, NEED_SATISFACTION));
	TEST_ASSERT_EQUAL_INT(7, wantUrgency(&a->needs, NEED_SATISFACTION));
	TEST_ASSERT_TRUE(wantUrgency(&a->needs, NEED_SATISFACTION) <
			 wantUrgency(&b->needs, NEED_SATISFACTION));

	/* ...and once survival recovers, the suppression lifts. */
	a->needs.value[NEED_SURVIVAL] = 1.0f;
	agentsDecay(&reg, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[NEED_SURVIVAL]);
	TEST_ASSERT_EQUAL_INT(0, a->panicMask);
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&a->needs, NEED_SATISFACTION));
}

static void test_decay_clamps_dt(void)
{
	AgentRegistry atMax, huge;
	Agent *a, *b;

	agentsInit(&atMax);
	agentsInit(&huge);
	a = agentGet(&atMax, agentCreate(&atMax, ROLE_CUSTOMER));
	b = agentGet(&huge, agentCreate(&huge, ROLE_CUSTOMER));

	agentsDecay(&atMax, AGENT_MAX_DT);
	agentsDecay(&huge, 100.0f);	/* clamped to AGENT_MAX_DT */
	TEST_ASSERT_FLOAT_WITHIN(EPS, a->needs.value[NEED_SURVIVAL],
				 b->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_TRUE(a->needs.value[NEED_SURVIVAL] < 1.0f);

	/* An empty registry is a no-op, and so is a NULL one. */
	agentsDecay(NULL, 0.1f);
	agentsInit(&atMax);
	agentsDecay(&atMax, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, atMax.liveCount);
}

static void test_decay_ignores_nonpositive_and_nan_dt(void)
{
	AgentRegistry reg;
	Agent *a;
	float zero = 0.0f;

	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	agentsDecay(&reg, 0.0f);
	agentsDecay(&reg, -1.0f);
	agentsDecay(&reg, zero / zero);	/* NaN */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, a->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->needs.panic[NEED_SURVIVAL]);
}

/* An agent whose role has no profile is skipped rather than dereferenced:
 * roles are data, and a bad row must not take the whole loop down. */
static void test_decay_unknown_role_is_skipped(void)
{
	AgentRegistry reg;
	Agent *a, *b;

	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	b = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	b->role = ROLE_COUNT;

	agentsDecay(&reg, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, a->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, b->needs.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, b->needs.panic[NEED_SURVIVAL]);

	b->role = -1;
	agentsDecay(&reg, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, b->needs.value[NEED_SURVIVAL]);
}

static void test_decay_is_deterministic(void)
{
	AgentRegistry a, b;
	AgentHandle ha[8], hb[8];
	int i;

	agentsInit(&a);
	agentsInit(&b);
	for (i = 0; i < 8; i++) {
		ha[i] = agentCreate(&a, i % ROLE_COUNT);
		hb[i] = agentCreate(&b, i % ROLE_COUNT);
	}
	agentDestroy(&a, 3);
	agentDestroy(&b, 3);
	agentGet(&a, ha[0])->needs.value[NEED_SURVIVAL] = 0.3f;
	agentGet(&b, hb[0])->needs.value[NEED_SURVIVAL] = 0.3f;
	agentGet(&a, ha[1])->engagedJob = 4;
	agentGet(&b, hb[1])->engagedJob = 4;

	for (i = 0; i < 100; i++)
		agentsDecay(&a, 1.0f / 60.0f);
	for (i = 0; i < 100; i++)
		agentsDecay(&b, 1.0f / 60.0f);

	TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

/* ---- stats -------------------------------------------------------------- */

static void test_agents_stats(void)
{
	AgentStats s;

	s.agentDecisions = 1234u;
	s.pathfindCalls = 56u;
	agentsStats(&s);
	/* Phase 1 does not act yet, so both counters read zero — and a read
	 * zeroes them for the phase that follows. */
	TEST_ASSERT_EQUAL_UINT(0u, s.agentDecisions);
	TEST_ASSERT_EQUAL_UINT(0u, s.pathfindCalls);

	agentsStats(&s);
	TEST_ASSERT_EQUAL_UINT(0u, s.agentDecisions);
	TEST_ASSERT_EQUAL_UINT(0u, s.pathfindCalls);

	agentsStats(NULL);
}

void run_test_agent(void);

void run_test_agent(void)
{
	RUN_TEST(test_registry_init_resets);
	RUN_TEST(test_registry_create_to_capacity);
	RUN_TEST(test_registry_get_guards);
	RUN_TEST(test_registry_destroy_frees_slot);
	RUN_TEST(test_registry_slot_reuse_is_fresh);
	RUN_TEST(test_registry_ids_are_monotonic);
	RUN_TEST(test_create_defaults);
	RUN_TEST(test_role_profile_customer);
	RUN_TEST(test_role_profile_staff);
	RUN_TEST(test_role_profile_critter);
	RUN_TEST(test_role_panic_is_reserved_to_survival);
	RUN_TEST(test_role_profile_unknown_is_null);
	RUN_TEST(test_decay_applies_role_multipliers);
	RUN_TEST(test_decay_advances_only_live_agents);
	RUN_TEST(test_decay_refreshes_panic_mask);
	RUN_TEST(test_decay_panic_rises_below_role_threshold);
	RUN_TEST(test_decay_only_survival_panics);
	RUN_TEST(test_decay_clamps_dt);
	RUN_TEST(test_decay_ignores_nonpositive_and_nan_dt);
	RUN_TEST(test_decay_unknown_role_is_skipped);
	RUN_TEST(test_decay_is_deterministic);
	RUN_TEST(test_agents_stats);
}
