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
#include "agents/activity.h"
#include "agents/board.h"
#include "ai/nav.h"
#include "entities/entities.h"
#include "render/voxmap.h"

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
	/* The personal board is embedded BY VALUE (task 3): a born agent owns
	 * an empty, private board — not a handle into a pool of boards. */
	TEST_ASSERT_EQUAL_INT(0, a->personal.liveCount);
	TEST_ASSERT_FALSE(a->personal.shared);
	TEST_ASSERT_FALSE(a->personal.alive[0]);
	TEST_ASSERT_FALSE(a->personal.alive[BOARD_MAX - 1]);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, a->engagedJob);
	TEST_ASSERT_FALSE(a->engagedShared);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, a->dwellLeft);
	/* Nothing has been routed yet: the last path is "none attempted". */
	TEST_ASSERT_EQUAL_INT(-1, a->pathLen);
}

/* ---- registry ----------------------------------------------------------- */

static void test_registry_init_resets(void)
{
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	 * handle, distinct from the ENTITY_INVALID birth value pinned
	 * above — and the embedded personal board takes work like any
	 * other board. */
	cust->entity = entityCreate(&(EntityRegistry){ 0 });
	{
		Job j;

		jobInit(&j, JOB_PAY);
		j.band = 4;
		TEST_ASSERT_EQUAL_INT(0, boardEnqueue(&cust->personal, &j,
						      JOB_PRIO_ERRAND));
	}
	cust->engagedJob = 0;
	cust->engagedShared = false;
	cust->dwellLeft = 1.5f;
	TEST_ASSERT_EQUAL_INT(0, cust->entity);
	TEST_ASSERT_TRUE(cust->entity != ENTITY_INVALID);
	TEST_ASSERT_EQUAL_INT(1, cust->personal.liveCount);
	TEST_ASSERT_EQUAL_INT(JOB_PAY, cust->personal.slots[0].kind);
	TEST_ASSERT_EQUAL_INT(4, cust->personal.slots[0].band);
	TEST_ASSERT_EQUAL_INT(0, cust->engagedJob);
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg, ref;
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
	static AgentRegistry atMax, huge;
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
	static AgentRegistry reg;
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
	static AgentRegistry reg;
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
	static AgentRegistry a, b;
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

/* ---- Task 4: bands are COMPUTED, so a test sets NEEDS, not integers --- *
 *
 * Before the activity catalogue a job's band was a number a producer wrote
 * onto the record. It is now computed by jobUrgency from the needs the
 * job's ACTIVITY serves (agents/activity.h), so these tests reach a band the
 * way the framework does: pick the activity, then set the need so that
 * wantUrgency x serves lands on the wanted band. Nothing is weakened — the
 * band an assertion talks about is still a real computed band, and
 * pushScored FAILS if the requested one is unreachable, rather than letting a
 * test quietly assert against a band it did not get.
 *
 * Two consequences worth stating, because they are properties of the design
 * rather than of the tests:
 *
 *   - TWO JOBS OF THE SAME KIND ALWAYS SCORE THE SAME for one agent. One
 *     activity, one fold. So a margin is only ever measured between
 *     DIFFERENT activities, which is what a real margin is.
 *   - THE BAND CEILING IS THE CATALOGUE'S. No row serves tier-0 SURVIVAL,
 *     so wantUrgency tops out at 9 on the strongest served need and the
 *     largest band any activity can reach is round(9 x 0.9) = 8. The bands
 *     below are real ones. */

/* Set one need so that wantUrgency reads exactly `want` for it (0..NEED_BANDS-1).
 * wantUrgency is round(tierWeight * deficit * (NEED_BANDS - 1)), so the
 * deficit that yields `want` is want / (bands-1) / weight, clamped to the
 * deficit's own 0..1 range. */
static void tuneNeed(Agent *a, int need, int want)
{
	float w = needTierWeight(need);
	float deficit = (float)want / ((float)(NEED_BANDS - 1) * w);

	if (!(deficit > 0.0f))
		deficit = 0.0f;
	else if (deficit > 1.0f)
		deficit = 1.0f;
	a->needs.value[need] = 1.0f - deficit;
	a->panicMask = needPanicMask(&a->needs);
}

/* Pin `a`'s needs so that a job of `kind` would score exactly `band`. NEEDS
 * DECAY, so a band pinned before a long run of ticks has drifted by the time
 * that run ends (a hand-set band never would have). This re-pins one without
 * pushing a job. */
static void tuneToBand(Agent *a, int kind, int band)
{
	const Activity *act = activitiesGet(kind);
	int drive = -1;
	int n;

	TEST_ASSERT_NOT_NULL(act);
	for (n = NEED_COUNT - 1; n >= 0; n--) {
		if (act->serves[n] > 0.0f) {
			drive = n;
			break;
		}
	}
	TEST_ASSERT_TRUE(drive >= 0);
	tuneNeed(a, drive, (int)((float)band / act->serves[drive] + 0.5f));
}

/* Push a job of `kind` whose COMPUTED band is exactly `band`, bound to a
 * tile so it walks. The activity's own place flag does not constrain a job's
 * target — a nap may still name a tile and be walked to — which is what lets
 * these tests measure the switch rule without also measuring walking.
 *
 * The need driven is the activity's LAST served slot, so two activities that
 * overlap on one need still stay independent (the nap drives ENERGY, the
 * coffee break drives SATISFACTION). */
static int pushScored(Agent *a, JobBoard *b, int kind, int x, int z, int band,
		      float duration)
{
	const Activity *act = activitiesGet(kind);
	Job j;
	int drive = -1;
	int n;

	TEST_ASSERT_NOT_NULL(act);
	for (n = NEED_COUNT - 1; n >= 0; n--) {
		if (act->serves[n] > 0.0f) {
			drive = n;
			break;
		}
	}
	TEST_ASSERT_TRUE(drive >= 0);
	tuneNeed(a, drive, (int)((float)band / act->serves[drive] + 0.5f));
	jobInit(&j, kind);
	j.duration = duration;
	jobSetTarget(&j, JOB_TARGET_TILE, JOB_TILE_PACK(x, z));
	/* The band is COMPUTED here, exactly as the tick computes it. */
	j.band = jobUrgency(a, &j);
	/* An unreachable request is a broken test, and says so here rather
	 * than letting the assertions below quietly measure a different
	 * band than the one they name. */
	TEST_ASSERT_EQUAL_INT(band, j.band);
	return boardEnqueue(b, &j, JOB_PRIO_FILL);
}

/* A wander job for a tile target: kind, a COMPUTED band (task 4 — see
 * pushScored), and the demo's timed wait as the job's duration. */
static int pushWander(JobBoard *b, int x, int z, int band, float duration)
{
	Job j;

	jobInit(&j, JOB_WANDER);
	j.band = band;
	j.duration = duration;
	jobSetTarget(&j, JOB_TARGET_TILE, JOB_TILE_PACK(x, z));
	return boardEnqueue(b, &j, JOB_PRIO_FILL);
}

/* ---- the switch decision ------------------------------------------------ *
 *
 * agentShouldSwitch is the single place an agent changes its mind, so these
 * tests pin the rule cell by cell AND the property the whole framework rests
 * on: an agent whose challenger is one band better does not move for 200
 * ticks, and one whose challenger clears AGENT_PREEMPT_MARGIN moves exactly
 * once and then holds. The property is checked twice — once against the rule
 * alone (the decision function in isolation, over fake dt) and once through
 * agentsUpdate, where the engaged job must actually stay engaged. */

static void test_should_switch_table(void)
{
	static AgentRegistry reg;
	Agent *a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));

	TEST_ASSERT_NOT_NULL(a);

	/* Idle (either side JOB_INVALID) takes the best available, whatever
	 * the dwell says: a dwell protects the job you HOLD, and an agent
	 * holding nothing has nothing to protect. */
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, JOB_INVALID, 0, 0.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, JOB_INVALID, 0, 9.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 3, JOB_INVALID, 0.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 3, JOB_INVALID, 9.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, JOB_INVALID, JOB_INVALID,
						   9.0f));

	/* THE anti-oscillation case: one band better is not a reason. */
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 9, 10, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 0, 1, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 14, 14, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 14, 15, 0.0f));
	/* A WORSE challenger never preempts, margin or no margin. */
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 9, 8, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 9, 0, 0.0f));

	/* A decisive gap does, once the dwell has expired. */
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 9, 11, 0.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 0,
						   AGENT_PREEMPT_MARGIN, 0.0f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 0, 15, 0.0f));

	/* Nothing preempts DURING the dwell, however urgent — and the
	 * boundary is exact: a dwell that has run out is not a dwell. */
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 0, 15, AGENT_MIN_DWELL));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(a, 0, 15, 0.0001f));
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 0, 15, 0.0f));
	/* A negative dwell is a spent dwell (the counter is floored). */
	TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(a, 0, 15, -1.0f));

	/* There is no agent to move: stay. */
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(NULL, 0, 15, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(NULL, JOB_INVALID, 0, 0.0f));
}

