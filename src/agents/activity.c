/*
 * The activity catalogue and the urgency fold (see agents/activity.h for the
 * model and the argument).
 *
 * One static table, one algorithm. The table is the spec's authorability
 * requirement made literal: a new activity is a row here and a comment
 * naming the need it serves, and no call site changes. jobUrgency is the
 * whole of the arithmetic — resolve the row, guard NULL with -1, take the
 * MAXIMUM of wantUrgency x serves over the served needs.
 *
 * Pure: no SDL, no allocation, no time source, no libm. Refusals are a
 * return value plus a stderr diagnostic (the convention entities.c,
 * mapsource.c and board.c all follow), so a caller that loses track of a row
 * hears about it instead of quietly carrying a zero.
 */

#include "agents/activity.h"

#include "agents/agent.h"

#include <stdio.h>
#include <string.h>

/* The return value that means "this job could not be scored", distinct from
 * every real band (0..NEED_BANDS-1). See the header note on unknown ids. */
#define JOB_URGENCY_ERROR (-1)

/* WHAT A PLACE-REQUIRED ACTIVITY IS LOOKING FOR. The row says only
 * ACT_NEEDS_PLACE; this says which of the three things the cached view must
 * offer. Kept beside the catalogue rather than in the row because the row
 * shape is fixed by the header, and because a place requirement is a fact
 * about the WORLD, not about the agent who might perform it. A new
 * place-requiring row must add a line here — the suite pins that every
 * ACT_NEEDS_PLACE row has a slot and every ACT_IN_PLACE row does not. */
typedef enum PlaceSlot {
	PLACE_NONE,
	PLACE_OTHER_AGENT,	/* somebody else within TALK_RADIUS tiles */
	PLACE_ARCADE,		/* an arcade fixture tile to stand at */
	PLACE_MACHINE		/* a machine to work on */
} PlaceSlot;

static const struct {
	int id;
	PlaceSlot slot;
} ACTIVITY_PLACES[] = {
	{ JOB_TALK, PLACE_OTHER_AGENT },
	{ JOB_PLAY_ARCADE, PLACE_ARCADE },
	{ JOB_REPAIR, PLACE_MACHINE },
};

/* THE CATALOGUE. One row per activity, one comment per row naming the need
 * it serves — the spec's "intent-revealing names", so the table reads like
 * the design. serves[] is in Need order (SURVIVAL, ENERGY, SOCIAL,
 * SATISFACTION, ESTEEM, FULFILMENT) and is written out in full: -1.0f means
 * "this activity does not serve that need at all", which is deliberately
 * NOT 0.0f, because a strength of zero would be a row that serves a need
 * with no effect and could never be told apart from one that ignores it.
 *
 * Durations are the activity's own runtime; cooldowns are how long THIS
 * agent may not pick it again afterwards. A zero cooldown means "as often as
 * you like", which is right for WANDER and only for WANDER.
 *
 * Rows are in the brief's order, not in id order, and nothing may depend on
 * that: every scan below breaks ties on the lowest id explicitly rather than
 * on position, so permuting this table cannot change a result.
 */
