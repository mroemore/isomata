/*
 * The activity catalogue and the urgency fold (CTOL rung 1: unit + boundary).
 *
 * Covers: the catalogue itself (every row resolves, ids are JobKinds, names
 * are present and distinct, every row serves at least one need, durations
 * and cooldowns are sane, the place flag and the place requirement agree);
 * activitiesGet / activitiesAt / activitiesCount including every guard;
 * jobUrgency — THE priority inversion, THE max-not-sum aggregation, the
 * unknown-activity error, the panic channel, the band range and the guards;
 * activityBindJob; the place predicate with no world at all; the skill gate;
 * cooldowns (including the inclusive boundary and the per-agent isolation);
 * agentBestFill (urgency ordering, cooldown, place, the no-duplicate rule,
 * the lowest-id tie-break and run-to-run determinism); and the unknown-id
 * error path end to end.
 *
 * THE TWO ASSERTIONS THAT MATTER MOST, and why they are shaped the way they
 * are:
 *
 *   test_urgency_inverts_priority_across_tiers — the spec's central claim.
 *   A 0.9 slice of an emptied TIER-1 need (a nap) must outrank a 0.2 slice
 *   of an emptied TIER-4 need (a wander). Nothing compares the two
 *   activities; wantUrgency already carries the tier weight, so this goes
 *   red the moment serves[] is treated as a flat priority or the tier
 *   weight is dropped.
 *
 *   test_urgency_takes_the_max_not_the_sum — with ONLY single-need rows,
 *   max and sum agree and the assertion would pass under either arithmetic,
 *   which makes it worse than useless. ACT_COFFEE is the catalogue's one
 *   multi-need row and exists for this test: two broad-but-weak needs (5 and
 *   5) must lose to one narrow-but-critical need (8). Under the sum the
 *   coffee scores 11 and BEATS the nap, so the assertion is red.
 *
 * Pure: links activity.c + its dependencies and the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_activity().
 */

#include "unity.h"

#include "agents/activity.h"
#include "agents/agent.h"

#include <math.h>
#include <string.h>

/* ---- fixtures ---------------------------------------------------------- */

/* An agent with fresh, content, calm needs. */
static Agent agentWithContentNeeds(void)
{
	Agent a;

	memset(&a, 0, sizeof(a));
	needSetInit(&a.needs);
	a.panicMask = needPanicMask(&a.needs);
	a.engagedJob = JOB_INVALID;
	boardInit(&a.personal, false);
	activityCooldownsInit(&a.cooldown);
	a.places.othersInRange = 0;
	a.places.arcadeTile = JOB_INVALID;
	a.places.machineId = JOB_INVALID;
	return a;
}

/* A job of `kind`, unbound: jobUrgency reads only `kind`, so nothing else
 * has to be set for a scoring test. */
static Job jobOf(int kind)
{
	Job j;

	jobInit(&j, kind);
	return j;
}

/* An agent whose needs are all EMPTY and calm: every want band is at its
 * maximum, so scores read directly off the tier weight. */
static Agent agentWithEmptyNeeds(void)
{
	Agent a = agentWithContentNeeds();
	int n;

	for (n = 0; n < NEED_COUNT; n++)
		a.needs.value[n] = 0.0f;
	return a;
}

/* ---- the catalogue ----------------------------------------------------- */

