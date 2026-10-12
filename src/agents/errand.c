/*
 * Errand templates, adoption, and one step of an errand (see agents/errand.h
 * for the model and the argument).
 *
 * One static table and one advance function. The table is the spec's
 * authorability requirement made literal: a new goal is a row plus an
 * ErrandId, and no call site changes. errandAdvance applies exactly ONE thing
 * per call — bind, enqueue, advance or re-select — so the caller can
 * interleave the world's effects (which machine, how much money) with the
 * framework's own tick, and can never be half-way through two steps at once.
 *
 * Pure: no SDL, no allocation, no time source, no libm. Refusals are a
 * return value plus a stderr diagnostic (the convention entities.c,
 * boards.c and activity.c all follow), because a caller that loses track of a
 * row must hear about it rather than queue nothing.
 */

#include "agents/errand.h"

#include "agents/activity.h"
#include "agents/agent.h"

/* sim/machines.h for the MACHINE_KIND_* values a template's step payload
 * names. It is the one sim include this module takes, and it is taken in the
 * .c and not the header: machines.h is itself dependency-free (no entities, no
 * SDL, no clock), and the errand HEADER stays free of any sim include, which
 * is what lets agent.h — and therefore the whole framework — carry errands
 * without knowing what a machine is. The constant is data either way. */
#include "sim/machines.h"

#include <stdio.h>

/*
 * THE TEMPLATE TABLE. One row per goal this build ships, one comment per row
 * naming what it is for. `serves[]` is in Need order (SURVIVAL, ENERGY,
 * SOCIAL, SATISFACTION, ESTEEM, FULFILMENT) and is the STRENGTH with which
 * the goal relieves each need — 0.0f means "this errand is not about that
 * need", which is different from the activity catalogue's -1.0f sentinel
 * because here there is nothing to confuse it with (a template is about what
 * it serves).
 *
 * priorityFloor is a board PROVENANCE class, never an order (agents/board.h) —
 * and TODAY the board validates it and discards it, so it governs nothing
 * here either (see the header note "WHAT priorityFloor DOES TODAY"). A
 * world-seeded errand competes on the ladder and can lose to the agent's own
 * errand work because adoption writes no need and no band, not because of its
 * floor: that is "adopted at that tier and then wins or loses like everything
 * else, it is never forced".
 */