static const Activity ACTIVITIES[] = {
	/* ACT_WANDER — "nothing better to do", the fallback filler.
	 * Serves FULFILMENT weakly (0.2): a stroll relieves boredom a
	 * little. In place, so it is always available. */
	{
		.id = ACT_WANDER,
		.name = "WANDER",
		.flags = ACT_IN_PLACE,
		.duration = 3.0f,
		.cooldown = 0.0f,
		.serves = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.2f },
		.skillGate = -1,
		.skillMin = 0.0f,
	},
	/* ACT_TALK — a conversation. Serves SOCIAL strongly (0.9): company is
	 * the whole point of talking to somebody. Needs a PLACE: another
	 * agent within TALK_RADIUS tiles. */
	{
		.id = ACT_TALK,
		.name = "TALK",
		.flags = ACT_NEEDS_PLACE,
		.duration = 5.0f,
		.cooldown = 5.0f,
		.serves = { -1.0f, -1.0f, 0.9f, -1.0f, -1.0f, -1.0f },
		.skillGate = -1,
		.skillMin = 0.0f,
	},
	/* ACT_PLAY_ARCADE — a 20s session. Serves FULFILMENT strongly
	 * (0.9): leisure and curiosity, the tier-4 want. Needs a PLACE: an
	 * arcade fixture tile. THE ARCADE RULE — this row is reached by an
	 * errand that enqueues the job and equally by an idle agent picking
	 * it for itself; nothing in the row says which. */
	{
		.id = ACT_PLAY_ARCADE,
		.name = "PLAY_ARCADE",
		.flags = ACT_NEEDS_PLACE,
		.duration = 20.0f,
		.cooldown = 20.0f,
		.serves = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.9f },
		.skillGate = -1,
		.skillMin = 0.0f,
	},
	/* ACT_NAP — rest. Serves ENERGY strongly (0.9). In place. This is
	 * the row the spec's priority-inversion test turns on: 0.9 of a
	 * tier-1 need outranks 0.2 of a tier-4 one, because wantUrgency
	 * already carries the tier weight. */
	{
		.id = ACT_NAP,
		.name = "NAP",
		.flags = ACT_IN_PLACE,
		.duration = 15.0f,
		.cooldown = 10.0f,
		.serves = { -1.0f, 0.9f, -1.0f, -1.0f, -1.0f, -1.0f },
		.skillGate = -1,
		.skillMin = 0.0f,
	},
	/* ACT_TIDY — tidying a machine. Serves ESTEEM moderately (0.6):
	 * competence is what tidying demonstrates. Needs a PLACE: a
	 * machine. Gated on SKILL_REPAIR at 0 — a gate that opens for
	 * everyone today, kept because "skills modify, never disqualify"
	 * is a rule and a rule with no shipped row is a rule nobody has
	 * tested. */
	{
		.id = ACT_TIDY,
		.name = "TIDY",
		.flags = ACT_NEEDS_PLACE,
		.duration = 4.0f,
		.cooldown = 8.0f,
		.serves = { -1.0f, -1.0f, -1.0f, -1.0f, 0.6f, -1.0f },
		.skillGate = SKILL_REPAIR,
		.skillMin = 0.0f,
	},
	/* ACT_COFFEE — a break at the counter. Serves TWO needs: ENERGY
	 * (0.6, tiredness) and NEED_SATISFACTION (0.6, the frustration of
	 * being blocked), which is why this row exists at all — it is the
	 * only multi-need row, and the suite needs one to tell the MAX
	 * aggregation from the SUM: two broad-but-weak needs must lose to
	 * one narrow-but-critical one. Declared on the JOB_PAY kind slot,
	 * the one JobKind no other row claims. */
	{
		.id = ACT_COFFEE,
		.name = "COFFEE",
		.flags = ACT_IN_PLACE,
		.duration = 6.0f,
		.cooldown = 30.0f,
		.serves = { -1.0f, 0.6f, -1.0f, 0.6f, -1.0f, -1.0f },
		.skillGate = -1,
		.skillMin = 0.0f,
	},
};

/* Which cached place a row needs, or PLACE_NONE when it needs none. The
 * first matching row wins; the table is tiny and the ids are unique. */
static PlaceSlot activityPlaceSlot(int id)
{
	size_t i;

	for (i = 0; i < sizeof(ACTIVITY_PLACES) / sizeof(ACTIVITY_PLACES[0]);
	     i++) {
		if (ACTIVITY_PLACES[i].id == id)
			return ACTIVITY_PLACES[i].slot;
	}
	return PLACE_NONE;
}

const Activity *activitiesGet(int id)
{
	size_t i;

	/* The linear scan is the point: an unknown id is a MISS, reported as
	 * NULL, and never as "the nearest row" or a default. Two live agents
	 * asking for the same id get the same pointer every time. */
	for (i = 0; i < sizeof(ACTIVITIES) / sizeof(ACTIVITIES[0]); i++) {
		if (ACTIVITIES[i].id == id)
			return &ACTIVITIES[i];
	}
	return NULL;
}

int activitiesCount(void)
{
	return (int)(sizeof(ACTIVITIES) / sizeof(ACTIVITIES[0]));
}

const Activity *activitiesAt(int index)
{
	if (index < 0 || index >= activitiesCount())
		return NULL;
	return &ACTIVITIES[index];
}