static void test_catalogue_rows_are_well_formed(void)
{
	int i;

	/* Bounded: the fixed array the cooldown store is indexed by. */
	TEST_ASSERT_TRUE(activitiesCount() > 0);
	TEST_ASSERT_TRUE(activitiesCount() <= ACTIVITY_MAX);

	for (i = 0; i < activitiesCount(); i++) {
		const Activity *act = activitiesAt(i);
		int served = 0;
		int n;

		TEST_ASSERT_NOT_NULL(act);
		/* Intent-revealing names: a row is readable on its own. */
		TEST_ASSERT_NOT_NULL(act->name);
		TEST_ASSERT_TRUE(act->name[0] != '\0');
		/* The id IS a JobKind: jobUrgency resolves a live job back to
		 * its row through exactly this field. */
		TEST_ASSERT_TRUE(act->id >= 0);
		TEST_ASSERT_TRUE(act->id < JOB_KIND_COUNT);
		/* Non-negative runtime, non-negative cooldown. A negative
		 * duration would be a job that ends before it begins. */
		TEST_ASSERT_TRUE(act->duration > 0.0f);
		TEST_ASSERT_TRUE(act->cooldown >= 0.0f);
		/* Either flag or neither — the enum is a two-value shape, so a
		 * bit outside it is a typo rather than a new capability. */
		TEST_ASSERT_TRUE((act->flags & ~(ACT_IN_PLACE | ACT_NEEDS_PLACE))
				 == 0);
		/* A row that serves NO need is a broken row: it could never be
		 * urgent and would sit on a board forever. */
		for (n = 0; n < NEED_COUNT; n++) {
			TEST_ASSERT_TRUE(act->serves[n] >= -1.0f);
			TEST_ASSERT_TRUE(act->serves[n] <= 1.0f);
			if (act->serves[n] > 0.0f)
				served++;
		}
		TEST_ASSERT_TRUE(served >= 1);
		/* An in-place row is satisfied by the world offering NOTHING —
		 * it is answered by its own flags and never by the world, so
		 * even a NULL view leaves it available. A row that needs a
		 * place is mapped to a way of satisfying it, and a full view
		 * satisfies all three. */
		if (act->flags & ACT_IN_PLACE) {
			TEST_ASSERT_TRUE(activityPlaceSatisfied(act, NULL));
		} else {
			ActivityPlaces full = { 1, JOB_TILE_PACK(1, 1), 0 };

			TEST_ASSERT_TRUE(activityPlaceSatisfied(act, &full));
		}
	}
}

static void test_catalogue_ids_are_unique_and_resolvable(void)
{
	int i;
	int j;

	for (i = 0; i < activitiesCount(); i++) {
		const Activity *a = activitiesAt(i);

		TEST_ASSERT_EQUAL_PTR(a, activitiesGet(a->id));
		for (j = i + 1; j < activitiesCount(); j++) {
			const Activity *b = activitiesAt(j);

			TEST_ASSERT_TRUE(a->id != b->id);
			TEST_ASSERT_TRUE(strcmp(a->name, b->name) != 0);
		}
	}
}

static void test_activities_at_is_bounded(void)
{
	TEST_ASSERT_NULL(activitiesAt(-1));
	TEST_ASSERT_NULL(activitiesAt(activitiesCount()));
	TEST_ASSERT_NULL(activitiesAt(ACTIVITY_MAX));
	TEST_ASSERT_NULL(activitiesAt(9999));
	TEST_ASSERT_NOT_NULL(activitiesAt(0));
}

static void test_unknown_activity_id_is_an_error(void)
{
	/* THE REVIEW FOCUS #5 GUARD. A miss is NULL — never the nearest row,
	 * never a default, never a silent zero. */
	TEST_ASSERT_NULL(activitiesGet(9999));
	TEST_ASSERT_NULL(activitiesGet(-1));
	TEST_ASSERT_NULL(activitiesGet(JOB_KIND_COUNT));
}

/* ---- the priority inversion (the acceptance oracle) ------------------- */

/* THE priority-inversion property: a critical tier-1 continuation outranks a
 * severe tier-4 fill. Nothing compares the two activities — wantUrgency
 * already carries the Maslov-style tier weight (agents/need.h), so the
 * ladder's structure comes out of the arithmetic. */
static void test_urgency_inverts_priority_across_tiers(void)
{
	Agent a = agentWithContentNeeds();
	Job fill = jobOf(ACT_WANDER);	/* serves FULFILMENT 0.2 (tier 4) */
	Job errand = jobOf(ACT_NAP);	/* serves ENERGY 0.9 (tier 1) */

	a.needs.value[NEED_ENERGY] = 0.02f;	/* critical */
	a.needs.value[NEED_FULFILMENT] = 0.01f;	/* severe, but lower tier */

	/* The tiers in isolation, so a regression names itself. */
	TEST_ASSERT_EQUAL_INT(9, wantUrgency(&a.needs, NEED_ENERGY));
	TEST_ASSERT_EQUAL_INT(2, wantUrgency(&a.needs, NEED_FULFILMENT));
	TEST_ASSERT_EQUAL_INT(8, jobUrgency(&a, &errand));
	TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &fill));

	TEST_ASSERT_TRUE(jobUrgency(&a, &errand) > jobUrgency(&a, &fill));
}

/* The same rule, stated as a ratio: a STRONGLY served tier-4 want still
 * loses to a WEAKLY served tier-1 one. Serve fulfilment at full strength and
 * the inversion must survive it. */
