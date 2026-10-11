/*
 * Need vector + want-ladder tests (CTOL rung 1: unit + boundary).
 *
 * Covers: needSetInit's content-and-calm default (panic starts clear, so a
 * fresh vector is not suppressed); the six EXACT tier weights and their
 * monotone non-increasing falloff in spec-tier order; the band mapping's
 * exact table (every need at content / half / empty, the [0,1] deficit clamp
 * and the unknown-slot fallback); the panic mask; wantUrgency's soft falloff,
 * the panicking-need override and the NEED_PANIC_SUPPRESS cross-need
 * suppression (all pinned to exact integers, not to inequalities); needDecay's
 * dt clamp, panic ramp, multiplier, non-negativity, NaN-safe dt and NULL
 * guards; needPanicDecay's full lifecycle — the RISE while a need sits under
 * its threshold (clamped at 1.0), the BLEED once it is back above (floored at
 * 0.0), the negative-threshold RESERVATION that silences every non-survival
 * need, and the dt clamp / NaN guard on the rise.
 *
 * Arithmetic notes: every band expectation is an exact integer, and the decay
 * probes use a dyadic dt (1/16 s) with dyadic multipliers so the floats are
 * exact too — TEST_ASSERT_FLOAT_WITHIN(0.0f, ...) is therefore a bit-exact
 * pin, not a loose one.
 *
 * Pure: links need.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_need().
 */

#include "unity.h"

#include "agents/need.h"

#define EPS 1e-6f

/* A need set with every slot at the same value / panic level. */
static NeedSet uniformSet(float value, float panic)
{
	NeedSet s;
	int n;

	for (n = 0; n < NEED_COUNT; n++) {
		s.value[n] = value;
		s.panic[n] = panic;
	}
	return s;
}

/* Every need content (1.0) and calm (panic 0). */
static NeedSet contentSet(void)
{
	return uniformSet(1.0f, 0.0f);
}

/* Every need empty (0.0) and calm. */
static NeedSet emptySet(void)
{
	return uniformSet(0.0f, 0.0f);
}

static void mulAll(float *mul, float m)
{
	int n;

	for (n = 0; n < NEED_COUNT; n++)
		mul[n] = m;
}

/* ---- the want ladder's pinned constants --------------------------------- */

static void test_need_constants(void)
{
	TEST_ASSERT_EQUAL_INT(6, NEED_COUNT);
	TEST_ASSERT_EQUAL_INT(16, NEED_BANDS);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.1f, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, NEED_PANIC_RAMP);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f, NEED_PANIC_DECAY);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f, NEED_PANIC_RISE);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.75f, NEED_PANIC_SUPPRESS);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.15f, NEED_PANIC_HOLD_DEFAULT);
}

static void test_need_set_init_is_content_and_calm(void)
{
	NeedSet s = uniformSet(0.0f, 0.0f);
	int n;

	needSetInit(&s);
	for (n = 0; n < NEED_COUNT; n++) {
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.value[n]);
		/* Panic starts CLEAR: it is an alarm the world raises, not a
		 * content quantity. Starting it at 1.0 pinned every want at
		 * its maximum and never recovered. */
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.panic[n]);
	}
	TEST_ASSERT_EQUAL_INT(0, needPanicMask(&s));

	/* A fresh set is NOT suppressed: wantUrgency is the plain band, for
	 * a content need and for an emptied one alike. */
	for (n = 0; n < NEED_COUNT; n++) {
		TEST_ASSERT_EQUAL_INT(0, needBand(&s, n));
		TEST_ASSERT_EQUAL_INT(0, wantUrgency(&s, n));
	}
	s.value[NEED_SURVIVAL] = 0.0f;
	TEST_ASSERT_EQUAL_INT(NEED_BANDS - 1, wantUrgency(&s, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(needBand(&s, NEED_SURVIVAL),
			      wantUrgency(&s, NEED_SURVIVAL));

	needSetInit(NULL);	/* NULL is a no-op, not a crash */
}

/* ---- tier weights ------------------------------------------------------- */

static void test_tier_weights_exact(void)
{
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.00f, needTierWeight(NEED_SURVIVAL));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.60f, needTierWeight(NEED_ENERGY));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.40f, needTierWeight(NEED_SOCIAL));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.60f, needTierWeight(NEED_SATISFACTION));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.25f, needTierWeight(NEED_ESTEEM));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.15f, needTierWeight(NEED_FULFILMENT));
}

/* The soft falloff is monotone non-increasing in SPEC tier (0 survival, 1
 * sustenance/satisfaction, 2 belonging, 3 esteem, 4 fulfilment) — NOT in enum
 * order, which is why SATISFACTION (0.60) sits above SOCIAL (0.40). Being
 * blocked is an inconvenience, not an emergency. */