/* The property, on the rule alone: 200 fake-dt ticks against a REAL board
 * (the challenger band is read the way agentsUpdate reads it) must never
 * move an agent whose challenger is one band better, and must move it
 * exactly once when the gap is AGENT_PREEMPT_MARGIN. */
static void test_should_switch_does_not_oscillate(void)
{
	static AgentRegistry reg;
	const float dt = 0.1f;
	Agent *a;
	int engaged;
	int switches;
	int i;

	/* --- one band apart: the engaged band never changes --- */
	engaged = 9;
	switches = 0;
	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	TEST_ASSERT_NOT_NULL(a);
	pushWander(&a->personal, 3, 1, engaged, 0.4f);
	pushWander(&a->personal, 3, 3, engaged + 1, 0.4f);
	a->dwellLeft = 0.0f;
	a->engagedJob = 0;

	for (i = 0; i < 200; i++) {
		int skip[2];
		int challenger;
		int challengerBand;

		skip[0] = a->engagedJob;
		skip[1] = JOB_INVALID;
		challenger = boardBestFor(&a->personal, skip, a->id);
		challengerBand = (challenger == JOB_INVALID)
					 ? JOB_INVALID
					 : a->personal.slots[challenger].band;
		if (agentShouldSwitch(a, engaged, challengerBand,
				      a->dwellLeft)) {
			engaged = challengerBand;
			a->engagedJob = challenger;
			switches++;
		}
		if (a->dwellLeft > 0.0f)
			a->dwellLeft -= dt;
		if (a->dwellLeft < 0.0f)
			a->dwellLeft = 0.0f;
	}
	/* One band better is noise: 200 ticks, zero switches, and the agent
	 * is still on the job it started with. */
	TEST_ASSERT_EQUAL_INT(0, switches);
	TEST_ASSERT_EQUAL_INT(9, engaged);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);

	/* --- exactly AGENT_PREEMPT_MARGIN apart: one move, then hold --- */
	switches = 0;
	agentsInit(&reg);
	a = agentGet(&reg, agentCreate(&reg, ROLE_CUSTOMER));
	TEST_ASSERT_NOT_NULL(a);
	pushWander(&a->personal, 3, 1, engaged, 0.4f);
	pushWander(&a->personal, 3, 3, engaged + AGENT_PREEMPT_MARGIN, 0.4f);
	a->dwellLeft = 0.0f;
	a->engagedJob = 0;

	for (i = 0; i < 200; i++) {
		int skip[2];
		int challenger;
		int challengerBand;

		skip[0] = a->engagedJob;
		skip[1] = JOB_INVALID;
		challenger = boardBestFor(&a->personal, skip, a->id);
		challengerBand = (challenger == JOB_INVALID)
					 ? JOB_INVALID
					 : a->personal.slots[challenger].band;
		if (agentShouldSwitch(a, engaged, challengerBand,
				      a->dwellLeft)) {
			engaged = challengerBand;
			a->engagedJob = challenger;
			switches++;
		}
		if (a->dwellLeft > 0.0f)
			a->dwellLeft -= dt;
		if (a->dwellLeft < 0.0f)
			a->dwellLeft = 0.0f;
	}
	/* One move onto the urgent job, and then nothing on the board beats
	 * it — so the remaining 199 ticks are silent. */
	TEST_ASSERT_EQUAL_INT(1, switches);
	TEST_ASSERT_EQUAL_INT(9 + AGENT_PREEMPT_MARGIN, engaged);
	TEST_ASSERT_EQUAL_INT(1, a->engagedJob);
}