static const ErrandTemplate ERRAND_TEMPLATES[] = {
	/* ERR_LAUNDRY_CLEAN — the customer's whole reason for being here, and
	 * the T23 BrainPhase switch written as data. Three steps per machine,
	 * twice, then leave:
	 *
	 *   USE_MACHINE(WASHER) — walk to the free washer this policy likes,
	 *     claim it on arrival, pay the fee, load, and start the run.
	 *   GATE(WAIT_MACHINE)  — the bounded wait for the machine's
	 *     PHASE_DONE wake. The stand-by job this parks is the producer for
	 *     the job lifecycle's JOB_TIMEOUT cell: a machine that never
	 *     delivers its wake fails this step instead of hanging it.
	 *   COLLECT(WASHER)     — walk back, take the load out, release the
	 *     machine. The load's own state says this collected a WASH, which
	 *     is what turns DIRTY into WET_CLEAN.
	 *   USE_MACHINE(DRYER)  / GATE / COLLECT(DRYER) — the same three for
	 *     the dryer; that collection turns WET_CLEAN into DRY_CLEAN.
	 *   LEAVE               — walk to the door and go.
	 *
	 * Serves NEED_SATISFACTION strongly (0.9): "get this done" is the
	 * customer's own want, and it is the same channel being blocked time
	 * decays, so a step that completes relieves the very thing that made
	 * the wait annoying. ESTEEM at 0.6 as well — a load delivered is a
	 * promise kept. Nothing here serves survival: waiting for a machine is
	 * an inconvenience, not an emergency, and the tier assignment is the
	 * spec's. */
	{
		.id = ERR_LAUNDRY_CLEAN,
		.name = "LAUNDRY_CLEAN",
		.role = ROLE_CUSTOMER,
		.serves = { 0.0f, 0.0f, 0.0f, 0.9f, 0.6f, 0.0f },
		.steps = {
			{ STEP_JOB, JOB_USE_MACHINE, ERRAND_INVALID,
			  MACHINE_KIND_WASHER },
			{ STEP_GATE, JOB_INVALID, STEP_GATE_WAIT_MACHINE,
			  MACHINE_KIND_WASHER },
			{ STEP_JOB, JOB_COLLECT, ERRAND_INVALID,
			  MACHINE_KIND_WASHER },
			{ STEP_JOB, JOB_USE_MACHINE, ERRAND_INVALID,
			  MACHINE_KIND_DRYER },
			{ STEP_GATE, JOB_INVALID, STEP_GATE_WAIT_MACHINE,
			  MACHINE_KIND_DRYER },
			{ STEP_JOB, JOB_COLLECT, ERRAND_INVALID,
			  MACHINE_KIND_DRYER },
			{ STEP_JOB, JOB_LEAVE, ERRAND_INVALID,
			  ERRAND_TARGET_NONE },
		},
		.stepCount = 7,
		.priorityFloor = JOB_PRIO_ERRAND,
		.worldSeeded = false,
	},
	/* ERR_FIX_MACHINE — a world-seeded errand: the world hands it to
	 * somebody (a staffer) rather than the agent deciding for itself. One
	 * step, and it is the catalogue's own TIDY activity (ACT_TIDY is
	 * declared on the JOB_REPAIR kind) bound to the faulty machine — so the
	 * repair is a job like any other and competes on the same band.
	 *
	 * The floor is JOB_PRIO_ASSIGNED: work off the shared board, which is
	 * what a world event is offering, and one provenance step above an
	 * agent's own errand. It is NOT a boost: the jobs it enqueues are
	 * scored by the ladder like every other job, so a fault repair loses to
	 * an urgent wash and wins over an arcade trip, exactly as the spec's
	 * "a tier-3 complaint does not beat an urgent wash" requires.
	 *
	 * Serves ESTEEM (0.7): competence is what fixing a machine
	 * demonstrates. */
	{
		.id = ERR_FIX_MACHINE,
		.name = "FIX_MACHINE",
		.role = ROLE_STAFF,
		.serves = { 0.0f, 0.0f, 0.0f, 0.0f, 0.7f, 0.0f },
		.steps = {
			{ STEP_JOB, JOB_REPAIR, ERRAND_INVALID,
			  ERRAND_TARGET_NONE },
		},
		.stepCount = 1,
		.priorityFloor = JOB_PRIO_ASSIGNED,
		.worldSeeded = true,
	},
};

/* The bound of the table, spelled once so both lookups and every scan below
 * agree with it. */
#define ERRAND_TABLE_SIZE ((int)(sizeof(ERRAND_TEMPLATES) / \
				 sizeof(ERRAND_TEMPLATES[0])))

/* Is `t` declared for `role`? The role gate, as one predicate so adoption and
 * the need scan cannot disagree about it. */
static bool errandRoleMatches(const ErrandTemplate *t, int role)
{
	return t != NULL && t->role == role;
}

/* The step this errand is on, or NULL when the index is outside the template
 * (a corrupt record, or an errand whose id no row claims any more). */
static const ErrandStep *errandStepAt(const Errand *e)
{
	const ErrandTemplate *t;

	if (e == NULL)
		return NULL;
	t = errandsTemplate(e->tmplId);

	if (t == NULL || e->step < 0 || e->step >= t->stepCount)
		return NULL;
	return &t->steps[e->step];
}

/* Add `id` to this errand's blacklist (idempotent, bounded). The blacklist is
 * per INSTANCE: a fresh adoption starts clean, so a machine that was broken
 * when one errand started is a candidate for the next one. */