bool activityPlaceSatisfied(const Activity *act, const ActivityPlaces *p)
{
	if (act == NULL)
		return false;
	/* IN PLACE is answered by the row's own flags, never by the world:
	 * this is the branch that keeps a critter busy with no fixtures,
	 * agents or machines anywhere in the level. */
	if (!(act->flags & ACT_NEEDS_PLACE))
		return true;
	if (p == NULL)
		return false;
	switch (activityPlaceSlot(act->id)) {
	case PLACE_OTHER_AGENT:
		return p->othersInRange > 0;
	case PLACE_ARCADE:
		return p->arcadeTile != JOB_INVALID;
	case PLACE_MACHINE:
		return p->machineId != JOB_INVALID;
	case PLACE_NONE:
	default:
		/* A row that asks for a place but names no way to satisfy it
		 * is a broken catalogue row. It reads unavailable rather than
		 * available: guessing a place would send an agent walking to
		 * one it was never told about. */
		return false;
	}
}

bool activitySkillOpen(const struct Agent *a, const Activity *act)
{
	if (act == NULL)
		return true;	/* nothing to gate */
	if (act->skillGate < 0)
		return true;	/* no gate declared */
	if (a == NULL)
		return true;	/* nothing to gate against */
	/* Skills MODIFY rather than disqualify (agents/agent.h), so this is
	 * a floor and not a qualification test: a row with skillMin 0 is
	 * open to everybody, which is the shipped state. */
	return (float)a->skills[act->skillGate] >= act->skillMin;
}

int jobUrgency(const struct Agent *a, const Job *j)
{
	const Activity *act;
	int best = 0;
	int n;

	if (a == NULL || j == NULL) {
		fprintf(stderr, "isomata: jobUrgency: no %s to score\n",
			(a == NULL) ? "agent" : "job");
		return JOB_URGENCY_ERROR;
	}
	act = activitiesGet(j->kind);
	if (act == NULL) {
		/* REVIEW FOCUS #5. An unknown activity is a BROKEN JOB, not a
		 * job nobody wants: the caller must log and fail it. Returning
		 * 0 here would be the worst available answer, because 0 is a
		 * real band meaning "satisfied agent, nothing pressing" and
		 * the job would then sit on a board looking merely boring. */
		fprintf(stderr,
			"isomata: jobUrgency: unknown activity id %d on job kind %d\n",
			j->kind, j->kind);
		return JOB_URGENCY_ERROR;
	}
	for (n = 0; n < NEED_COUNT; n++) {
		float v;
		int band;

		/* !(serves > 0.0f) rejects -1.0f (not served), 0.0f (serves
		 * nothing) and NaN in one comparison, so a broken row can
		 * never cast an undefined value to an int. */
		if (!(act->serves[n] > 0.0f))
			continue;
		/* wantUrgency is 0..NEED_BANDS-1 and carries the tier weight,
		 * so this product is where the ladder's structure reaches the
		 * job: 0.9 of an emptied tier-1 need beats 0.2 of an emptied
		 * tier-4 one without anything comparing the two activities. */
		v = (float)wantUrgency(&a->needs, n) * act->serves[n];
		/* band is round(v), and the product is ALREADY inside the band
		 * range: wantUrgency is 0..NEED_BANDS-1 and serves is 0..1 on
		 * every row (the suite pins that bound), so v cannot leave
		 * 0..NEED_BANDS-1 and no output clamp is needed — exactly the
		 * argument needBand makes, for the same reason. */
		band = (int)(v + 0.5f);
		/* THE MAXIMUM, NEVER THE SUM. Taking the best served need is
		 * what stops a broad activity from outranking a pressing one
		 * merely by covering more slots. */
		if (band > best)
			best = band;
	}
	return best;
}

bool activityBindJob(Job *j, int actId)
{
	const Activity *act = activitiesGet(actId);
	int n;

	if (j == NULL || act == NULL) {
		fprintf(stderr,
			"isomata: activityBindJob: unknown activity id %d\n",
			actId);
		return false;
	}
	jobInit(j, act->id);
	j->duration = act->duration;
	for (n = 0; n < NEED_COUNT; n++) {
		j->served[n] = (act->serves[n] > 0.0f)
				       ? (int)(act->serves[n] * ACTIVITY_SERVE_SCALE
						+ 0.5f)
				       : 0;
	}
	/* IN PLACE is bound explicitly, because "deliberately happens here"
	 * and "nobody ever set a target" are different facts and the tick
	 * must not confuse them. A needs-place row keeps JOB_INVALID: the
	 * caller owns the world and has not found its place yet. */
	if (!(act->flags & ACT_NEEDS_PLACE))
		jobSetTarget(j, JOB_TARGET_NONE, JOB_INVALID);
	/* band stays JOB_URGENCY_ERROR (dirty) — scoring needs an agent,
	 * and a producer that guessed a band would be hand-setting the very
	 * number this module exists to compute. */
	return true;
}