/* --- the tick ----------------------------------------------------------- *
 *
 * agentsUpdate is the decision half plus the execute half, so its tests need
 * a world: a flat map, an entity to move and an agent to decide. The bodies
 * are stepped with the same entitiesUpdate the demo runs, so the tests
 * observe the real walk rather than a stubbed one. */

#define TICK_MAP_DIM 16

/* The same plateau with a hole punched in it, so a target that resolves
 * inside the map can still be nowhere to stand. */
static Voxmap *holedMap(void)
{
	char text[TICK_MAP_DIM * (TICK_MAP_DIM + 1) + 1];
	int i, n = 0;

	for (i = 0; i < TICK_MAP_DIM; i++) {
		int j;

		for (j = 0; j < TICK_MAP_DIM; j++) {
			/* Row 5, columns 3-8: no ground at all. */
			text[n++] = (i == 5 && j >= 3 && j <= 8) ? '.'
								 : '1';
		}
		text[n++] = '\n';
	}
	text[n] = '\0';
	return parseVoxmapText(text, (size_t)n, NULL);
}

static Voxmap *flatMap(void)
{
	char text[TICK_MAP_DIM * (TICK_MAP_DIM + 1) + 1];
	int i, n = 0;

	for (i = 0; i < TICK_MAP_DIM; i++) {
		int j;

		for (j = 0; j < TICK_MAP_DIM; j++)
			text[n++] = '1';
		text[n++] = '\n';
	}
	text[n] = '\0';
	return parseVoxmapText(text, (size_t)n, NULL);
}

static EntityHandle makeBody(EntityRegistry *ents, const Voxmap *map, int x,
			     int z)
{
	EntityHandle h = entityCreate(ents);
	Entity *e = entityGet(ents, h);

	if (e == NULL || !entityPlace(e, x, z))
		return ENTITY_INVALID;
	e->speed = 3.0f;
	e->animated = true;
	e->easing = EASE_IN_OUT;
	(void)map;
	return h;
}

/* One agent with a body and an empty personal board, plus the boards set the
 * tick needs. Static storage: a registry embeds AGENT_MAX boards by value and
 * does not belong on a test's stack. */
typedef struct TickWorld {
	Voxmap *map;
	EntityRegistry ents;
	AgentRegistry reg;
	AgentBoards boards;
	AgentHandle agent;
	Agent *a;
	EntityHandle body;
} TickWorld;

static TickWorld g_world;

static void worldInit(TickWorld *w, int bodyX, int bodyZ)
{
	w->map = flatMap();
	entitiesInit(&w->ents, w->map);
	agentsInit(&w->reg);
	boardInit(&w->boards.shared, true);
	w->agent = agentCreate(&w->reg, ROLE_CRITTER);
	w->a = agentGet(&w->reg, w->agent);
	w->body = makeBody(&w->ents, w->map, bodyX, bodyZ);
	w->a->entity = w->body;
}

static void worldFree(TickWorld *w)
{
	destroyVoxmap(w->map);
	w->map = NULL;
}

/* Run `ticks` ticks of `dt`, stepping the bodies exactly as the demo does. */
static int runTicks(TickWorld *w, int ticks, float dt)
{
	int total = 0;
	int i;

	for (i = 0; i < ticks; i++) {
		int decisions = 0;

		total += agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, dt,
				      &decisions);
		entitiesUpdate(&w->ents, dt);
	}
	return total;
}

/* An idle agent takes the BEST job on its personal board, claims it, starts
 * the lifecycle, and paths its body there — all in the one tick. */
static void test_update_engages_the_best_job(void)
{
	TickWorld *w = &g_world;
	Entity *e;
	int decisions = 0;

	worldInit(w, 1, 1);
	/* A weak wander and a decisive nap, on two different activities so
	 * the two computed bands really do differ (see pushScored). */
	pushScored(w->a, &w->a->personal, ACT_PLAY_ARCADE, 3, 1, 2, 0.4f);
	pushScored(w->a, &w->a->personal, ACT_NAP, 9, 1, 8, 0.4f);

	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, 0.1f,
			 &decisions);

	TEST_ASSERT_EQUAL_INT(1, decisions);
	TEST_ASSERT_EQUAL_INT(1, w->a->engagedJob);
	TEST_ASSERT_FALSE(w->a->engagedShared);
	/* The claim is the agent's, and the lifecycle has moved on to the
	 * walk: CLAIM then the ARRIVED that issues MOVE_TO. */
	TEST_ASSERT_EQUAL_INT(w->a->id, w->a->personal.slots[1].claimer);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[1].state);
	/* Staying power starts the moment a job is taken. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, AGENT_MIN_DWELL, w->a->dwellLeft);
	/* ...and the body was routed to the job's tile. */
	e = entityGet(&w->ents, w->body);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(8, w->a->pathLen);
	/* The route is eight tiles long and its FIRST step is already in
	 * flight (toX is the segment target, not the job's tile). */
	TEST_ASSERT_EQUAL_INT(2, e->toX);
	TEST_ASSERT_TRUE(e->moving || e->pathCount > 0);
	worldFree(w);
}

/* The engaged band is what the body is doing; the challenge is the property
 * test through the real tick. */