static void errandExclude(Errand *e, int id)
{
	int i;

	if (e == NULL || id < 0)
		return;
	for (i = 0; i < e->blacklistCount; i++)
		if (e->blacklist[i] == id)
			return;
	if (e->blacklistCount < ERRAND_MAX_FAILED)
		e->blacklist[e->blacklistCount++] = id;
}

/* Zero one errand record to "holds nothing". Adoption writes this before it
 * writes anything else, so a reused slot can never show the previous
 * occupant's template, step or blacklist. */
static void errandBlank(Errand *e)
{
	*e = (Errand){ 0 };
	e->tmplId = ERRAND_INVALID;
	e->step = 0;
	e->state = ERRAND_DONE;
	e->targetKind = ERRAND_TARGET_NONE;
	e->targetId = ERRAND_INVALID;
	e->targetTile = ERRAND_INVALID;
	e->jobHandle = ERRAND_INVALID;
	e->retryStep = ERRAND_INVALID;
	e->garment = ERRAND_INVALID;
	e->gateSecs = ERRAND_INVALID;
	e->stepBand = 0;
	e->priorityFloor = JOB_PRIO_ERRAND;
}

void errandListInit(ErrandList *l)
{
	int i;

	if (l == NULL)
		return;
	for (i = 0; i < ERRAND_MAX; i++)
		errandBlank(&l->items[i]);
	l->count = 0;
}

const ErrandTemplate *errandsTemplate(int id)
{
	int i;

	/* The linear scan is the point: an unknown id is a MISS, reported as
	 * NULL and never as "the nearest row" or a default (Review Focus #5).
	 * Two agents asking for the same id get the same pointer every time. */
	for (i = 0; i < ERRAND_TABLE_SIZE; i++)
		if (ERRAND_TEMPLATES[i].id == id)
			return &ERRAND_TEMPLATES[i];
	fprintf(stderr,
		"isomata: errandsTemplate: unknown errand template id %d\n", id);
	return NULL;
}

int errandsTemplateCount(void)
{
	return ERRAND_TABLE_SIZE;
}

const ErrandTemplate *errandsTemplateAt(int index)
{
	if (index < 0 || index >= ERRAND_TABLE_SIZE)
		return NULL;
	return &ERRAND_TEMPLATES[index];
}

Errand *errandActive(Agent *a)
{
	int i;

	if (a == NULL)
		return NULL;
	for (i = 0; i < a->errands.count; i++)
		if (a->errands.items[i].state == ERRAND_ACTIVE)
			return &a->errands.items[i];
	return NULL;
}

Errand *errandFind(Agent *a, int tmplId)
{
	int i;

	if (a == NULL || tmplId < 0)
		return NULL;
	for (i = 0; i < a->errands.count; i++)
		if (a->errands.items[i].tmplId == tmplId &&
		    a->errands.items[i].state == ERRAND_ACTIVE)
			return &a->errands.items[i];
	return NULL;
}

/* Drop every terminal errand and slide the live ones down, so a long-lived
 * agent reuses its slots instead of filling the list with finished business.
 * Only ADOPTION compacts: a caller reads a terminal errand's state in the
 * frame it became terminal, which is exactly when it needs to. */
static void errandCompact(ErrandList *l)
{
	int i;
	int out = 0;

	for (i = 0; i < l->count; i++) {
		if (l->items[i].state == ERRAND_ACTIVE)
			l->items[out++] = l->items[i];
	}
	for (i = out; i < l->count; i++)
		errandBlank(&l->items[i]);
	l->count = out;
}