static void test_tier_weights_fall_off_with_tier(void)
{
	static const int specTierOrder[NEED_COUNT] = {
		NEED_SURVIVAL, NEED_ENERGY, NEED_SATISFACTION,
		NEED_SOCIAL,	 NEED_ESTEEM,	 NEED_FULFILMENT,
	};
	int i;

	for (i = 0; i < NEED_COUNT - 1; i++) {
		float hi = needTierWeight(specTierOrder[i]);
		float lo = needTierWeight(specTierOrder[i + 1]);

		TEST_ASSERT_TRUE(hi >= lo);
	}
	/* The shared tier-1 weight is exact, and the survival / fulfilment ends
	 * are the pinned bookends of the curve. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, needTierWeight(NEED_ENERGY),
				 needTierWeight(NEED_SATISFACTION));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.00f, needTierWeight(NEED_SURVIVAL));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.15f, needTierWeight(NEED_FULFILMENT));
	/* Unknown slot: no weight at all, so it can never outrank a real need. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, needTierWeight(NEED_COUNT));
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, needTierWeight(-1));
}

/* ---- band mapping ------------------------------------------------------- */

static void bandIs(int need, float value, int expected)
{
	NeedSet s = contentSet();

	s.value[need] = value;
	TEST_ASSERT_EQUAL_INT(expected, needBand(&s, need));
}

static void test_band_endpoints(void)
{
	NeedSet content = contentSet();
	NeedSet empty = emptySet();

	TEST_ASSERT_EQUAL_INT(0, needBand(&content, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(15, needBand(&empty, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(NEED_BANDS - 1, needBand(&empty, NEED_SURVIVAL));
}

static void test_band_table_is_exact(void)
{
	/* value 1.0 (content): deficit 0 -> band 0 for every need. */
	int n;

	for (n = 0; n < NEED_COUNT; n++)
		bandIs(n, 1.0f, 0);

	/* value 0.5: deficit 0.5 -> band = round(0.5 * weight * 15). */
	bandIs(NEED_SURVIVAL, 0.5f, 8);
	bandIs(NEED_ENERGY, 0.5f, 5);
	bandIs(NEED_SATISFACTION, 0.5f, 5);
	bandIs(NEED_SOCIAL, 0.5f, 3);
	bandIs(NEED_ESTEEM, 0.5f, 2);
	bandIs(NEED_FULFILMENT, 0.5f, 1);

	/* value 0.0 (empty): band = round(weight * 15) — the tier's ceiling. */
	bandIs(NEED_SURVIVAL, 0.0f, 15);
	bandIs(NEED_ENERGY, 0.0f, 9);
	bandIs(NEED_SOCIAL, 0.0f, 6);
	bandIs(NEED_SATISFACTION, 0.0f, 9);
	bandIs(NEED_ESTEEM, 0.0f, 4);
	bandIs(NEED_FULFILMENT, 0.0f, 2);

	/* The ceiling is strictly ordered by tier weight, never equal. */
	TEST_ASSERT_TRUE(needTierWeight(NEED_SURVIVAL) >
			 needTierWeight(NEED_ENERGY));
	TEST_ASSERT_TRUE(needTierWeight(NEED_ENERGY) >
			 needTierWeight(NEED_SOCIAL));
	TEST_ASSERT_TRUE(needTierWeight(NEED_SOCIAL) >
			 needTierWeight(NEED_ESTEEM));
	TEST_ASSERT_TRUE(needTierWeight(NEED_ESTEEM) >
			 needTierWeight(NEED_FULFILMENT));
}

static void test_band_deficit_is_clamped(void)
{
	/* Over-full reads as content, under-empty as empty. */
	bandIs(NEED_SURVIVAL, 1.5f, 0);
	bandIs(NEED_ENERGY, 2.0f, 0);
	bandIs(NEED_SURVIVAL, -0.5f, 15);
	bandIs(NEED_FULFILMENT, -1.0f, 2);
}

static void test_band_null_and_unknown_slot(void)
{
	NeedSet s = contentSet();

	/* An unknown slot carries no weight -> band 0, never a wild read. */
	TEST_ASSERT_EQUAL_INT(0, needBand(&s, NEED_COUNT));
	TEST_ASSERT_EQUAL_INT(0, needBand(&s, -1));
	TEST_ASSERT_EQUAL_INT(0, needBand(NULL, NEED_SURVIVAL));
}

/* ---- the panic mask ----------------------------------------------------- */

static void test_panic_mask(void)
{
	NeedSet s = contentSet();

	TEST_ASSERT_EQUAL_INT(0, needPanicMask(&s));

	s.panic[NEED_ENERGY] = 0.5f;
	TEST_ASSERT_EQUAL_INT(1 << NEED_ENERGY, needPanicMask(&s));

	s.panic[NEED_SURVIVAL] = 0.01f;
	s.panic[NEED_FULFILMENT] = 0.01f;
	TEST_ASSERT_EQUAL_INT((1 << NEED_SURVIVAL) | (1 << NEED_ENERGY) |
				      (1 << NEED_FULFILMENT),
			      needPanicMask(&s));

	/* Decayed-to-zero panic is over: the bit clears. */
	s.panic[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_EQUAL_INT((1 << NEED_SURVIVAL) | (1 << NEED_FULFILMENT),
			      needPanicMask(&s));

	/* A negative level (never produced by needPanicDecay, defensive) is not
	 * a panic. */
	s.panic[NEED_SURVIVAL] = -1.0f;
	s.panic[NEED_FULFILMENT] = -1.0f;
	TEST_ASSERT_EQUAL_INT(0, needPanicMask(&s));

	TEST_ASSERT_EQUAL_INT(0, needPanicMask(NULL));
}

static void test_panic_mask_full_set(void)
{
	NeedSet s = uniformSet(1.0f, 1.0f);

	TEST_ASSERT_EQUAL_INT((1 << NEED_COUNT) - 1, needPanicMask(&s));
}

/* ---- wantUrgency: the single ordering function -------------------------- */

static void test_urgency_matches_band_when_calm(void)
{
	NeedSet s = contentSet();

	s.value[NEED_SURVIVAL] = 0.5f;
	TEST_ASSERT_EQUAL_INT(8, wantUrgency(&s, NEED_SURVIVAL));
	s.value[NEED_SURVIVAL] = 0.0f;
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&s, NEED_SURVIVAL));
	s.value[NEED_ESTEEM] = 0.5f;
	TEST_ASSERT_EQUAL_INT(2, wantUrgency(&s, NEED_ESTEEM));
}

/* Soft falloff: a mild survival need loses to a severe social one ... */
static void test_urgency_soft_falloff(void)
{
	NeedSet mildSurvival = contentSet();
	NeedSet criticalSocial = contentSet();

	mildSurvival.value[NEED_SURVIVAL] = 0.9f;	/* deficit 0.1 -> band 2 */
	criticalSocial.value[NEED_SOCIAL] = 0.0f;	/* deficit 1.0 -> band 6 */

	TEST_ASSERT_EQUAL_INT(2, wantUrgency(&mildSurvival, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(6, wantUrgency(&criticalSocial, NEED_SOCIAL));
	TEST_ASSERT_TRUE(wantUrgency(&criticalSocial, NEED_SOCIAL) >
			 wantUrgency(&mildSurvival, NEED_SURVIVAL));
}

/* ... but a critical survival need beats a critical social one. */
static void test_urgency_tier_ordering(void)
{
	NeedSet criticalSurvival = contentSet();
	NeedSet criticalSocial = contentSet();

	criticalSurvival.value[NEED_SURVIVAL] = 0.0f;
	criticalSocial.value[NEED_SOCIAL] = 0.0f;

	TEST_ASSERT_TRUE(wantUrgency(&criticalSurvival, NEED_SURVIVAL) >
			 wantUrgency(&criticalSocial, NEED_SOCIAL));
}

/* Panic overrides: a panicking need is maximal whatever its value, and the
 * override is deterministic across repeated reads. */
static void test_urgency_panicking_need_is_maximal(void)
{
	NeedSet panicking = contentSet();

	panicking.panic[NEED_ENERGY] = 0.5f;

	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&panicking, NEED_ENERGY));
	TEST_ASSERT_TRUE(wantUrgency(&panicking, NEED_SURVIVAL) ==
			 wantUrgency(&panicking, NEED_SURVIVAL));

	/* Even a CONTENT need is maximal while IT panics: the mode change is
	 * read from the panic channel, not the value channel. */
	panicking.panic[NEED_FULFILMENT] = 0.5f;
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&panicking, NEED_FULFILMENT));
	/* ...and a need that is NOT panicking is only suppressed, never
	 * promoted: somebody else's panic pushes wants down, not up. */
	panicking.panic[NEED_FULFILMENT] = 0.0f;
	TEST_ASSERT_EQUAL_INT(0, wantUrgency(&panicking, NEED_FULFILMENT));
	TEST_ASSERT_EQUAL_INT(needBand(&panicking, NEED_SURVIVAL),
			      wantUrgency(&panicking, NEED_SURVIVAL));
}

/* While ANY need panics, the non-panicking wants are scaled by
 * NEED_PANIC_SUPPRESS (0.75) — pinned as an exact integer, not an inequality:
 * an empty ENERGY is band 9 on its own and band 7 once survival panics. */
static void test_urgency_panic_suppresses_others(void)
{
	NeedSet calm = contentSet();
	NeedSet panicAndHigh = contentSet();

	calm.value[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&calm, NEED_ENERGY));

	panicAndHigh.value[NEED_ENERGY] = 0.0f;
	panicAndHigh.panic[NEED_SURVIVAL] = 1.0f;
	TEST_ASSERT_EQUAL_INT(7, wantUrgency(&panicAndHigh, NEED_ENERGY));
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&panicAndHigh, NEED_SURVIVAL));
	TEST_ASSERT_TRUE(wantUrgency(&panicAndHigh, NEED_ENERGY) <
			 wantUrgency(&panicAndHigh, NEED_SURVIVAL));

	/* Suppression scales down; it never raises a band. A content need stays
	 * band 0, and the panicking need keeps its override. */
	TEST_ASSERT_EQUAL_INT(0, wantUrgency(&panicAndHigh, NEED_SOCIAL));
}