static void test_update_does_not_oscillate_on_a_near_equal_band(void)
{
	TickWorld *w = &g_world;
	int engagedFirst;
	int switches = 0;
	int i;

	worldInit(w, 1, 1);
	/* The job it takes first, then — one tick later — a challenger one
	 * band better turns up on the same board. That is the shape the
	 * property is about: not two jobs at rest, but new work arriving
	 * beside work already running. */
	pushWander(&w->a->personal, 3, 1, 9, 0.4f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	pushWander(&w->a->personal, 3, 3, 9 + 1, 0.4f);
	engagedFirst = w->a->engagedJob;

	for (i = 0; i < 200; i++) {
		int before = w->a->engagedJob;
		int state = (before != JOB_INVALID)
				    ? w->a->personal.slots[before].state
				    : JOB_UNCLAIMED;

		runTicks(w, 1, 0.1f);
		/* A PREEMPTION is leaving a job that was still running for a
		 * different one. Finishing a job and picking up the next is
		 * not a switch — it is the agent working. */
		if (w->a->engagedJob != JOB_INVALID &&
		    w->a->engagedJob != before &&
		    state != JOB_DONE && state != JOB_FAILED)
			switches++;
	}
	/* 200 ticks with a one-band challenger sitting on the board the
	 * whole time: the running job is never displaced. */
	TEST_ASSERT_EQUAL_INT(0, switches);
	TEST_ASSERT_EQUAL_INT(engagedFirst, 0);
	/* It is not a stalled agent either: both legs ran to completion (in
	 * about three seconds), so the final state is an idle agent with two
	 * finished jobs still on its board for its owner to retire. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, w->a->personal.slots[1].state);
	worldFree(w);
}

/* A challenger that clears the margin preempts EXACTLY ONCE, and the job it
 * displaced is failed and released — never left claimed. */
static void test_update_preempts_once_on_a_decisive_challenger(void)
{
	TickWorld *w = &g_world;
	int switchedAt = -1;
	int i;

	worldInit(w, 1, 1);
	/* The engaged one: band 5. */
	pushScored(w->a, &w->a->personal, ACT_COFFEE, 2, 1, 5, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	/* The urgent work turns up while the dwell is still running: band 8,
	 * which clears AGENT_PREEMPT_MARGIN (2) off the incumbent's 5. */
	pushScored(w->a, &w->a->personal, ACT_NAP, 2, 3, 5 + AGENT_PREEMPT_MARGIN,
		   4.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, AGENT_MIN_DWELL, w->a->dwellLeft);

	/* The dwell runs out after AGENT_MIN_DWELL seconds; before that the
	 * urgent job must not move it. */
	for (i = 0; i < 100; i++) {
		runTicks(w, 1, 0.1f);
		if (w->a->engagedJob == 1)
			break;
		TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	}
	switchedAt = w->a->engagedJob;
	TEST_ASSERT_EQUAL_INT(1, switchedAt);
	/* ...and it took the whole dwell to get there: 1.5s at 0.1s/tick is
	 * 15 ticks (the exact count is a float-accumulation detail, the
	 * bound is not). */
	TEST_ASSERT_TRUE(i >= 14);
	TEST_ASSERT_TRUE(i <= 17);
	/* The displaced job failed and gave its claim back. */
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->personal.slots[0].claimer);

	/* ...and it holds afterwards: the displaced job is FAILED, so it is
	 * not even a candidate again. The only thing that may move
	 * engagedJob from here is the new job's own completion. */
	for (i = 0; i < 50; i++) {
		runTicks(w, 1, 0.1f);
		TEST_ASSERT_TRUE(w->a->engagedJob != 0);
	}
	worldFree(w);
}

/* A finished job is released and the agent goes back to being idle; the job
 * itself stays on the board until its owner retires it. */
static void test_update_releases_a_finished_job(void)
{
	TickWorld *w = &g_world;
	Entity *e;
	int i;

	worldInit(w, 1, 1);
	/* A short walk with a zero-length wait: DONE within a few ticks. */
	pushWander(&w->a->personal, 2, 1, 5, 0.0f);

	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[0].state);

	/* Walk the body home and let the phase clock run out. */
	for (i = 0; i < 40; i++) {
		runTicks(w, 1, 0.1f);
		if (w->a->engagedJob == JOB_INVALID)
			break;
	}
	e = entityGet(&w->ents, w->body);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(2, e->tileX);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, w->a->personal.slots[0].state);
	/* Released, not just abandoned. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->personal.slots[0].claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f,
				 w->a->personal.slots[0].claimExpiry);
	/* Still enqueued: the board does not drop a job nobody retired. */
	TEST_ASSERT_EQUAL_INT(1, w->a->personal.liveCount);
	/* With nothing else to do the agent is idle, not stuck on a ghost. */
	runTicks(w, 3, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	worldFree(w);
}

/* A job whose target is nowhere walkable is denied, not walked: the lifecycle
 * fails it and the agent's body never gets a route. */
static void test_update_denies_an_unreachable_target(void)
{
	TickWorld *w = &g_world;
	Entity *e;
	AgentStats s;

	worldInit(w, 1, 1);
	pushWander(&w->a->personal, TICK_MAP_DIM + 4, 1, 5, 0.4f);

	runTicks(w, 1, 0.1f);
	e = entityGet(&w->ents, w->body);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(-1, w->a->pathLen);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	/* The failed job is released and the agent is idle again. */
	runTicks(w, 2, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->personal.slots[0].claimer);

	/* The denial still counted as a path attempt and a decision. */
	agentsStats(&s);
	TEST_ASSERT_TRUE(s.pathfindCalls >= 1u);
	worldFree(w);
}

/* An in-place activity binds no tile, so it can never be walked to: the
 * agent claims it, refuses the move and the job fails. Task 4's catalogue is
 * where in-place activities stop being wander-shaped, but the rule already
 * has to hold today. */
static void test_update_denies_an_unresolvable_target(void)
{
	TickWorld *w = &g_world;
	Job j;

	worldInit(w, 1, 1);
	jobInit(&j, JOB_NAP);	/* in place: no target at all */
	j.band = 5;
	j.duration = 1.0f;
	TEST_ASSERT_EQUAL_INT(0, boardEnqueue(&w->a->personal, &j, JOB_PRIO_FILL));

	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(-1, w->a->pathLen);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->personal.slots[0].claimer);
	worldFree(w);
}

/* A target that resolves INSIDE the map but has no floor under it is denied
 * before any A* runs: the job fails and the body is never handed a route. */
static void test_update_denies_a_void_tile(void)
{
	TickWorld *w = &g_world;
	Entity *e;

	worldInit(w, 1, 1);
	destroyVoxmap(w->map);
	w->map = holedMap();
	entitiesInit(&w->ents, w->map);
	w->body = makeBody(&w->ents, w->map, 1, 1);
	w->a->entity = w->body;
	pushWander(&w->a->personal, 5, 5, 5, 0.4f);	/* the hole */

	runTicks(w, 1, 0.1f);
	e = entityGet(&w->ents, w->body);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(-1, w->a->pathLen);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	worldFree(w);
}