void activityCooldownsInit(ActivityCooldowns *cd)
{
	if (cd == NULL)
		return;
	/* 0.0f everywhere: "available since the beginning of time", which is
	 * the honest reading for a mind that has done nothing yet. */
	memset(cd, 0, sizeof(*cd));
}

bool activityMarkDone(ActivityCooldowns *cd, int id, float now)
{
	const Activity *act = activitiesGet(id);

	if (cd == NULL)
		return true;	/* no store to write: nothing is suppressed */
	if (act == NULL) {
		fprintf(stderr,
			"isomata: activityMarkDone: unknown activity id %d\n",
			id);
		return false;
	}
	cd->readyAt[id] = now + act->cooldown;
	return true;
}

bool activityReady(const ActivityCooldowns *cd, int id, float now)
{
	const Activity *act = activitiesGet(id);

	/* An unknown id is never available: there is no row whose cooldown
	 * could have elapsed. */
	if (act == NULL)
		return false;
	/* No cooldown record means nothing can be shown to be off cooldown,
	 * so the conservative answer is "not ready". A caller with no store
	 * (a test, a headless probe) initialises one. */
	if (cd == NULL)
		return false;
	/* Inclusive at the boundary: at exactly now + cooldown the activity
	 * is available again, so a zero cooldown never suppresses anything
	 * for even one tick. */
	return now >= cd->readyAt[id];
}

/* True when the board already holds live work of this kind. A terminal job
 * does not count: it is dead, and the caller retires it — a finished nap
 * must not stop the next one being queued. */
static bool activityQueuedOn(const JobBoard *b, int kind)
{
	int h;

	for (h = 0; h < BOARD_MAX; h++) {
		const Job *j;

		if (!b->alive[h])
			continue;
		j = &b->slots[h];
		if (j->kind != kind)
			continue;
		if (j->state == JOB_DONE || j->state == JOB_FAILED)
			continue;
		return true;
	}
	return false;
}

int agentBestFill(const struct Agent *a, const JobBoard *b, float now)
{
	int best = JOB_INVALID;
	int bestScore = JOB_URGENCY_ERROR;
	int i;

	if (a == NULL || b == NULL)
		return JOB_INVALID;
	for (i = 0; i < activitiesCount(); i++) {
		const Activity *act = activitiesAt(i);
		Job probe;
		int score;

		/* activitiesAt cannot return NULL inside this loop bound —
		 * the two come from the same table — so there is nothing to
		 * check here that the loop does not already guarantee. */
		if (!activityReady(&a->cooldown, act->id, now))
			continue;
		if (!activityPlaceSatisfied(act, &a->places))
			continue;
		if (!activitySkillOpen(a, act))
			continue;
		/* Not a duplicate of work already queued: an idle agent that
		 * enqueued thirty identical fills would fill its own board and
		 * crowd out its own errands. */
		if (activityQueuedOn(b, act->id))
			continue;
		/* Scored with the SAME arithmetic as every other job, so a fill
		 * competes on the identical terms an errand does. This is the
		 * point of the whole module: one function decides what is
		 * worth doing, whatever produced the job.
		 *
		 * A NEGATIVE SCORE IS FILTERED BY THE COMPARISON ITSELF, not
		 * by a guard: -1 is below every real band (0..NEED_BANDS-1)
		 * and `bestScore` starts at -1, so a strict `>` can never
		 * adopt one — it would have to beat -1, and it is -1. The
		 * error value is therefore excluded by construction. */
		jobInit(&probe, act->id);
		score = jobUrgency(a, &probe);
		/* Strict `>` PLUS an explicit lowest-id tie-break: the table is
		 * not in id order and nothing may depend on that, so permuting
		 * the rows cannot change which activity wins. */
		if (score > bestScore ||
		    (score == bestScore && act->id < best)) {
			best = act->id;
			bestScore = score;
		}
	}
	return best;
}