/* Suppression is applied after banding, so a band that rounding would leave
 * unchanged stays unchanged — pinned so the order of the two steps is
 * observable rather than incidental. */
static void test_urgency_suppression_rounds_after_scaling(void)
{
	NeedSet s = contentSet();

	/* FULFILMENT empty is band 2; 2 * 0.75 = 1.5, which rounds back to 2. */
	s.value[NEED_FULFILMENT] = 0.0f;
	TEST_ASSERT_EQUAL_INT(2, needBand(&s, NEED_FULFILMENT));
	s.panic[NEED_SURVIVAL] = 1.0f;
	TEST_ASSERT_EQUAL_INT(2, wantUrgency(&s, NEED_FULFILMENT));

	/* SURVIVAL half-empty is band 8; 8 * 0.75 = 6.0 -> 6. The suppression
	 * comes from ENERGY panicking — if SURVIVAL itself panicked it would
	 * take its own maximal override and never be scaled. */
	s.value[NEED_SURVIVAL] = 0.5f;
	s.panic[NEED_SURVIVAL] = 0.0f;
	s.panic[NEED_ENERGY] = 1.0f;
	TEST_ASSERT_EQUAL_INT(6, wantUrgency(&s, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&s, NEED_ENERGY));
}

static void test_urgency_null_and_unknown_slot(void)
{
	NeedSet s = contentSet();

	s.value[NEED_SURVIVAL] = 0.0f;
	TEST_ASSERT_EQUAL_INT(0, wantUrgency(NULL, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(0, wantUrgency(&s, NEED_COUNT));
	TEST_ASSERT_EQUAL_INT(0, wantUrgency(&s, -1));
}

static void test_urgency_is_deterministic(void)
{
	NeedSet s = contentSet();
	int n;

	s.value[NEED_SOCIAL] = 0.3f;
	s.value[NEED_ESTEEM] = 0.7f;
	s.panic[NEED_ENERGY] = 0.2f;
	for (n = 0; n < NEED_COUNT; n++) {
		int first = wantUrgency(&s, n);
		int i;

		for (i = 0; i < 8; i++)
			TEST_ASSERT_EQUAL_INT(first, wantUrgency(&s, n));
	}
}

/* ---- needDecay ---------------------------------------------------------- */

static void test_decay_dyadic_amount(void)
{
	NeedSet s = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 1.0f);
	needDecay(&s, mul, 0.0625f);
	/* Content decays at the base rate: 1.0 - 1 * (1/16). */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, s.value[NEED_SURVIVAL]);

	s = contentSet();
	mulAll(mul, 2.0f);
	needDecay(&s, mul, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.875f, s.value[NEED_SURVIVAL]);
}