/* Arrival is "the whole route is walked": a queued step that has not started
 * yet is not an arrival, so the job keeps walking. */
static void test_update_is_not_arrived_on_a_queued_step(void)
{
	TickWorld *w = &g_world;
	Entity *e;
	int i;

	worldInit(w, 1, 1);
	pushWander(&w->a->personal, 9, 1, 5, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[0].state);

	/* Freeze the body mid-route with one step queued but not begun: the
	 * queue is not an arrival. */
	e = entityGet(&w->ents, w->body);
	TEST_ASSERT_NOT_NULL(e);
	e->moving = false;
	e->pathCount = 1;
	e->pathHead = 0;
	e->path[0][0] = 4;
	e->path[0][1] = 1;
	for (i = 0; i < 3; i++)
		runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[0].state);
	worldFree(w);
}

/* A job another agent holds is not a candidate, and a claim lost in the race
 * is not a decision. The staffer's own tick is the race case: it still sees
 * the job it holds as its own best candidate and is refused, because START is
 * not a re-claim. */
static void test_update_ignores_a_job_another_agent_holds(void)
{
	TickWorld *w = &g_world;
	Agent *other;
	int decisions = 0;
	int i;

	worldInit(w, 1, 1);
	pushWander(&w->boards.shared, 5, 1, 12, 0.4f);
	boardExpire(&w->boards.shared, 1.0f);

	/* Somebody else takes it first, and the reservation is the kind the
	 * window is FOR: claimed, STARTed, and left sitting in JOB_CLAIMED
	 * ("reserved, not yet walking"). */
	other = agentGet(&w->reg, agentCreate(&w->reg, ROLE_STAFF));
	TEST_ASSERT_NOT_NULL(other);
	TEST_ASSERT_TRUE(boardClaim(&w->boards.shared, 0, other->id));
	jobStep(&w->boards.shared.slots[0], JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, w->boards.shared.slots[0].state);

	/* The staffer is ticked in the same call and must stay idle, so give
	 * it one TERMINAL job of its own: terminal work is not a candidate,
	 * and a non-empty board is not a licence to generate a fill (see
	 * agentsUpdate). Without this the staffer would correctly invent its
	 * own filler, and this test would be measuring the filler rather
	 * than the claim. */
	{
		Job done;

		jobInit(&done, JOB_PAY);
		jobStep(&done, JOB_PHASE_DONE);
		TEST_ASSERT_TRUE(boardEnqueue(&other->personal, &done,
					      JOB_PRIO_FILL) != JOB_INVALID);
	}

	/* Our critter sees an empty candidate set — the only job on either
	 * board is held by somebody else — and takes NOTHING. Its own board
	 * is empty, so the tick generates its own filler instead, which is a
	 * decision the perf counter counts but emphatically not a theft:
	 * the shared claim is untouched. */
	agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, 0.1f,
			 &decisions);
	TEST_ASSERT_EQUAL_INT(other->id, w->boards.shared.slots[0].claimer);
	TEST_ASSERT_FALSE(w->a->engagedShared);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, w->boards.shared.slots[0].state);

	/* Let the critter's own filler run to completion so it is idle again
	 * (the generated wander is in place and parks on its 3s), and retire
	 * it: an idle agent with an empty board is the state this test needs
	 * to show what it does with a lapsed claim. */
	for (i = 0; i < 80 && w->a->engagedJob != JOB_INVALID; i++)
		runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	for (i = 0; i < BOARD_MAX; i++) {
		if (w->a->personal.alive[i])
			boardRetire(&w->a->personal, i);
	}
	TEST_ASSERT_EQUAL_INT(0, w->a->personal.liveCount);

	/* Once the reservation lapses, the job is available again and the
	 * same tick takes it. */
	decisions = 0;
	boardExpire(&w->boards.shared, 1.0f + BOARD_RESERVE_SECS + 0.5f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->boards.shared.slots[0].claimer);
	agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, 0.1f,
			 &decisions);
	TEST_ASSERT_EQUAL_INT(1, decisions);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_TRUE(w->a->engagedShared);
	TEST_ASSERT_EQUAL_INT(w->a->id, w->boards.shared.slots[0].claimer);

	/* The staffer is in the same registry and ticked in the same call: it
	 * has nothing of its own, and the shared job now belongs to the
	 * critter, so the second agent still takes nothing. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, other->engagedJob);
	TEST_ASSERT_EQUAL_INT(w->a->id, w->boards.shared.slots[0].claimer);
	worldFree(w);
}

/* TWO BOARDS, TWO NAMESPACEES. A handle is a slot index ON ITS BOARD, so the
 * job an agent already holds must only ever hide the SAME slot of the SAME
 * board. Personal slot 0 and shared slot 0 are different jobs, and an agent
 * engaged on one must still see the other. */
static void test_update_sees_the_other_board_behind_its_slot_number(void)
{
	TickWorld *w = &g_world;
	Agent *a;

	worldInit(w, 1, 1);
	boardExpire(&w->boards.shared, 1.0f);
	a = w->a;
	/* The agent's own work is personal slot 0 ... */
	pushScored(a, &a->personal, ACT_PLAY_ARCADE, 3, 1, 2, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	TEST_ASSERT_FALSE(a->engagedShared);

	/* ... and the urgent work is shared slot 0 — the same NUMBER. It is
	 * band 8 against the incumbent's 2, so it clears the margin, and
	 * shared slot 1 (band 1) is left alone. 20 ticks: the AGENT_MIN_DWELL
	 * (1.5s) has to expire before ANY preemption, on either board. */
	pushScored(a, &w->boards.shared, ACT_NAP, 6, 5, 8, 4.0f);
	pushScored(a, &w->boards.shared, ACT_TIDY, 7, 5, 1, 4.0f);
	runTicks(w, 20, 0.1f);
	TEST_ASSERT_TRUE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);	/* shared slot 0 */
	TEST_ASSERT_EQUAL_INT(a->id, w->boards.shared.slots[0].claimer);
	/* The displaced personal job was released, not claimed by anyone. */
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, a->personal.slots[0].claimer);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->boards.shared.slots[1].claimer);
}