static void test_a_strong_tier4_want_still_loses_to_a_weak_tier1(void)
{
	Agent a = agentWithContentNeeds();
	Job nap = jobOf(ACT_NAP);
	Job arcade = jobOf(ACT_PLAY_ARCADE);

	a.needs.value[NEED_ENERGY] = 0.0f;
	a.needs.value[NEED_FULFILMENT] = 0.0f;
	/* Both activities are at their strongest possible for their need. */
	TEST_ASSERT_EQUAL_INT(8, jobUrgency(&a, &nap));
	TEST_ASSERT_EQUAL_INT(2, jobUrgency(&a, &arcade));
	TEST_ASSERT_TRUE(jobUrgency(&a, &nap) > jobUrgency(&a, &arcade));
}

/* THE MAX, NOT THE SUM. Two broad-but-weak served needs must lose to one
 * narrow-but-critical need. ACT_COFFEE (ENERGY 0.6 + SATISFACTION 0.6)
 * scores 5 under the max and 11 under the sum, and 11 BEATS the nap's 8 —
 * so this assertion is red the moment the fold becomes a sum. */
static void test_urgency_takes_the_max_not_the_sum(void)
{
	Agent a = agentWithEmptyNeeds();
	Job coffee = jobOf(ACT_COFFEE);	/* serves TWO needs at 0.6 */
	Job nap = jobOf(ACT_NAP);	/* serves ONE need at 0.9 */

	TEST_ASSERT_EQUAL_INT(5, jobUrgency(&a, &coffee));
	TEST_ASSERT_EQUAL_INT(8, jobUrgency(&a, &nap));
	TEST_ASSERT_TRUE(jobUrgency(&a, &coffee) < jobUrgency(&a, &nap));

	/* And the fold is exactly the max over the served slots, not an
	 * approximation of it: recomputing the parts by hand from the same
	 * wantUrgency reproduces the total exactly. */
	{
		const Activity *row = activitiesGet(ACT_COFFEE);
		int best = 0;
		int n;

		for (n = 0; n < NEED_COUNT; n++) {
			int band;

			if (!(row->serves[n] > 0.0f))
				continue;
			band = (int)((float)wantUrgency(&a.needs, n) *
					     row->serves[n] +
				     0.5f);
			if (band > best)
				best = band;
		}
		TEST_ASSERT_EQUAL_INT(5, best);
		TEST_ASSERT_EQUAL_INT(best, jobUrgency(&a, &coffee));
	}
}

/* Panic is a MODE CHANGE and is read from the panic channel, never the
 * value channel: a panicking need is maximally urgent however full its
 * value looks. */
static void test_a_panicking_need_is_maximal_whatever_its_value(void)
{
	Agent a = agentWithContentNeeds();
	Job nap = jobOf(ACT_NAP);
	Job wander = jobOf(ACT_WANDER);

	/* Energy looks perfectly CONTENT (value 1.0) and still panics. */
	a.needs.panic[NEED_ENERGY] = 0.5f;
	TEST_ASSERT_EQUAL_INT(NEED_BANDS - 1, wantUrgency(&a.needs, NEED_ENERGY));
	/* The job lands one band BELOW the maximum, and that is the point:
	 * serves[] is a STRENGTH, so even an activity that serves the
	 * panicking need at its full 0.9 scores round(15 x 0.9) = 14. A
	 * panic raises the want to its ceiling; it does not exempt the job
	 * from the activity's own quality. */
	TEST_ASSERT_EQUAL_INT(14, jobUrgency(&a, &nap));
	/* And a job serving a need that is NOT panicking is untouched by it:
	 * the panic is read per need, not smeared across the vector. */
	TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &wander));
}

/* An unknown activity is an ERROR, not a silent zero: -1 sits outside the
 * 0..NEED_BANDS-1 band range so "broken" can never read as "boring". */
static void test_unknown_activity_is_an_error_not_a_zero(void)
{
	Agent a = agentWithContentNeeds();
	Job bogus = jobOf(9999);

	TEST_ASSERT_EQUAL_INT(-1, jobUrgency(&a, &bogus));
	TEST_ASSERT_NULL(activitiesGet(9999));
	/* Also for the guards: no agent or no job is unscorable, never 0. */
	TEST_ASSERT_EQUAL_INT(-1, jobUrgency(NULL, &bogus));
	TEST_ASSERT_EQUAL_INT(-1, jobUrgency(&a, NULL));
	TEST_ASSERT_EQUAL_INT(-1, jobUrgency(NULL, NULL));
	/* And a real activity on a content agent is a REAL zero, which is a
	 * different answer from -1. */
	{
		Job nap = jobOf(ACT_NAP);

		TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &nap));
	}
}