/* Deceleration accelerates near empty (NEED_PANIC_RAMP): the loss per step
 * grows as the need empties. All three probes are dyadic, so the comparison is
 * exact. */
static void test_decay_ramp_accelerates_near_empty(void)
{
	NeedSet s;
	float mul[NEED_COUNT];
	float lossContent, lossHalf, lossNearEmpty;

	mulAll(mul, 1.0f);

	s = contentSet();
	s.value[NEED_SURVIVAL] = 1.0f;
	needDecay(&s, mul, 0.0625f);
	lossContent = 1.0f - s.value[NEED_SURVIVAL];
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0625f, lossContent);

	s = contentSet();
	s.value[NEED_SURVIVAL] = 0.5f;
	needDecay(&s, mul, 0.0625f);
	lossHalf = 0.5f - s.value[NEED_SURVIVAL];
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.09375f, lossHalf);

	s = contentSet();
	s.value[NEED_SURVIVAL] = 0.125f;
	needDecay(&s, mul, 0.0625f);
	lossNearEmpty = 0.125f - s.value[NEED_SURVIVAL];
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.1171875f, lossNearEmpty);

	TEST_ASSERT_TRUE(lossContent < lossHalf);
	TEST_ASSERT_TRUE(lossHalf < lossNearEmpty);
}