/* The mirror image: engaged on SHARED slot 0, the agent must still see
 * personal slot 0, and a personal job that clears the margin preempts. */
static void test_update_sees_the_personal_board_behind_its_slot_number(void)
{
	TickWorld *w = &g_world;
	Agent *a;

	worldInit(w, 1, 1);
	boardExpire(&w->boards.shared, 1.0f);
	a = w->a;
	pushScored(a, &w->boards.shared, ACT_PLAY_ARCADE, 6, 5, 2, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	TEST_ASSERT_TRUE(a->engagedShared);

	/* Personal slot 0 — the same NUMBER again — at band 8, which clears
	 * the margin once the dwell has run out. */
	pushScored(a, &a->personal, ACT_NAP, 3, 1, 8, 4.0f);
	runTicks(w, 20, 0.1f);
	TEST_ASSERT_FALSE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	TEST_ASSERT_EQUAL_INT(a->id, a->personal.slots[0].claimer);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->boards.shared.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->boards.shared.slots[0].claimer);
}

/* The margin is the margin ACROSS boards: a shared challenger one band short
 * of it never moves a personal incumbent, however long the dwell has run
 * out. (The anti-oscillation property does not stop at the board boundary.) */
static void test_update_holds_the_margin_across_boards(void)
{
	TickWorld *w = &g_world;
	Agent *a;
	int i;

	worldInit(w, 1, 1);
	boardExpire(&w->boards.shared, 1.0f);
	a = w->a;
	/* A long duration: this test is about the MARGIN, and a job that
	 * finished inside the 40-tick loop would let the agent go idle and
	 * wander off to the shared challenger before the decisive push. */
	pushScored(a, &a->personal, ACT_TALK, 3, 1, 3, 60.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	/* One band better, on the other board: not a reason to move. */
	pushScored(a, &w->boards.shared, ACT_COFFEE, 6, 5, 4, 4.0f);

	/* Well past AGENT_MIN_DWELL, so the dwell is not what holds it. */
	for (i = 0; i < 40; i++) {
		runTicks(w, 1, 0.1f);
		TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
		TEST_ASSERT_FALSE(a->engagedShared);
	}
	/* One more band and it moves: the rule reads the band, not the board
	 * the two jobs happen to sit on. */
	boardRetire(&w->boards.shared, 0);
	/* Re-pin the incumbent: its computed band drifted as its need decayed
	 * through the 40 ticks above, so the decisive gap is measured against
	 * a freshly pinned 3. */
	tuneToBand(a, ACT_TALK, 3);
	pushScored(a, &w->boards.shared, ACT_NAP, 7, 5, 5, 4.0f);
	runTicks(w, 2, 0.1f);
	TEST_ASSERT_TRUE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
}

/* Finding 2: a handle names a SLOT, not a job. An agent may only hold work
 * it owns, so a slot retired and refilled under it is not its job any more —
 * stepping it would be executing somebody else's claim, and releasing it
 * would clear their live claim. */
static void test_update_drops_a_slot_it_no_longer_owns(void)
{
	TickWorld *w = &g_world;
	Agent *thief;
	Job *stolen;
	float expiry;

	worldInit(w, 1, 1);
	thief = agentGet(&w->reg, agentCreate(&w->reg, ROLE_STAFF));
	TEST_ASSERT_NOT_NULL(thief);
	pushWander(&w->a->personal, 3, 1, 5, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(w->a->id, w->a->personal.slots[0].claimer);

	/* The slot is retired under the holder and refilled with work the
	 * thief has claimed AND started: it is parked on a timed phase. */
	TEST_ASSERT_TRUE(boardRetire(&w->a->personal, 0));
	pushWander(&w->a->personal, 9, 1, 12, 4.0f);
	TEST_ASSERT_TRUE(boardClaim(&w->a->personal, 0, thief->id));
	jobStep(&w->a->personal.slots[0], JOB_START);
	jobStep(&w->a->personal.slots[0], JOB_ARRIVED);	/* -> WALKING */
	jobStep(&w->a->personal.slots[0], JOB_ARRIVED);	/* -> INTERACTING */
	jobStep(&w->a->personal.slots[0], JOB_PHASE_DONE);	/* -> WAITING */
	w->a->personal.slots[0].timer = 4.0f;
	expiry = w->a->personal.slots[0].claimExpiry;

	/* Our agent is holding slot 0 and the slot is not its job. */
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_FALSE(w->a->engagedShared);

	/* It did not execute the thief's work ... */
	stolen = boardJob(&w->a->personal, 0);
	TEST_ASSERT_NOT_NULL(stolen);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, stolen->state);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 4.0f, stolen->timer);
	/* ... and it did not release the thief's claim. */
	TEST_ASSERT_EQUAL_INT(thief->id, stolen->claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, expiry, stolen->claimExpiry);
	/* Which survives the whole timed phase the thief is parked on. */
	runTicks(w, 30, 0.1f);
	stolen = boardJob(&w->a->personal, 0);
	TEST_ASSERT_EQUAL_INT(thief->id, stolen->claimer);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, stolen->state);
}

/* Both boards are scanned and the BANDS decide, never the board a job
 * happens to sit on: a shared job beats a weaker personal one, and a
 * stronger personal one beats a shared one. */