int errandAdopt(Agent *a, const ErrandTemplate *t, float now)
{
	Errand *e;

	if (a == NULL || t == NULL) {
		fprintf(stderr, "isomata: errandAdopt: no %s to adopt\n",
			(a == NULL) ? "agent" : "template");
		return ERRAND_INVALID;
	}
	/* THE ROLE GATE. An errand is registered for a role, so a customer's
	 * laundry round is refused for a critter and a staffer's fault round is
	 * refused for a customer. */
	if (!errandRoleMatches(t, a->role)) {
		fprintf(stderr,
			"isomata: errandAdopt: errand %s is for role %d, agent %d is role %d\n",
			t->name, t->role, a->id, a->role);
		return ERRAND_INVALID;
	}
	errandCompact(&a->errands);
	/* ONE INSTANCE OF A GOAL AT A TIME. A second copy would enqueue the
	 * same jobs twice and fight itself for the same machine. */
	if (errandFind(a, t->id) != NULL)
		return ERRAND_INVALID;
	if (a->errands.count >= ERRAND_MAX)
		return ERRAND_INVALID;
	e = &a->errands.items[a->errands.count];
	errandBlank(e);
	e->tmplId = t->id;
	e->step = 0;
	e->state = ERRAND_ACTIVE;
	e->startedAt = now;
	/* THE FLOOR IS RECORDED AND NOTHING MORE. No need, no panic, no job, no
	 * band, no claim: the errand it creates carries the declared class into
	 * boardEnqueue (which validates it and discards it — agents/board.h),
	 * and the ladder arbitrates the errand's JOBS against everything else
	 * exactly as it would a fill's. THAT is what "adopted at its declared
	 * floor, never forced" means in code today: a world event cannot put
	 * itself on top, because it never reaches the arithmetic at all.
	 * Pinned by test_the_floor_is_provenance_never_an_order (the job still
	 * bands 0 for a content agent) and by the needs snapshot in
	 * test_offer_to_agent_is_adopted_at_its_floor. */
	e->priorityFloor = t->priorityFloor;
	a->errands.count++;
	return t->id;
}

/* The most pressing served need of `t` for `a`, as a band: the SAME fold
 * jobUrgency performs (agents/activity.h), so an errand and a job compete on
 * identical terms. A template that serves nothing scores 0 — a different
 * answer from an error, and the same "nothing wants this" the activity layer
 * means by it. */
static int errandUrgency(const Agent *a, const ErrandTemplate *t)
{
	int best = 0;
	int n;

	for (n = 0; n < NEED_COUNT; n++) {
		int band;

		/* !(serves > 0.0f) rejects 0.0f and NaN in one comparison, so a
		 * broken row can never cast an undefined value to an int. */
		if (!(t->serves[n] > 0.0f))
			continue;
		band = (int)((float)wantUrgency(&a->needs, n) * t->serves[n]
			     + 0.5f);
		if (band > best)
			best = band;
	}
	return best;
}

int errandAdoptByNeed(Agent *a, float now)
{
	const ErrandTemplate *best = NULL;
	int bestBand = -1;
	int i;

	if (a == NULL)
		return ERRAND_INVALID;
	/* Table order is id order and the comparison is a strict `>`, so a
	 * tie goes to the LOWEST template id and the scan is permutable. A
	 * template whose needs are all content still scores 0 and is still a
	 * candidate: refusing every zero would leave a born agent with no
	 * goal at all, which is the inert-ladder failure agents/need.h warns
	 * about when it decides a panic must start clear. */
	for (i = 0; i < ERRAND_TABLE_SIZE; i++) {
		const ErrandTemplate *t = &ERRAND_TEMPLATES[i];
		int band;

		if (!errandRoleMatches(t, a->role))
			continue;
		if (errandFind(a, t->id) != NULL)
			continue;
		band = errandUrgency(a, t);
		if (band > bestBand) {
			bestBand = band;
			best = t;
		}
	}
	if (best == NULL)
		return ERRAND_INVALID;
	return errandAdopt(a, best, now);
}