/* The ramp factor is exactly 1.0 + (1 - value) * NEED_PANIC_RAMP. */
static void test_decay_ramp_formula(void)
{
	NeedSet s = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 1.0f);
	s.value[NEED_SURVIVAL] = 0.25f;
	needDecay(&s, mul, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f,
				 0.25f - (1.0f + 0.75f * NEED_PANIC_RAMP) * 0.0625f,
				 s.value[NEED_SURVIVAL]);
}

static void test_decay_clamps_dt(void)
{
	NeedSet atMax = contentSet();
	NeedSet huge = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 1.0f);
	needDecay(&atMax, mul, NEED_MAX_DT);
	needDecay(&huge, mul, 100.0f);	/* clamped to NEED_MAX_DT */

	TEST_ASSERT_FLOAT_WITHIN(EPS, atMax.value[NEED_SURVIVAL],
				 huge.value[NEED_SURVIVAL]);
	TEST_ASSERT_TRUE(atMax.value[NEED_SURVIVAL] < 1.0f);
	/* A second clamped step is still bounded: 100 s of stall is 0.2 s. */
	needDecay(&huge, mul, 100.0f);
	TEST_ASSERT_TRUE(huge.value[NEED_SURVIVAL] > 0.75f);
}

static void test_decay_ignores_nonpositive_and_nan_dt(void)
{
	NeedSet s = contentSet();
	NeedSet nan = contentSet();
	float mul[NEED_COUNT];
	float zero = 0.0f;

	mulAll(mul, 1.0f);
	needDecay(&s, mul, 0.0f);
	needDecay(&s, mul, -1.0f);
	needDecay(&nan, mul, zero / zero);	/* NaN */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, nan.value[NEED_SURVIVAL]);
}

static void test_decay_never_goes_negative(void)
{
	/* Start from a fully-initialised vector: needDecay walks all six slots,
	 * so leaving the other eleven floats uninitialised would make the
	 * suite read indeterminate memory (valgrind catches it even though the
	 * assertions still pass by luck). */
	NeedSet s = contentSet();
	float mul[NEED_COUNT];
	int i;

	mulAll(mul, 1.0f);
	s.value[NEED_SURVIVAL] = 0.05f;
	for (i = 0; i < 1000; i++)
		needDecay(&s, mul, NEED_MAX_DT);

	TEST_ASSERT_TRUE(s.value[NEED_SURVIVAL] >= 0.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.value[NEED_SURVIVAL]);
	/* Still safe at zero (no oscillation into negative). */
	needDecay(&s, mul, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.value[NEED_SURVIVAL]);
}

static void test_decay_zero_multiplier_freezes(void)
{
	NeedSet s = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 0.0f);
	needDecay(&s, mul, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.value[NEED_SURVIVAL]);
}

static void test_decay_null_guards(void)
{
	NeedSet s = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 1.0f);
	needDecay(NULL, mul, 0.1f);
	needDecay(&s, NULL, 0.1f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.value[NEED_SURVIVAL]);
}

static void test_decay_is_per_need(void)
{
	NeedSet s = contentSet();
	float mul[NEED_COUNT];

	mulAll(mul, 1.0f);
	mul[NEED_SOCIAL] = 4.0f;
	s.value[NEED_ESTEEM] = 1.0f;
	needDecay(&s, mul, 0.0625f);

	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.75f, s.value[NEED_SOCIAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, s.value[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.9375f, s.value[NEED_ESTEEM]);
}

/* ---- needPanicDecay ----------------------------------------------------- */

static void test_panic_decay_dyadic_amount(void)
{
	NeedSet s = uniformSet(1.0f, 1.0f);

	needPanicDecay(&s, 0.0625f);
	/* NEED_PANIC_DECAY (0.5) per second, dt 1/16 -> 1.0 - 0.03125. */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.96875f, s.panic[NEED_SURVIVAL]);
}

/* Below the default threshold the panic RISES — the cause is live, so the
 * alarm climbs. dyadic dt and rate: the expectation is bit-exact. */