static void test_update_compares_bands_across_boards(void)
{
	TickWorld *w = &g_world;
	Agent *second;
	int decisions = 0;

	worldInit(w, 1, 1);
	boardExpire(&w->boards.shared, 1.0f);
	pushScored(w->a, &w->a->personal, ACT_PLAY_ARCADE, 3, 1, 2, 0.4f);
	pushScored(w->a, &w->boards.shared, ACT_TALK, 6, 5, 4, 0.4f);
	pushScored(w->a, &w->boards.shared, ACT_NAP, 7, 5, 8, 0.4f);

	/* The first critter's own job is the weakest of the three, so the
	 * urgent shared job is what it takes. */
	agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, 0.1f,
			 &decisions);
	TEST_ASSERT_EQUAL_INT(1, decisions);
	TEST_ASSERT_EQUAL_INT(1, w->a->engagedJob);	/* shared slot 1 */
	TEST_ASSERT_TRUE(w->a->engagedShared);
	TEST_ASSERT_EQUAL_INT(w->a->id, w->boards.shared.slots[1].claimer);

	/* A second critter with a better job of its own keeps it: provenance
	 * never orders, the band does. */
	second = agentGet(&w->reg, agentCreate(&w->reg, ROLE_CRITTER));
	TEST_ASSERT_NOT_NULL(second);
	second->entity = makeBody(&w->ents, w->map, 1, 1);
	pushScored(second, &second->personal, ACT_COFFEE, 3, 3, 5, 0.4f);
	decisions = 0;
	agentsUpdate(&w->reg, &w->ents, &w->boards, w->map, 0.1f,
			 &decisions);
	/* The first critter holds its urgent job through its dwell (its
	 * only challenger is the weaker band 4 talk), so the one decision
	 * this tick is the second critter taking its own band-5 job. */
	TEST_ASSERT_EQUAL_INT(1, decisions);
	TEST_ASSERT_EQUAL_INT(0, second->engagedJob);
	TEST_ASSERT_FALSE(second->engagedShared);
	worldFree(w);
}

/* An idle agent with an EMPTY board generates its own fill — the spec's
 * "when blocked, do secondary activities". Nothing here was queued by a
 * producer, so nothing is being talked over. */
static void test_update_generates_a_fill_on_an_empty_board(void)
{
	TickWorld *w = &g_world;
	Job *j;

	worldInit(w, 1, 1);
	/* Nothing queued, nothing engaged. */
	TEST_ASSERT_EQUAL_INT(0, w->a->personal.liveCount);

	runTicks(w, 1, 0.1f);

	/* The agent generated exactly one job of its own accord ... */
	TEST_ASSERT_EQUAL_INT(1, w->a->personal.liveCount);
	j = boardJob(&w->a->personal, 0);
	TEST_ASSERT_NOT_NULL(j);
	/* ... a real activity, taken and started ... */
	TEST_ASSERT_NOT_NULL(activitiesGet(j->kind));
	TEST_ASSERT_EQUAL_INT(w->a->id, j->claimer);
	/* ... and scored from the needs it serves, not hand-set. */
	TEST_ASSERT_EQUAL_INT(jobUrgency(w->a, j), j->band);
	worldFree(w);
}

/* A cooling-down activity is not re-picked: the fill the tick generates is
 * the best one the COOLDOWNS allow, not the best one in the catalogue. */
static void test_update_fills_with_something_the_cooldown_allows(void)
{
	TickWorld *w = &g_world;
	Job *j;

	worldInit(w, 1, 1);
	/* Energy is empty, so the nap (band 8) would win ... */
	w->a->needs.value[NEED_ENERGY] = 0.0f;
	/* ... but it is on cooldown, so the coffee break (band 5) is taken
	 * instead. A fill that ignored the cooldown would sit there for the
	 * whole cooldown doing nothing. */
	TEST_ASSERT_TRUE(activityMarkDone(&w->a->cooldown, ACT_NAP, 0.0f));

	runTicks(w, 1, 0.1f);

	j = boardJob(&w->a->personal, 0);
	TEST_ASSERT_NOT_NULL(j);
	TEST_ASSERT_EQUAL_INT(ACT_COFFEE, j->kind);
	TEST_ASSERT_EQUAL_INT(w->a->id, j->claimer);
	worldFree(w);
}

/* An idle agent with an EMPTY board, but for whom nothing is available to do,
 * enqueues nothing and stays idle. Cooldowns are what make a fill unavailable,
 * so this is the SUPPRESSION path rather than a missing one. */
static void test_update_does_not_fill_when_every_fill_is_cooling_down(void)
{
	TickWorld *w = &g_world;
	int i;

	worldInit(w, 1, 1);
	/* Every activity held off cooldown far into the future. The store is
	 * the agent's own and this is the very field activityMarkDone writes;
	 * it is set directly only because ACT_WANDER's cooldown is ZERO, so
	 * marking it done would stamp readyAt == now and leave it available —
	 * correct (a zero cooldown never suppresses) but no use for making
	 * the WHOLE catalogue unavailable. */
	for (i = 0; i < activitiesCount(); i++)
		w->a->cooldown.readyAt[activitiesAt(i)->id] = 1.0e9f;

	runTicks(w, 30, 0.1f);

	/* Idle, and still idle: an empty board is not a licence to repeat a
	 * suppressed activity, and the agent does not busy itself inventing
	 * work it has already decided against. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(0, w->a->personal.liveCount);
	worldFree(w);
}

/* A job whose activity id no row claims is a BROKEN JOB, and the tick fails
 * it rather than leaving it on the board to be compared against a margin it
 * can never win. This is Review Focus #5 end to end: the -1 out of jobUrgency
 * reaches the one place a band is written, and becomes JOB_FAILED rather than
 * a plausible-looking band 0. */
static void test_update_fails_a_job_whose_activity_is_unknown(void)
{
	TickWorld *w = &g_world;
	Job j;

	worldInit(w, 1, 1);
	jobInit(&j, 9999);	/* no catalogue row claims this kind */
	j.duration = 1.0f;
	jobSetTarget(&j, JOB_TARGET_TILE, JOB_TILE_PACK(2, 1));
	j.band = -1;
	TEST_ASSERT_EQUAL_INT(0, boardEnqueue(&w->a->personal, &j, JOB_PRIO_FILL));

	runTicks(w, 1, 0.1f);
	/* Failed on the very tick the band was computed, never engaged, and no
	 * path was ever attempted for work that cannot be scored. */
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(-1, w->a->pathLen);
	/* It holds: the tick keeps re-failing it, never resurrecting it. */
	runTicks(w, 20, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	worldFree(w);
}

/* A body destroyed mid-walk never arrives: the job is denied, the claim goes
 * back, and the agent is idle again rather than completing a walk nobody
 * made. */
static void test_update_denies_when_the_body_is_gone(void)
{
	TickWorld *w = &g_world;

	worldInit(w, 1, 1);
	pushWander(&w->a->personal, 6, 1, 5, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, w->a->personal.slots[0].state);

	/* The body goes away while the job is walking. */
	TEST_ASSERT_TRUE(entityDestroy(&w->ents, w->body));
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, w->a->personal.slots[0].claimer);
	worldFree(w);
}

/* A shared board that is no longer there cannot wedge an agent: the handle
 * that named a slot on it is released rather than held forever. */
static void test_update_releases_a_vanished_board(void)
{
	TickWorld *w = &g_world;
	Agent *a;

	worldInit(w, 1, 1);
	a = w->a;	/* read AFTER worldInit: g_world is reused per test */
	boardExpire(&w->boards.shared, 1.0f);
	pushWander(&w->boards.shared, 5, 5, 6, 4.0f);
	runTicks(w, 1, 0.1f);
	TEST_ASSERT_TRUE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);

	/* The next tick is handed no boards at all — the slot the agent was
	 * holding is unreachable, so the agent lets go of it. Its own board
	 * is empty afterwards, so the tick legitimately hands it a generated
	 * filler; what matters is that the filler is a PERSONAL one and the
	 * shared job it could no longer reach was dropped, not adopted. */
	agentsUpdate(&w->reg, &w->ents, NULL, w->map, 0.1f, NULL);
	TEST_ASSERT_FALSE(a->engagedShared);
	TEST_ASSERT_EQUAL_INT(0, a->engagedJob);
	TEST_ASSERT_EQUAL_INT(1, a->personal.liveCount);
	worldFree(w);
}