int errandOfferToAgent(Agent *a, int templateId, float now)
{
	const ErrandTemplate *t;

	/* errandsTemplate reports an unknown id with a diagnostic, so the
	 * world's bad offer is visible rather than silently doing nothing. */
	t = errandsTemplate(templateId);
	if (t == NULL)
		return ERRAND_INVALID;
	/* From here the offer is an ordinary adoption: the same role gate,
	 * the same already-active refusal, the same recorded floor. The world
	 * decides WHAT to offer, never how much it outranks. */
	return errandAdopt(a, t, now);
}

int errandsOfferToRole(struct AgentRegistry *r, int templateId, float now)
{
	const ErrandTemplate *t;
	int adopted = 0;
	int i;

	if (r == NULL)
		return 0;
	/* A broken row is reported by errandsTemplate (with its diagnostic)
	 * and addressed to nobody: the WORLD named an errand that does not
	 * exist, which is a missing declaration rather than an agent's
	 * refusal. */
	t = errandsTemplate(templateId);
	if (t == NULL)
		return 0;
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a = &r->slots[i];

		if (!r->alive[i])
			continue;
		/* The same gate errandAdopt applies, read from the same row, so
		 * "who is this errand for" has exactly one answer in the
		 * framework. */
		if (a->role != t->role)
			continue;
		/* Every refusal below is an ordinary answer: the agent already
		 * holds this goal, or its list is full. Neither is an error and
		 * neither stops the sweep — the rest of the floor is still
		 * being offered. */
		if (errandOfferToAgent(a, templateId, now) == ERRAND_INVALID)
			continue;
		adopted++;
	}
	return adopted;
}

void errandBindTarget(Errand *e, int targetKind, int id, int targetTile)
{
	if (e == NULL)
		return;
	e->targetKind = targetKind;
	e->targetId = id;
	e->targetTile = targetTile;
	/* A fresh target is a fresh chance: the blacklist bounded the OLD
	 * candidate, not this one. */
	e->blacklistCount = 0;
}

int errandStepGate(const Errand *e)
{
	const ErrandStep *s = errandStepAt(e);

	if (s == NULL || s->kind != STEP_GATE)
		return ERRAND_INVALID;
	return s->gate;
}

void errandGateSeconds(Errand *e, float secs)
{
	if (e == NULL)
		return;
	/* Stored as a whole number of seconds in an int field, so the record
	 * stays integral and comparable; a caller with a sub-second budget
	 * rounds up, because rounding DOWN would shorten a deadline the world
	 * promised. Non-positive means "unset", which a gate refuses to park
	 * against (see errandAdvance). */
	e->gateSecs = (secs > 0.0f) ? (int)(secs + 0.5f) : ERRAND_INVALID;
}

void errandBlacklist(Errand *e, int id)
{
	errandExclude(e, id);
}

bool errandBlocked(const Errand *e)
{
	const ErrandStep *s;

	if (e == NULL || e->state != ERRAND_ACTIVE)
		return false;
	s = errandStepAt(e);
	if (s == NULL)
		return false;
	if (s->kind == STEP_GATE)
		return true;	/* waiting on a wake that has not come */
	/* A step with nothing bound is waiting on the WORLD to find its
	 * thing; a step with a thing bound is about to enqueue work, and a
	 * step with work enqueued is busy. Neither is "blocked". */
	return e->targetKind == ERRAND_TARGET_NONE;
}

bool errandRewindToJob(Errand *e, int jobKind)
{
	const ErrandTemplate *t;
	int i;

	if (e == NULL || jobKind < 0)
		return false;
	t = errandsTemplate(e->tmplId);
	if (t == NULL)
		return false;
	/* Backwards, inclusive of the current step: the step that PRODUCED the
	 * lost thing is at or before the one that lost it. */
	for (i = (e->step < t->stepCount) ? e->step : t->stepCount - 1;
	     i >= 0; i--) {
		if (t->steps[i].kind == STEP_JOB &&
		    t->steps[i].jobKind == jobKind) {
			e->step = i;
			e->failureCount = 0;
			e->jobHandle = ERRAND_INVALID;
			/* The binding stays as the caller left it (cleared, by
			 * the re-plan that called this): what is missing is a
			 * fresh thing to bind, which is the caller's next act. */
			return true;
		}
	}
	e->state = ERRAND_FAILED;
	e->step = 0;
	e->jobHandle = ERRAND_INVALID;
	return false;
}