static void test_panic_rises_below_threshold(void)
{
	NeedSet s = uniformSet(NEED_PANIC_HOLD_DEFAULT / 2.0f, 0.0f);

	needPanicDecay(&s, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, NEED_PANIC_RISE * 0.0625f,
				 s.panic[NEED_SURVIVAL]);
	needPanicDecay(&s, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 2.0f * NEED_PANIC_RISE * 0.0625f,
				 s.panic[NEED_SURVIVAL]);

	/* Repeatedly driven, it saturates at the 1.0 ceiling and stops. */
	{
		int i;

		for (i = 0; i < 200; i++)
			needPanicDecay(&s, NEED_MAX_DT);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.panic[NEED_SURVIVAL]);
		needPanicDecay(&s, NEED_MAX_DT);
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.panic[NEED_SURVIVAL]);
	}

	/* Above the threshold the alarm has nothing to climb toward: it
	 * bleeds instead. */
	s.value[NEED_SURVIVAL] = 1.0f;
	needPanicDecay(&s, 0.0625f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f,
				 1.0f - NEED_PANIC_DECAY * 0.0625f,
				 s.panic[NEED_SURVIVAL]);
}

/* A negative threshold RESERVES the need: emptied and still silent, and a
 * hand-written panic level is left exactly where it stands. */
static void test_reserved_need_never_panics(void)
{
	float reserved[NEED_COUNT];
	NeedSet s;
	int n;

	mulAll(reserved, -1.0f);

	s = uniformSet(0.0f, 0.0f);
	needPanicDecayRole(&s, reserved, NEED_MAX_DT);
	for (n = 0; n < NEED_COUNT; n++)
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.panic[n]);

	/* The reservation is not a "never rise" rule but a "never touch"
	 * rule: even a panic someone wrote by hand survives untouched. */
	s = uniformSet(0.0f, 0.6f);
	needPanicDecayRole(&s, reserved, NEED_MAX_DT);
	for (n = 0; n < NEED_COUNT; n++)
		TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.6f, s.panic[n]);

	/* The role-shaped vector: survival armed, the rest reserved. */
	mulAll(reserved, -1.0f);
	reserved[NEED_SURVIVAL] = NEED_PANIC_HOLD_DEFAULT;
	s = uniformSet(0.0f, 0.0f);
	needPanicDecayRole(&s, reserved, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, NEED_PANIC_RISE * NEED_MAX_DT,
				 s.panic[NEED_SURVIVAL]);
	for (n = 0; n < NEED_COUNT; n++)
		if (n != NEED_SURVIVAL)
			TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.panic[n]);
	TEST_ASSERT_EQUAL_INT(1 << NEED_SURVIVAL, needPanicMask(&s));

	/* A reserved need is still fully urgent — through its band. */
	s = uniformSet(0.0f, 0.0f);
	TEST_ASSERT_EQUAL_INT(NEED_BANDS - 1,
			      needBand(&s, NEED_SURVIVAL));
	TEST_ASSERT_EQUAL_INT(2, needBand(&s, NEED_FULFILMENT));
	TEST_ASSERT_EQUAL_INT(2, wantUrgency(&s, NEED_FULFILMENT));
}

/* The rise obeys the same dt clamp and NaN guard as every decay entry. */
static void test_panic_rise_clamps_dt_and_ignores_nan(void)
{
	NeedSet atMax = uniformSet(0.0f, 0.0f);
	NeedSet huge = uniformSet(0.0f, 0.0f);
	NeedSet nan = uniformSet(0.0f, 0.3f);
	float zero = 0.0f;

	needPanicDecay(&atMax, NEED_MAX_DT);
	needPanicDecay(&huge, 100.0f);	/* clamped to NEED_MAX_DT */
	TEST_ASSERT_FLOAT_WITHIN(0.0f, NEED_PANIC_RISE * NEED_MAX_DT,
				 atMax.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, atMax.panic[NEED_SURVIVAL],
				 huge.panic[NEED_SURVIVAL]);
	/* A 100 s stall is one clamped step, nowhere near the 1.0 ceiling. */
	TEST_ASSERT_TRUE(huge.panic[NEED_SURVIVAL] < 0.2f);

	/* A huge dt cannot overshoot the ceiling either. */
	huge = uniformSet(0.0f, 0.99f);
	needPanicDecay(&huge, 100.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, huge.panic[NEED_SURVIVAL]);

	needPanicDecay(&nan, zero / zero);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.3f, nan.panic[NEED_SURVIVAL]);
}

/* The full lifecycle: empty the need, the alarm rises and suppresses
 * everything else; restore it, the alarm bleeds off and the ladder returns to
 * its unsuppressed ordering. */