static void test_update_guards_and_clamping(void)
{
	TickWorld *w = &g_world;
	AgentStats s;

	/* NULL registry: a no-op that reports no decisions. */
	TEST_ASSERT_EQUAL_INT(0, agentsUpdate(NULL, &w->ents, &w->boards,
					      w->map, 0.1f, NULL));

	worldInit(w, 1, 1);
	pushWander(&w->a->personal, 2, 1, 5, 4.0f);

	/* NULL out-param, NULL boards (no shared board at all) and NULL
	 * entity registry all still make the decision — and a body that
	 * cannot be reached makes the MOVE a denial, not a silent no-op:
	 * work nobody can run is work that fails. */
	TEST_ASSERT_EQUAL_INT(1, agentsUpdate(&w->reg, NULL, NULL, w->map,
					      0.1f, NULL));
	TEST_ASSERT_EQUAL_INT(0, w->a->engagedJob);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, w->a->personal.slots[0].state);
	TEST_ASSERT_EQUAL_INT(-1, w->a->pathLen);

	/* The claiming tick SETS the dwell, so it does not also spend one.
	 * dt is then clamped to AGENT_MAX_DT, NaN-safe: a 10-second stall
	 * buys the agent 0.1s of dwell, and a NaN buys none. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, AGENT_MIN_DWELL, w->a->dwellLeft);
	runTicks(w, 1, 10.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, AGENT_MIN_DWELL - 0.1f,
				 w->a->dwellLeft);
	{
		float before = w->a->dwellLeft;

		runTicks(w, 1, 0.0f / 0.0f);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, before, w->a->dwellLeft);
	}

	/* The stats read back as a delta and then zero themselves. */
	agentsStats(&s);
	TEST_ASSERT_TRUE(s.agentDecisions >= 1u);
	TEST_ASSERT_TRUE(s.pathfindCalls >= 1u);
	agentsStats(&s);
	TEST_ASSERT_EQUAL_UINT(0u, s.agentDecisions);
	TEST_ASSERT_EQUAL_UINT(0u, s.pathfindCalls);
	worldFree(w);
}

static void test_update_is_deterministic(void)
{
	static TickWorld a, b;
	int i;

	worldInit(&a, 1, 1);
	worldInit(&b, 1, 1);
	for (i = 0; i < 4; i++) {
		pushWander(&a.a->personal, 2 + i, 1, i * 3, 0.4f);
		pushWander(&b.a->personal, 2 + i, 1, i * 3, 0.4f);
	}
	pushWander(&a.boards.shared, 5, 5, 9, 0.4f);
	pushWander(&b.boards.shared, 5, 5, 9, 0.4f);
	boardExpire(&a.boards.shared, 1.0f);
	boardExpire(&b.boards.shared, 1.0f);

	for (i = 0; i < 200; i++)
		runTicks(&a, 1, 0.1f);
	for (i = 0; i < 200; i++)
		runTicks(&b, 1, 0.1f);

	TEST_ASSERT_EQUAL_MEMORY(&a.reg, &b.reg, sizeof(a.reg));
	TEST_ASSERT_EQUAL_MEMORY(&a.boards, &b.boards, sizeof(a.boards));
	worldFree(&a);
	worldFree(&b);
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
	RUN_TEST(test_should_switch_table);
	RUN_TEST(test_should_switch_does_not_oscillate);
	RUN_TEST(test_update_engages_the_best_job);
	RUN_TEST(test_update_does_not_oscillate_on_a_near_equal_band);
	RUN_TEST(test_update_preempts_once_on_a_decisive_challenger);
	RUN_TEST(test_update_releases_a_finished_job);
	RUN_TEST(test_update_denies_an_unreachable_target);
	RUN_TEST(test_update_denies_an_unresolvable_target);
	RUN_TEST(test_update_generates_a_fill_on_an_empty_board);
	RUN_TEST(test_update_fills_with_something_the_cooldown_allows);
	RUN_TEST(test_update_does_not_fill_when_every_fill_is_cooling_down);
	RUN_TEST(test_update_fails_a_job_whose_activity_is_unknown);
	RUN_TEST(test_update_denies_a_void_tile);
	RUN_TEST(test_update_is_not_arrived_on_a_queued_step);
	RUN_TEST(test_update_ignores_a_job_another_agent_holds);
	RUN_TEST(test_update_sees_the_other_board_behind_its_slot_number);
	RUN_TEST(test_update_sees_the_personal_board_behind_its_slot_number);
	RUN_TEST(test_update_holds_the_margin_across_boards);
	RUN_TEST(test_update_drops_a_slot_it_no_longer_owns);
	RUN_TEST(test_update_compares_bands_across_boards);
	RUN_TEST(test_update_denies_when_the_body_is_gone);
	RUN_TEST(test_update_releases_a_vanished_board);
	RUN_TEST(test_update_guards_and_clamping);
	RUN_TEST(test_update_is_deterministic);
}