void errandAbandon(Errand *e)
{
	if (e == NULL)
		return;
	e->state = ERRAND_ABANDONED;
	e->step = 0;
	e->jobHandle = ERRAND_INVALID;
	/* The target is NOT cleared: abandoning is a fact about the errand, and
	 * the caller releasing whatever the target holds is its own business
	 * (the errand layer has no claim on a machine). Keeping it lets the
	 * caller find the machine it must give back. */
}

/* Build the job for `s` and put it on `b`. Returns the handle, or
 * ERRAND_INVALID when the row does not resolve (a template naming an activity
 * the catalogue has never heard of — a broken template, reported and refused,
 * never guessed at), the target is unbound, or the board refused the row. */
static int errandEnqueueJob(int jobKind, Errand *e, Agent *a, JobBoard *b)
{
	Job j;
	int handle;
	int band;

	/* An unbound target is a hard failure, never a walk to the origin:
	 * this is the same rule activityBindJob states for the activity
	 * layer, and the caller — which owns the registry — has not found this
	 * step's thing yet. */
	if (e->targetTile == ERRAND_INVALID)
		return ERRAND_INVALID;
	if (!activityBindJob(&j, jobKind)) {
		fprintf(stderr,
			"isomata: errandAdvance: template %d step %d names unknown activity %d\n",
			e->tmplId, e->step, jobKind);
		return ERRAND_INVALID;
	}
	/* The errand names the THING; the job names the TILE to walk to, and
	 * it always walks — even for an in-place catalogue row such as the
	 * gate's stand-by job, whose "in place" default the caller overrides
	 * with the wait spot. */
	jobSetTarget(&j, JOB_TARGET_TILE, e->targetTile);
	/* The band is COMPUTED from the needs this activity serves (agents/
	 * activity.h), never assigned by the errand. Provenance may decide who
	 * may claim a job and may never decide its order (agents/board.h) —
	 * and the board does not store the recorded priorityFloor at all yet,
	 * so today the band's own arithmetic is the whole story.
	 *
	 * ONE EXCEPTION, and it is a floor the COMPUTED band can never go
	 * below rather than a number of our own: a GATE is a continuation of
	 * the step before it, so its stand-by job carries that step's band.
	 * The spec's rule is "an errand's continuation always outranks the
	 * agent's own filler, so the moment a machine finishes and its COLLECT
	 * job becomes available, it preempts the arcade game mid-animation" —
	 * and without the floor the gate would break that rule in the one
	 * place it matters: the stand-by job is a wander, which on its own
	 * merits bands 0, while the nap it would be waiting out bands 6. The
	 * customer would then sleep through their own wash and the errand's
	 * patience would burn down for nothing. Both numbers are still
	 * computed from needs — this only refuses to let the continuation
	 * under-sell the work it continues. */
	band = jobUrgency(a, &j);
	if (band < 0) {
		/* Unreachable for a shipped template — the id came from a
		 * template row and this is the very catalogue it resolves
		 * against — and handled rather than assumed: a job that cannot
		 * be scored is a broken job, not a boring one. */
		fprintf(stderr,
			"isomata: errandAdvance: template %d step %d job %d could not be scored\n",
			e->tmplId, e->step, jobKind);
		return ERRAND_INVALID;
	}
	if (jobKind == ERRAND_GATE_JOB && band < e->stepBand)
		band = e->stepBand;
	j.band = band;
	if (jobKind != ERRAND_GATE_JOB)
		e->stepBand = band;
	handle = boardEnqueue(b, &j, e->priorityFloor);
	if (handle == ERRAND_INVALID)
		return ERRAND_INVALID;
	e->jobHandle = handle;
	return handle;
}