static void test_panic_lifecycle_raise_then_bleed(void)
{
	NeedSet panicking = uniformSet(0.0f, 0.0f);
	NeedSet calm = uniformSet(0.0f, 0.0f);

	/* Emptying survival raises its alarm; ENERGY (reserved) stays clear. */
	needPanicDecay(&panicking, NEED_MAX_DT);
	TEST_ASSERT_TRUE(panicking.panic[NEED_SURVIVAL] > 0.0f);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, panicking.panic[NEED_ENERGY]);
	TEST_ASSERT_EQUAL_INT(1 << NEED_SURVIVAL,
			      needPanicMask(&panicking));
	TEST_ASSERT_EQUAL_INT(15, wantUrgency(&panicking, NEED_SURVIVAL));

	/* An emptied ENERGY is band 9 alone, band 7 once survival panics —
	 * the documented NEED_PANIC_SUPPRESS rounding, measured against the
	 * same need with no panic active. */
	panicking.value[NEED_ENERGY] = 0.0f;
	calm.value[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&calm, NEED_ENERGY));
	TEST_ASSERT_EQUAL_INT(7, wantUrgency(&panicking, NEED_ENERGY));
	TEST_ASSERT_TRUE(wantUrgency(&panicking, NEED_ENERGY) <
			 wantUrgency(&calm, NEED_ENERGY));

	/* Cause cleared: the alarm bleeds to zero and the suppression lifts. */
	panicking.value[NEED_SURVIVAL] = 1.0f;
	{
		int i;

		for (i = 0; i < 8; i++)
			needPanicDecay(&panicking, 0.25f);
	}
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, panicking.panic[NEED_SURVIVAL]);
	TEST_ASSERT_EQUAL_INT(0, needPanicMask(&panicking));
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&panicking, NEED_ENERGY));
}

static void test_panic_decay_floors_at_zero(void)
{
	NeedSet s = uniformSet(1.0f, 0.01f);

	/* Only the armed slot bleeds; the reserved ones are left exactly as
	 * they were, so they still contribute their bits. */
	needPanicDecay(&s, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.01f, s.panic[NEED_ENERGY]);
	TEST_ASSERT_EQUAL_INT(((1 << NEED_COUNT) - 1) &
			      ~(1 << NEED_SURVIVAL), needPanicMask(&s));

	/* Idempotent at zero. */
	needPanicDecay(&s, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, s.panic[NEED_SURVIVAL]);
}

/* Each need moves on its own threshold: under it the alarm rises, over it the
 * alarm bleeds. Pinned through the role-aware entry point so more than one
 * slot can be armed at once (the shipped profiles arm survival only). */
static void test_panic_move_is_per_need(void)
{
	float threshold[NEED_COUNT];
	NeedSet s = uniformSet(1.0f, 0.5f);

	mulAll(threshold, -1.0f);
	threshold[NEED_SURVIVAL] = 0.20f;
	threshold[NEED_ENERGY] = 0.80f;

	/* SURVIVAL under its threshold rises; ENERGY over its bleeds; the
	 * reserved slots are untouched. */
	s.value[NEED_SURVIVAL] = 0.10f;
	s.value[NEED_ENERGY] = 0.90f;
	needPanicDecayRole(&s, threshold, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f + NEED_PANIC_RISE * NEED_MAX_DT,
				 s.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f - NEED_PANIC_DECAY * NEED_MAX_DT,
				 s.panic[NEED_ENERGY]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f, s.panic[NEED_SOCIAL]);
	/* Every slot is still above zero here — the reserved ones were never
	 * touched — so the mask stays full. */
	TEST_ASSERT_EQUAL_INT((1 << NEED_COUNT) - 1, needPanicMask(&s));

	/* Swap them: the same two needs now take the other branch. */
	s = uniformSet(1.0f, 0.5f);
	s.value[NEED_SURVIVAL] = 0.30f;
	s.value[NEED_ENERGY] = 0.70f;
	needPanicDecayRole(&s, threshold, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f - NEED_PANIC_DECAY * NEED_MAX_DT,
				 s.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f + NEED_PANIC_RISE * NEED_MAX_DT,
				 s.panic[NEED_ENERGY]);
}