/* THE ARCADE RULE: one declaration, two producers. An errand's job and an
 * idle agent's self-chosen job are the same row, scored identically —
 * nothing in the score can know which producer made it. */
static void test_one_activity_two_producers(void)
{
	Agent a = agentWithEmptyNeeds();
	Job fromErrand = jobOf(JOB_PLAY_ARCADE);
	Job fromFill = jobOf(ACT_PLAY_ARCADE);

	TEST_ASSERT_EQUAL_PTR(activitiesGet(JOB_PLAY_ARCADE),
			      activitiesGet(ACT_PLAY_ARCADE));
	/* Same pointer every call: the catalogue is static storage. */
	TEST_ASSERT_EQUAL_PTR(activitiesGet(ACT_PLAY_ARCADE),
			      activitiesGet(ACT_PLAY_ARCADE));
	TEST_ASSERT_EQUAL_INT(jobUrgency(&a, &fromErrand),
			      jobUrgency(&a, &fromFill));
	/* Binding either producer's job produces the same declaration. */
	TEST_ASSERT_TRUE(activityBindJob(&fromErrand, ACT_PLAY_ARCADE));
	TEST_ASSERT_TRUE(activityBindJob(&fromFill, ACT_PLAY_ARCADE));
	TEST_ASSERT_EQUAL_INT(fromErrand.duration, fromFill.duration);
	TEST_ASSERT_EQUAL_INT(jobUrgency(&a, &fromErrand),
			      jobUrgency(&a, &fromFill));
}

/* ---- places ------------------------------------------------------------ */

/* An in-place activity needs no fixture at all: it is available with the
 * world offering NOTHING. */
static void test_an_in_place_activity_never_needs_a_fixture(void)
{
	ActivityPlaces none = { 0, JOB_INVALID, JOB_INVALID };
	const Activity *nap = activitiesGet(ACT_NAP);
	const Activity *wander = activitiesGet(ACT_WANDER);
	const Activity *coffee = activitiesGet(ACT_COFFEE);

	TEST_ASSERT_NOT_NULL(nap);
	TEST_ASSERT_TRUE(activityPlaceSatisfied(nap, &none));
	TEST_ASSERT_TRUE(activityPlaceSatisfied(wander, &none));
	TEST_ASSERT_TRUE(activityPlaceSatisfied(coffee, &none));
	/* Not even a NULL world view. */
	TEST_ASSERT_TRUE(activityPlaceSatisfied(nap, NULL));
	/* A NULL row satisfies nothing — there is nothing to satisfy. */
	TEST_ASSERT_FALSE(activityPlaceSatisfied(NULL, &none));
}

static void test_talk_needs_another_agent_in_range(void)
{
	ActivityPlaces p = { 0, JOB_INVALID, JOB_INVALID };
	const Activity *talk = activitiesGet(ACT_TALK);

	TEST_ASSERT_NOT_NULL(talk);
	TEST_ASSERT_EQUAL_INT(ACT_NEEDS_PLACE, talk->flags);
	/* Nobody about: unavailable. */
	TEST_ASSERT_FALSE(activityPlaceSatisfied(talk, &p));
	/* Somebody about: available. */
	p.othersInRange = 1;
	TEST_ASSERT_TRUE(activityPlaceSatisfied(talk, &p));
	/* A missing world view is not an available one. */
	TEST_ASSERT_FALSE(activityPlaceSatisfied(talk, NULL));
	TEST_ASSERT_EQUAL_INT(3, TALK_RADIUS);
}

static void test_place_requiring_activities_are_unavailable_by_default(void)
{
	ActivityPlaces none = { 0, JOB_INVALID, JOB_INVALID };
	const Activity *arcade = activitiesGet(ACT_PLAY_ARCADE);
	const Activity *tidy = activitiesGet(ACT_TIDY);

	TEST_ASSERT_NOT_NULL(arcade);
	TEST_ASSERT_NOT_NULL(tidy);
	TEST_ASSERT_FALSE(activityPlaceSatisfied(arcade, &none));
	TEST_ASSERT_FALSE(activityPlaceSatisfied(tidy, &none));
	/* The world supplying them makes them available. */
	none.arcadeTile = JOB_TILE_PACK(3, 4);
	TEST_ASSERT_TRUE(activityPlaceSatisfied(arcade, &none));
	none.machineId = 2;
	TEST_ASSERT_TRUE(activityPlaceSatisfied(tidy, &none));
}

/* ---- the skill gate ---------------------------------------------------- */