/* A finished step: one job's end. A DONE job advances the errand; a FAILED
 * job re-selects on the same step, and a failed GATE falls back to the job
 * step whose wake it was waiting on — which is how a broken or mute machine
 * sends the customer back to the machine step to choose a different one. */
static void errandFinishJob(Errand *e, const ErrandStep *s, JobBoard *b,
			    int state)
{
	const ErrandTemplate *t = errandsTemplate(e->tmplId);

	if (e->jobHandle != ERRAND_INVALID) {
		/* Retire, do not release: the job is FINISHED business, and a
		 * long-lived personal board that kept its terminal rows would
		 * fill up with them (board.h: retirement is the reclaim path
		 * for a job nobody will run again). */
		(void)boardRetire(b, e->jobHandle);
		e->jobHandle = ERRAND_INVALID;
	}
	if (state == JOB_DONE) {
		e->failureCount = 0;
		e->step++;
		/* Past the last step there is nothing left to do. A template
		 * that declares none is ERRAND_DONE the moment it starts:
		 * "do nothing" is a complete, honest answer. */
		if (t == NULL || e->step >= t->stepCount) {
			e->state = ERRAND_DONE;
			e->step = 0;
		}
		return;
	}
	/* FAILED: count it against this step and re-select. */
	e->failureCount++;
	if (e->failureCount > ERRAND_MAX_STEP_FAILURES) {
		fprintf(stderr,
			"isomata: errandAdvance: errand %d gave up on step %d after %d failures\n",
			e->tmplId, e->step, e->failureCount);
		e->state = ERRAND_FAILED;
		e->step = 0;
		e->jobHandle = ERRAND_INVALID;
		return;
	}
	/* Never the same thing twice: the failed target goes on this errand's
	 * blacklist, and the binding clears so the caller's next pass picks a
	 * different one.
	 *
	 * THE FIELDS ARE CLEARED DIRECTLY rather than through errandBindTarget,
	 * which is correct to clear the blacklist (a fresh binding is a fresh
	 * chance) and exactly wrong here: the blacklist was JUST written, and
	 * asking for a re-bind that would empty it again would make "never the
	 * same thing twice" a comment rather than a rule. */
	/* Only a THING is blacklisted: a gate's binding is the WAIT SPOT, and a
	 * packed tile that happened to equal a machine handle would put a
	 * perfectly good washer on a list it never misbehaved on. */
	if (e->targetKind == ERRAND_TARGET_MACHINE)
		errandExclude(e, e->targetId);
	e->targetKind = ERRAND_TARGET_NONE;
	e->targetId = ERRAND_INVALID;
	e->targetTile = ERRAND_INVALID;
	if (s->kind == STEP_GATE) {
		/* The machine whose wake never came is the whole reason this
		 * gate failed: go back to the step that bound it. */
		e->step = (e->retryStep >= 0) ? e->retryStep : 0;
	}
}