/* The per-role entry point honours the caller's threshold vector. */
static void test_panic_decay_role_thresholds(void)
{
	NeedSet s = uniformSet(1.0f, 1.0f);
	float threshold[NEED_COUNT];

	/* A strict role: the need must recover past 0.5 before the alarm
	 * starts bleeding, and below 0.5 the alarm RISES. */
	mulAll(threshold, -1.0f);
	threshold[NEED_ENERGY] = 0.50f;

	s.value[NEED_ENERGY] = 0.30f;
	needPanicDecayRole(&s, threshold, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.panic[NEED_ENERGY]);	/* ceiling */

	s = uniformSet(1.0f, 1.0f);
	s.value[NEED_ENERGY] = 0.60f;
	needPanicDecayRole(&s, threshold, NEED_MAX_DT);
	TEST_ASSERT_TRUE(s.panic[NEED_ENERGY] < 1.0f);

	/* A NULL threshold vector falls back to the module default vector, so
	 * the two entry points agree on the same input. Each step below is a
	 * single call from a full 1.0 panic, so the expectation is exact. */
	s = uniformSet(1.0f, 1.0f);
	needPanicDecay(&s, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f - NEED_PANIC_DECAY * NEED_MAX_DT,
				 s.panic[NEED_SURVIVAL]);

	s = uniformSet(1.0f, 1.0f);
	needPanicDecayRole(&s, NULL, NEED_MAX_DT);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f - NEED_PANIC_DECAY * NEED_MAX_DT,
				 s.panic[NEED_SURVIVAL]);

	needPanicDecayRole(NULL, threshold, NEED_MAX_DT);
	needPanicDecay(NULL, NEED_MAX_DT);
}

static void test_panic_decay_clamps_dt(void)
{
	NeedSet atMax = uniformSet(1.0f, 1.0f);
	NeedSet huge = uniformSet(1.0f, 1.0f);

	needPanicDecay(&atMax, NEED_MAX_DT);
	needPanicDecay(&huge, 100.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, atMax.panic[NEED_SURVIVAL],
				 huge.panic[NEED_SURVIVAL]);

	/* A 100 s stall cannot zero a 1.0 panic in one call. */
	TEST_ASSERT_TRUE(huge.panic[NEED_SURVIVAL] > 0.9f);
}

static void test_panic_decay_ignores_nonpositive_and_nan_dt(void)
{
	NeedSet s = uniformSet(1.0f, 1.0f);
	NeedSet nan = uniformSet(1.0f, 1.0f);
	float zero = 0.0f;

	needPanicDecay(&s, 0.0f);
	needPanicDecay(&s, -1.0f);
	needPanicDecay(&nan, zero / zero);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.panic[NEED_SURVIVAL]);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, nan.panic[NEED_SURVIVAL]);
}

void run_test_need(void);

void run_test_need(void)
{
	RUN_TEST(test_need_constants);
	RUN_TEST(test_need_set_init_is_content_and_calm);
	RUN_TEST(test_tier_weights_exact);
	RUN_TEST(test_tier_weights_fall_off_with_tier);
	RUN_TEST(test_band_endpoints);
	RUN_TEST(test_band_table_is_exact);
	RUN_TEST(test_band_deficit_is_clamped);
	RUN_TEST(test_band_null_and_unknown_slot);
	RUN_TEST(test_panic_mask);
	RUN_TEST(test_panic_mask_full_set);
	RUN_TEST(test_urgency_matches_band_when_calm);
	RUN_TEST(test_urgency_soft_falloff);
	RUN_TEST(test_urgency_tier_ordering);
	RUN_TEST(test_urgency_panicking_need_is_maximal);
	RUN_TEST(test_urgency_panic_suppresses_others);
	RUN_TEST(test_urgency_suppression_rounds_after_scaling);
	RUN_TEST(test_urgency_null_and_unknown_slot);
	RUN_TEST(test_urgency_is_deterministic);
	RUN_TEST(test_decay_dyadic_amount);
	RUN_TEST(test_decay_ramp_accelerates_near_empty);
	RUN_TEST(test_decay_ramp_formula);
	RUN_TEST(test_decay_clamps_dt);
	RUN_TEST(test_decay_ignores_nonpositive_and_nan_dt);
	RUN_TEST(test_decay_never_goes_negative);
	RUN_TEST(test_decay_zero_multiplier_freezes);
	RUN_TEST(test_decay_null_guards);
	RUN_TEST(test_decay_is_per_need);
	RUN_TEST(test_panic_decay_dyadic_amount);
	RUN_TEST(test_panic_rises_below_threshold);
	RUN_TEST(test_reserved_need_never_panics);
	RUN_TEST(test_panic_rise_clamps_dt_and_ignores_nan);
	RUN_TEST(test_panic_lifecycle_raise_then_bleed);
	RUN_TEST(test_panic_decay_floors_at_zero);
	RUN_TEST(test_panic_move_is_per_need);
	RUN_TEST(test_panic_decay_role_thresholds);
	RUN_TEST(test_panic_decay_clamps_dt);
	RUN_TEST(test_panic_decay_ignores_nonpositive_and_nan_dt);
}