static void test_the_skill_gate_is_a_floor_not_a_qualification(void)
{
	Agent a = agentWithEmptyNeeds();
	const Activity *tidy = activitiesGet(ACT_TIDY);

	TEST_ASSERT_NOT_NULL(tidy);
	TEST_ASSERT_EQUAL_INT(SKILL_REPAIR, tidy->skillGate);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, tidy->skillMin);
	/* Open to everybody as shipped: a bad hire must not deadlock. */
	TEST_ASSERT_TRUE(activitySkillOpen(&a, tidy));
	/* A row with no gate, and no agent to gate against. */
	TEST_ASSERT_TRUE(activitySkillOpen(&a, activitiesGet(ACT_NAP)));
	TEST_ASSERT_TRUE(activitySkillOpen(&a, NULL));
	/* A raised floor closes the gate for an agent below it. */
	{
		Activity gated = *tidy;

		gated.skillMin = 5.0f;
		TEST_ASSERT_FALSE(activitySkillOpen(&a, &gated));
		a.skills[SKILL_REPAIR] = 5;
		TEST_ASSERT_TRUE(activitySkillOpen(&a, &gated));
		/* No agent to gate against is open, not closed: a missing
		 * reader must never be read as a refusal. */
		TEST_ASSERT_TRUE(activitySkillOpen(NULL, &gated));
	}
}

/* A row that asks for a place but names no way to satisfy one is a BROKEN
 * row, and it reads unavailable rather than available: guessing a place would
 * send an agent walking somewhere it was never told about. */
static void test_a_place_row_with_no_way_to_satisfy_it_is_unavailable(void)
{
	ActivityPlaces places = { .othersInRange = 0, .arcadeTile = JOB_INVALID,
				  .machineId = JOB_INVALID };
	Activity gated = *activitiesGet(ACT_NAP);

	gated.id = 4242;		/* ACT_NEEDS_PLACE, no slot mapped */
	gated.flags = ACT_NEEDS_PLACE;
	TEST_ASSERT_FALSE(activityPlaceSatisfied(&gated, &places));
}

/* ---- binding a job ----------------------------------------------------- */

static void test_bind_job_stamps_the_declaration(void)
{
	Job j;
	const Activity *nap = activitiesGet(ACT_NAP);

	TEST_ASSERT_TRUE(activityBindJob(&j, ACT_NAP));
	TEST_ASSERT_EQUAL_INT(ACT_NAP, j.kind);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, nap->duration, j.duration);
	/* The served strengths, in percent; 0 means "does not serve". */
	TEST_ASSERT_EQUAL_INT(90, j.served[NEED_ENERGY]);
	TEST_ASSERT_EQUAL_INT(0, j.served[NEED_FULFILMENT]);
	TEST_ASSERT_EQUAL_INT(0, j.served[NEED_SURVIVAL]);
	/* An in-place row binds JOB_TARGET_NONE EXPLICITLY: "deliberately
	 * here" and "nobody set a target" are different facts. */
	TEST_ASSERT_EQUAL_INT(JOB_TARGET_NONE, j.targetKind);
	/* The band is left DIRTY: a producer that guessed one would be hand-
	 * setting the number this module exists to compute. */
	TEST_ASSERT_EQUAL_INT(-1, j.band);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.claimer);
}