void errandAdvance(Errand *e, Agent *a, float now, JobBoard *b)
{
	const ErrandTemplate *t;
	const ErrandStep *s;
	Job *j;

	/* `now` is the caller's clock, and it is here so an adoption and a
	 * re-bind can be dated by the world rather than by this module. The
	 * gate's own deadline is the caller's too (errandGateSeconds): only
	 * the world knows how long a machine runs, and this module has no
	 * clock of its own to time it with. */
	(void)now;
	if (e == NULL || a == NULL || b == NULL)
		return;
	if (e->state != ERRAND_ACTIVE)
		return;
	t = errandsTemplate(e->tmplId);
	s = errandStepAt(e);
	if (t == NULL || s == NULL) {
		/* A record naming no template (or an out-of-range step) cannot
		 * advance, and pretending it is waiting would hang the caller. */
		fprintf(stderr,
			"isomata: errandAdvance: errand %d has no usable step %d\n",
			e->tmplId, e->step);
		e->state = ERRAND_FAILED;
		e->step = 0;
		return;
	}

	/* Something already enqueued for this step: let the framework's tick
	 * run it, or read its ending. */
	if (e->jobHandle != ERRAND_INVALID) {
		j = boardJob(b, e->jobHandle);
		if (j == NULL) {
			/* The row is GONE — retired or overwritten between two
			 * calls. It is not this errand's to read the ending of,
			 * so the step re-selects rather than advancing on a
			 * result nobody reported. */
			e->jobHandle = ERRAND_INVALID;
			if (e->targetKind == ERRAND_TARGET_MACHINE)
				errandExclude(e, e->targetId);
			e->targetKind = ERRAND_TARGET_NONE;
			e->targetId = ERRAND_INVALID;
			e->targetTile = ERRAND_INVALID;
			e->failureCount++;
			return;
		}
		if (j->state == JOB_DONE || j->state == JOB_FAILED) {
			/* THE URGENCY THIS ERRAND WAS ACTUALLY RUNNING AT. A
			 * job's band is recomputed every tick from the needs it
			 * serves, so the band at ENQUEUE time says what the work
			 * was worth to a content agent — which is what the
			 * stand-by gate would have inherited, and is precisely
			 * the wrong answer. The gate continues this step, so it
			 * inherits the band the step actually ran at. */
			if (j->state == JOB_DONE && j->band > e->stepBand)
				e->stepBand = j->band;
			errandFinishJob(e, s, b, j->state);
		}
		return;
	}

	/* Nothing enqueued yet. A gate parks a bounded stand-by job; a job
	 * step enqueues its own work — but only once the caller has bound a
	 * target. */
	if (s->kind == STEP_GATE) {
		/* The step whose wake this gate waits for is the PREVIOUS
		 * step's job, which has already ended — that is how the errand
		 * got here — so a failure here returns to it. */
		e->retryStep = (e->step > 0) ? e->step - 1 : ERRAND_INVALID;
		/* A GATE WITH NO DEADLINE IS NOT A WAIT, IT IS A HANG. Only the
		 * world knows how long a machine runs, so an unset budget
		 * enqueues nothing: the caller has not declared the bound yet
		 * and the next pass will. */
		if (e->gateSecs == ERRAND_INVALID)
			return;
		if (errandEnqueueJob(ERRAND_GATE_JOB, e, a, b) == ERRAND_INVALID)
			return;
		/* The stand-by job's whole purpose is to run out of time, so its
		 * duration IS the deadline the world declared. */
		{
			Job *gj = boardJob(b, e->jobHandle);

			if (gj != NULL)
				gj->duration = (float)e->gateSecs;
		}
		return;
	}
	/* STEP_JOB: a step with nothing bound has to wait for the caller. */
	if (e->targetTile == ERRAND_INVALID)
		return;
	(void)errandEnqueueJob(s->jobKind, e, a, b);
}

const char *errandStateName(int state)
{
	switch (state) {
	case ERRAND_ACTIVE:
		return "ACTIVE";
	case ERRAND_DONE:
		return "DONE";
	case ERRAND_FAILED:
		return "FAILED";
	case ERRAND_ABANDONED:
		return "ABANDONED";
	default:
		return "?";
	}
}

const char *errandStepKindName(int kind)
{
	switch (kind) {
	case STEP_JOB:
		return "JOB";
	case STEP_GATE:
		return "GATE";
	case STEP_GATE_WAIT_MACHINE:
		return "WAIT_MACHINE";
	case STEP_GATE_WALK_TO:
		return "WALK_TO";
	default:
		return "JOB";
	}
}

bool errandStateValid(int state)
{
	return state >= ERRAND_ACTIVE && state < ERRAND_STATE_COUNT;
}