static void test_bind_job_refuses_a_needs_place_row_without_a_target(void)
{
	Job j;

	TEST_ASSERT_TRUE(activityBindJob(&j, ACT_TALK));
	/* Needs a place the caller has not found yet: bound as INVALID, so
	 * an unbound job fails loudly rather than walking to the origin. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetKind);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetId);
	TEST_ASSERT_EQUAL_INT(90, j.served[NEED_SOCIAL]);
	TEST_ASSERT_EQUAL_INT(0, j.served[NEED_ENERGY]);
}

static void test_bind_job_guards(void)
{
	Job j;

	TEST_ASSERT_EQUAL_INT(-1, jobOf(ACT_NAP).band);
	TEST_ASSERT_FALSE(activityBindJob(NULL, ACT_NAP));
	TEST_ASSERT_FALSE(activityBindJob(&j, 9999));
	TEST_ASSERT_FALSE(activityBindJob(&j, -1));
}

/* ---- cooldowns --------------------------------------------------------- */

static void test_cooldowns_suppress_a_repick_until_they_expire(void)
{
	ActivityCooldowns cd;
	const Activity *nap = activitiesGet(ACT_NAP);

	activityCooldownsInit(&cd);
	TEST_ASSERT_TRUE(activityReady(&cd, ACT_NAP, 0.0f));
	TEST_ASSERT_TRUE(activityMarkDone(&cd, ACT_NAP, 0.0f));
	/* Off cooldown the moment it was done... */
	TEST_ASSERT_FALSE(activityReady(&cd, ACT_NAP, 0.0f));
	/* ...and back at exactly now + cooldown, the boundary INCLUSIVE, so
	 * a zero cooldown never suppresses even one tick. */
	TEST_ASSERT_FALSE(activityReady(&cd, ACT_NAP, nap->cooldown - 0.01f));
	TEST_ASSERT_TRUE(activityReady(&cd, ACT_NAP, nap->cooldown));
	TEST_ASSERT_TRUE(activityReady(&cd, ACT_NAP, nap->cooldown + 10.0f));
	/* Another activity is untouched. */
	TEST_ASSERT_TRUE(activityReady(&cd, ACT_WANDER, 0.0f));
}

static void test_cooldown_guards(void)
{
	ActivityCooldowns cd;

	activityCooldownsInit(&cd);
	/* NULL initialise is a no-op, not a crash. */
	activityCooldownsInit(NULL);
	/* An unknown id is never available and is never stamped. */
	TEST_ASSERT_FALSE(activityReady(&cd, 9999, 0.0f));
	TEST_ASSERT_FALSE(activityMarkDone(&cd, 9999, 0.0f));
	TEST_ASSERT_FALSE(activityMarkDone(&cd, -1, 0.0f));
	/* No store: nothing can be shown to be off cooldown. */
	TEST_ASSERT_FALSE(activityReady(NULL, ACT_NAP, 0.0f));
	TEST_ASSERT_TRUE(activityMarkDone(NULL, ACT_NAP, 0.0f));
}

/* Cooldowns are PER AGENT: one mind's nap must never suppress another's. */
static void test_cooldowns_are_per_agent(void)
{
	ActivityCooldowns mine;
	ActivityCooldowns theirs;

	activityCooldownsInit(&mine);
	activityCooldownsInit(&theirs);
	TEST_ASSERT_TRUE(activityMarkDone(&mine, ACT_NAP, 0.0f));
	TEST_ASSERT_FALSE(activityReady(&mine, ACT_NAP, 0.0f));
	TEST_ASSERT_TRUE(activityReady(&theirs, ACT_NAP, 0.0f));
}

/* ---- the fill pick ----------------------------------------------------- */

static void test_best_fill_takes_the_most_urgent_available_activity(void)
{
	Agent a = agentWithEmptyNeeds();
	JobBoard *b = &a.personal;

	/* ENERGY empty: the nap (8) beats the coffee (5) and the wander (0). */
	a.needs.value[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 0.0f));

	/* Energy satisfied, fulfilment empty: only the wander is worth
	 * anything, because the arcade needs a fixture nobody has. */
	needSetInit(&a.needs);
	a.needs.value[NEED_FULFILMENT] = 0.0f;
	TEST_ASSERT_EQUAL_INT(ACT_WANDER, agentBestFill(&a, b, 0.0f));

	/* An unplaceable activity is a filtered CANDIDATE, never a failed
	 * job: give it a fixture and the same pick changes. */
	a.places.arcadeTile = JOB_TILE_PACK(2, 2);
	TEST_ASSERT_EQUAL_INT(ACT_PLAY_ARCADE, agentBestFill(&a, b, 0.0f));
}

static void test_best_fill_respects_cooldowns(void)
{
	Agent a = agentWithEmptyNeeds();
	JobBoard *b = &a.personal;

	a.needs.value[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 0.0f));
	/* Having just napped, the nap is off the menu and the next best
	 * available fill is the coffee break. */
	TEST_ASSERT_TRUE(activityMarkDone(&a.cooldown, ACT_NAP, 0.0f));
	TEST_ASSERT_EQUAL_INT(ACT_COFFEE, agentBestFill(&a, b, 0.0f));
	/* And it comes back when the cooldown expires. */
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 30.0f));
}

static void test_best_fill_respects_places(void)
{
	/* ONLY social is empty, so the talk is the one thing this agent
	 * wants: every other fill scores 0 and cannot win it. */
	Agent a = agentWithContentNeeds();
	JobBoard *b = &a.personal;
	ActivityPlaces none = { 0, JOB_INVALID, JOB_INVALID };

	a.needs.value[NEED_SOCIAL] = 0.0f;
	/* Nobody about: the talk is unavailable and the in-place rows tie
	 * at zero, so the lowest id takes it. */
	TEST_ASSERT_EQUAL_INT(ACT_WANDER, agentBestFill(&a, b, 0.0f));
	/* Somebody in range, and the talk becomes the fill. */
	a.places.othersInRange = 1;
	TEST_ASSERT_EQUAL_INT(ACT_TALK, agentBestFill(&a, b, 0.0f));
	/* A NULL world view leaves only the in-place rows. */
	a.places = none;
	TEST_ASSERT_EQUAL_INT(ACT_WANDER, agentBestFill(&a, b, 0.0f));
}

static void test_best_fill_never_duplicates_queued_work(void)
{
	Agent a = agentWithEmptyNeeds();
	JobBoard *b = &a.personal;
	Job j;

	a.needs.value[NEED_ENERGY] = 0.0f;
	TEST_ASSERT_TRUE(activityBindJob(&j, ACT_NAP));
	TEST_ASSERT_EQUAL_INT(0, boardEnqueue(b, &j, JOB_PRIO_FILL));
	/* The nap is already queued, so the fill pick moves on rather than
	 * filling the agent's own board with identical napping. */
	TEST_ASSERT_EQUAL_INT(ACT_COFFEE, agentBestFill(&a, b, 0.0f));
	/* A TERMINAL job is dead work and does not block a fresh pick — and
	 * this is the shape that reaches the terminal check in the scan, as
	 * opposed to the dead-slot check a RETIRED row would reach. */
	boardRetire(b, 0);
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 0.0f));
	{
		Job done;

		TEST_ASSERT_TRUE(activityBindJob(&done, ACT_NAP));
		/* Driven to DONE through the lifecycle rather than poked: an
		 * illegal pair is a no-op, so PHASE_DONE on an UNCLAIMED job
		 * would leave it exactly that and prove nothing. */
		jobStep(&done, JOB_START);
		jobStep(&done, JOB_ARRIVED);	/* CLAIMED     -> WALKING */
		jobStep(&done, JOB_ARRIVED);	/* WALKING     -> INTERACTING */
		jobStep(&done, JOB_PHASE_DONE);	/* INTERACTING -> WAITING */
		jobStep(&done, JOB_PHASE_DONE);	/* WAITING     -> DONE */
		TEST_ASSERT_EQUAL_INT(JOB_DONE, done.state);
		/* Slot 0 was retired above, so the lowest free slot is 0 again. */
	TEST_ASSERT_EQUAL_INT(0, boardEnqueue(b, &done, JOB_PRIO_FILL));
	}
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 0.0f));

	/* The same for a FAILED job: a nap that was refused is dead work,
	 * exactly as a finished one is, and neither suppresses a fresh pick. */
	{
		Job dead;

		TEST_ASSERT_TRUE(activityBindJob(&dead, ACT_NAP));
		jobStep(&dead, JOB_DENIED);	/* UNCLAIMED -> FAILED */
		TEST_ASSERT_EQUAL_INT(JOB_FAILED, dead.state);
		TEST_ASSERT_TRUE(boardRetire(b, 0));
		TEST_ASSERT_EQUAL_INT(0, boardEnqueue(b, &dead, JOB_PRIO_FILL));
	}
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 0.0f));
}

static void test_best_fill_breaks_a_tie_on_the_lowest_id(void)
{
	Agent a = agentWithContentNeeds();
	JobBoard *b = &a.personal;

	/* A content agent wants nothing, so every in-place fill scores 0 and
	 * the winner is decided by the tie-break alone. The table is NOT in
	 * id order (the coffee row sits last but declares the lowest id after
	 * the wander), so this is exactly the case where an implementation
	 * that leaned on row order would differ. */
	TEST_ASSERT_EQUAL_INT(ACT_WANDER, agentBestFill(&a, b, 0.0f));
	/* Every available candidate is genuinely tied at zero. */
	TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &(Job){ .kind = ACT_NAP }));
	TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &(Job){ .kind = ACT_COFFEE }));
	TEST_ASSERT_EQUAL_INT(0, jobUrgency(&a, &(Job){ .kind = ACT_WANDER }));
}

static void test_best_fill_is_deterministic(void)
{
	/* A whole ladder of partly-empty needs, so several rows score above
	 * zero and the tie-break is not what is being measured. */
	Agent a = agentWithEmptyNeeds();
	JobBoard *b = &a.personal;
	int i;

	a.needs.value[NEED_ENERGY] = 0.1f;
	a.needs.value[NEED_SOCIAL] = 0.3f;
	a.needs.value[NEED_FULFILMENT] = 0.7f;
	a.needs.value[NEED_ESTEEM] = 0.2f;
	a.places.othersInRange = 1;
	a.places.machineId = 3;
	a.places.arcadeTile = JOB_TILE_PACK(5, 5);
	/* The nap is the most urgent thing available (band 7) and is put on
	 * cooldown at t=0 for its full 10s. */
	activityMarkDone(&a.cooldown, ACT_NAP, 0.0f);

	/* Same input, same answer, every time. */
	for (i = 0; i < 64; i++)
		TEST_ASSERT_EQUAL_INT(agentBestFill(&a, b, 6.0f),
				      agentBestFill(&a, b, 6.0f));
	/* At t=6 the nap is still cooling, so the coffee break (band 5) is
	 * the best available fill — the clock moves the answer ONLY through
	 * the cooldown it was told about. */
	TEST_ASSERT_EQUAL_INT(ACT_COFFEE, agentBestFill(&a, b, 6.0f));
	TEST_ASSERT_EQUAL_INT(ACT_COFFEE, agentBestFill(&a, b, 9.9f));
	/* Once the cooldown expires the more urgent nap wins it back: the
	 * cooldown suppresses a re-pick, it does not demote the row. */
	TEST_ASSERT_EQUAL_INT(ACT_NAP, agentBestFill(&a, b, 10.0f));
}

static void test_best_fill_guards(void)
{
	Agent a = agentWithEmptyNeeds();

	TEST_ASSERT_EQUAL_INT(JOB_INVALID, agentBestFill(NULL, &a.personal, 0.0f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, agentBestFill(&a, NULL, 0.0f));
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, agentBestFill(NULL, NULL, 0.0f));
}

/* ---- the whole loop, once ---------------------------------------------- */

/* The fold is bounded by the band range whatever the needs do, so it can
 * never hand a board an out-of-range urgency that the preemption margin
 * would then compare against. */
static void test_urgency_stays_inside_the_band_range(void)
{
	Agent a = agentWithEmptyNeeds();
	int i;

	for (i = 0; i < NEED_COUNT; i++)
		a.needs.panic[i] = 1.0f;	/* every need panicking at once */

	for (i = 0; i < activitiesCount(); i++) {
		const Activity *act = activitiesAt(i);
		Job j = jobOf(act->id);
		int band = jobUrgency(&a, &j);

		TEST_ASSERT_TRUE(band >= 0);
		TEST_ASSERT_TRUE(band <= NEED_BANDS - 1);
	}
}

void run_test_activity(void);

void run_test_activity(void)
{
	RUN_TEST(test_catalogue_rows_are_well_formed);
	RUN_TEST(test_catalogue_ids_are_unique_and_resolvable);
	RUN_TEST(test_activities_at_is_bounded);
	RUN_TEST(test_unknown_activity_id_is_an_error);
	RUN_TEST(test_urgency_inverts_priority_across_tiers);
	RUN_TEST(test_a_strong_tier4_want_still_loses_to_a_weak_tier1);
	RUN_TEST(test_urgency_takes_the_max_not_the_sum);
	RUN_TEST(test_a_panicking_need_is_maximal_whatever_its_value);
	RUN_TEST(test_unknown_activity_is_an_error_not_a_zero);
	RUN_TEST(test_one_activity_two_producers);
	RUN_TEST(test_an_in_place_activity_never_needs_a_fixture);
	RUN_TEST(test_talk_needs_another_agent_in_range);
	RUN_TEST(test_place_requiring_activities_are_unavailable_by_default);
	RUN_TEST(test_a_place_row_with_no_way_to_satisfy_it_is_unavailable);
	RUN_TEST(test_the_skill_gate_is_a_floor_not_a_qualification);
	RUN_TEST(test_bind_job_stamps_the_declaration);
	RUN_TEST(test_bind_job_refuses_a_needs_place_row_without_a_target);
	RUN_TEST(test_bind_job_guards);
	RUN_TEST(test_cooldowns_suppress_a_repick_until_they_expire);
	RUN_TEST(test_cooldown_guards);
	RUN_TEST(test_cooldowns_are_per_agent);
	RUN_TEST(test_best_fill_takes_the_most_urgent_available_activity);
	RUN_TEST(test_best_fill_respects_cooldowns);
	RUN_TEST(test_best_fill_respects_places);
	RUN_TEST(test_best_fill_never_duplicates_queued_work);
	RUN_TEST(test_best_fill_breaks_a_tie_on_the_lowest_id);
	RUN_TEST(test_best_fill_is_deterministic);
	RUN_TEST(test_best_fill_guards);
	RUN_TEST(test_urgency_stays_inside_the_band_range);
}