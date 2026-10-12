/*
 * The customer driver: the world's half of a customer running an errand (see
 * sim/brain.h for the model, and agents/errand.h for what the driver does NOT
 * decide).
 *
 * PURE GLUE OVER THE FRAMEWORK AND THE T22 MACHINE LAYER. No allocation, no
 * SDL, no time source: the world advances only by the caller's dt, so a
 * fake-dt scenario is deterministic and replayable.
 *
 * THE ORDER OF A CUSTOMER'S TICK, and why it is this order:
 *
 *   1. machinesUpdate, and each wake routed to its owner's customer — a wake
 *      is delivered BEFORE the driver looks at anything, so the machine's
 *      PHASE_DONE can finish the gate this very tick.
 *   2. agentsUpdate (agents/agent.h): needs decay, the one switch decision,
 *      and the job lifecycle for whatever every customer is running.
 *   3. this file's driver: the world's effects on the step in flight (claim,
 *      pay, load, start, collect, release, the garment), then the errand's own
 *      advance, then the blocked-time satisfaction decay and the walk out.
 *
 * The errand's step index says WHICH half of the leg the customer is in, so
 * the driver derives its leg rather than carrying a phase of its own: the old
 * BrainPhase switch is gone and this file has no equivalent. What it does
 * carry is the world's bookkeeping the framework cannot hold — which machine,
 * whether the fee has been paid, which job is walking to the door.
 *
 * A CUSTOMER NEVER LEAVES HOLDING SOMEBODY ELSE'S WASHING, and that is a TEST
 * rather than an intention. There are THREE places this file gives a machine
 * back, and every path that can END while holding one goes through one of them:
 *
 *   - brainReplan — the shared release. It reads the machine the CUSTOMER is
 *     holding (c->target) rather than the errand's binding, because the two are
 *     not the same thing and only one of them is still true once an errand has
 *     gone terminal. Every failure takes it (a claim refused at arrival, a route
 *     that will not compute, a gate whose bound expired, a machine that broke
 *     mid-run, an errand abandoned for want of patience) and so does the walk
 *     out, including an errand that went terminal while its step still held a
 *     machine, and a body or mind that vanished underneath it.
 *   - brainDriveLoad's "claimed but cannot pay": a machine taken straight back
 *     rather than through the re-plan, because the errand is being abandoned
 *     and there is nothing to re-select.
 *   - brainDriveCollect's completion: the NORMAL release, on the happy path.
 *
 * (The list is about which PATHS can end holding a machine, not about where
 * the call happens: two of the three are not failure paths.)
 *
 * Three oracles back the sentence. tests/test_brain.c asserts the claim
 * invariant on EVERY TICK of every scenario (no machine is ever owned by a
 * customer that no longer exists),
 * test_a_terminal_errand_releases_the_machine_it_holds aims it at the walk out,
 * and test_the_mind_vanishing_releases_the_machine aims it at the death branch.
 */

#include "sim/brain.h"

#include "agents/activity.h"
#include "agents/errand.h"
#include "ai/nav.h"

#include <stddef.h>

/* A slot is in use exactly when it holds a live mind. AGENT_INVALID is the
 * free marker (it is -1, so a zeroed slot does NOT read as live). This
 * replaces the old BrainPhase's BRAIN_PH_INACTIVE gate: what a slot is for is
 * no longer a phase of a switch. */
/* The only caller-side fact: does this slot hold a live mind? Every call site
 * passes a record taken out of the registry (`&w->customers[i]`, or a `c` bound
 * to one), so there is no NULL arm — the registry itself is the check. */
static bool brainSlotUsed(const SimCustomer *c)
{
	return c->agent != AGENT_INVALID;
}

/* Append one event if the buffer has room (a bounded, deterministic prefix). */
static void brainEmit(BrainEvent *out, int cap, int *n, int kind,
		      const SimCustomer *c, MachineHandle machine, int amount,
		      int score, int distance)
{
	BrainEvent *ev;

	if (out == NULL || cap <= 0 || n == NULL || *n >= cap)
		return;
	ev = &out[*n];
	ev->kind = kind;
	ev->customer = c->handle;
	ev->id = c->id;
	ev->policy = c->policy;
	ev->machine = machine;
	ev->amount = amount;
	ev->score = score;
	ev->distance = distance;
	ev->garment = c->garment;
	ev->tmpl = ERRAND_INVALID;
	ev->band = 0;
	(*n)++;
}

/* The leg an errand step puts the customer in. A gate is a wait for the world,
 * a USE_MACHINE job is the load half of a leg, a COLLECT job its other half,
 * and a LEAVE job is the walk out — so this is the ONE place that reads a
 * step and says what the driver does about it. */
static int brainLegOfStep(const Errand *e)
{
	const ErrandTemplate *t;
	const ErrandStep *s;

	if (e == NULL)
		return BRAIN_LEG_NONE;
	t = errandsTemplate(e->tmplId);
	if (t == NULL || e->step < 0 || e->step >= t->stepCount)
		return BRAIN_LEG_NONE;
	s = &t->steps[e->step];
	if (s->kind == STEP_GATE)
		return BRAIN_LEG_GATE;
	switch (s->jobKind) {
	case JOB_USE_MACHINE:
		return BRAIN_LEG_LOAD;
	case JOB_COLLECT:
		return BRAIN_LEG_COLLECT;
	case JOB_LEAVE:
		return BRAIN_LEG_LEAVE;
	default:
		return BRAIN_LEG_NONE;
	}
}

/* The current step, or NULL when the errand names no template (or an index
 * outside it). Every caller that reads the step goes through this, so the
 * bounds check exists in exactly one place. */
static const ErrandStep *brainStepOf(const Errand *e)
{
	const ErrandTemplate *t = errandsTemplate(e->tmplId);

	if (e == NULL || t == NULL || e->step < 0 || e->step >= t->stepCount)
		return NULL;
	return &t->steps[e->step];
}

/* Give back whatever this customer is holding, then say so. This is THE
 * release path: the machine the customer had claimed goes FREE (a broken
 * machine has already dropped its owner and is left broken), and the errand's
 * binding is cleared so the next pass has to choose again. A customer holding
 * nothing is still a legitimate call — a route that would not compute holds
 * nothing — and costs no event.
 *
 * THE RELEASE IS UNCONDITIONAL; ONLY THE BINDING NEEDS AN ERRAND. The machine
 * this gives back is named by the CUSTOMER (`c->target`), which is why the
 * release must not be gated on the errand: the one path that most needs it is
 * the walk out, and by then the errand may already be terminal — and a NULL
 * there would skip the release and leave a washer claimed by somebody who has
 * left the building. `e` is only the errand's own binding to clear, so that
 * half is guarded. Pinned by
 * test_a_terminal_errand_releases_the_machine_it_holds. */
static void brainReplan(SimWorld *w, SimCustomer *c, Errand *e, BrainEvent *out,
			int cap, int *n, int kind)
{
	Machine *m = machineGet(&w->machines, c->target);

	if (m != NULL && m->owner == c->handle) {
		machineRelease(m);
		brainEmit(out, cap, n, kind, c, c->target, 0, 0, 0);
	}
	/* Clear the binding so errandAdvance cannot walk to a thing nobody
	 * holds, and drop the sub-step so a re-selected leg pays again. */
	if (e != NULL)
		errandBindTarget(e, ERRAND_TARGET_NONE, ERRAND_INVALID,
				 ERRAND_INVALID);
	c->target = MACHINE_INVALID;
	c->actStep = BRAIN_STEP_NONE;
	c->waitRetries = 0;
	c->timer = 0.0f;
	c->gateDeadline = 0.0f;
}

/* The leave job itself: one declaration (ACT_LEAVE), one band computed from
 * the needs it serves, one board. It is the customer's own errand-class work,
 * so agentsUpdate routes the body to the door exactly as it routes a wash —
 * and a door it cannot reach fails the job, which is how the "leave where you
 * stand" path happens without this file walking anybody by hand. Returns the
 * handle, or ERRAND_INVALID when the board refused it. */
/* The leave job. Every caller reaches it with the customer's own live agent
 * (brainDrive returns early without one), so there is no NULL arm here;
 * ACT_LEAVE is a shipped catalogue row, so its bind cannot fail either. The
 * one thing that CAN fail is boardEnqueue on a full board, and that is the
 * caller's to cope with. */
static int brainEnqueueLeave(SimWorld *w, Agent *a)
{
	Job j;
	int h;

	if (!activityBindJob(&j, ACT_LEAVE))
		return ERRAND_INVALID;
	/* The door is the world's business: the catalogue row says "leaving",
	 * not "leaving through THAT door". */
	jobSetTarget(&j, JOB_TARGET_TILE, JOB_TILE_PACK(w->exitX, w->exitZ));
	j.band = jobUrgency(a, &j);
	h = boardEnqueue(&a->personal, &j, JOB_PRIO_ERRAND);
	return h;
}

/* Begin the walk to the exit. Used by the abandon path and by an errand that
 * ended without reaching its own LEAVE step.
 *
 * The "already on the way out" guard is duplicated defence TODAY: brainDrive
 * performs the same check two functions up, before it looks for an errand, so
 * both call sites arrive with leaveJob == ERRAND_INVALID. It is kept (and named
 * in coverage-min.txt) because it is the only thing standing between a future
 * caller that enqueues a leave job from somewhere else and a customer being
 * announced as leaving twice — the case
 * test_an_errand_dying_on_its_leave_step_walks_out_once describes. */
static void brainGoToExit(SimWorld *w, SimCustomer *c, Agent *a, BrainEvent *out,
			  int cap, int *n)
{
	if (c->leaveJob != ERRAND_INVALID)
		return;		/* already on the way out */
	brainEmit(out, cap, n, BRAIN_EV_LEAVE_BEGIN, c, MACHINE_INVALID, 0, 0, 0);
	c->leaveJob = brainEnqueueLeave(w, a);
	if (c->leaveJob == ERRAND_INVALID) {
		/* The board refused the row: there is nothing left to wait for,
		 * so the customer leaves where it stands. Still honest. */
		brainEmit(out, cap, n, BRAIN_EV_DEPART, c, MACHINE_INVALID, 0, 0,
			  0);
		c->wantDespawn = true;
	}
}

/* The step's job, or NULL when the errand has none enqueued yet. `a` is the
 * errand's agent: the job lives on that agent's own board, and every caller
 * has a non-NULL agent by the time it gets here (brainDrive returns early
 * without one), so there is no arm for that. */
static Job *brainStepJob(Agent *a, Errand *e)
{
	if (e->jobHandle == ERRAND_INVALID)
		return NULL;
	return boardJob(&a->personal, e->jobHandle);
}

/* Abandon the errand and walk out. The abandoned step's claim is released
 * FIRST: a customer who gives up must never leave the building still holding a
 * machine, or that washer stays claimed until something else breaks it. */
static void brainAbandonErrand(SimWorld *w, SimCustomer *c, Agent *a, Errand *e,
			       BrainEvent *out, int cap, int *n)
{
	/* Once per errand. The two give-up tests (cannot pay, patience spent)
	 * are separate and could both fire in one tick; without this a single
	 * abandoned customer would be announced ABANDONed twice. Not reachable
	 * by the shipped flow today (a customer that just paid was not blocked),
	 * which is why it is named rather than tested — see coverage-min.txt. */
	if (c->abandoned)
		return;
	c->abandoned = true;
	brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
	errandAbandon(e);
	brainEmit(out, cap, n, BRAIN_EV_ABANDON, c, c->target, 0, 0, 0);
	brainGoToExit(w, c, a, out, cap, n);
}

/* Choose a machine for the errand's current step and bind it. This is the
 * heuristic selection the T23 demo has always used, called now from the
 * errand's select step: the policies, their scores and their logging are
 * unchanged, and the demo's visible divergence between them is still what
 * decides which customer walks where. Returns false when nothing is free — the
 * caller then takes the bounded no-candidate wait. */
static bool brainSelectMachine(SimWorld *w, SimCustomer *c, const Entity *ent,
			       Errand *e, BrainEvent *out, int cap, int *n)
{
	const ErrandStep *s = brainStepOf(e);
	MachineHandle chosen;
	const Machine *m;
	int score;
	int distance;

	chosen = selChoose(&w->machines, ent, &w->select, c->policy,
			   s->machineKind, e->blacklist, e->blacklistCount);
	if (chosen == MACHINE_INVALID) {
		/* Nothing free right now. This is a WAIT, not a failure: the step
		 * keeps whatever it had, the bounded retry decides how long, and
		 * the errand's blocked-time clock runs meanwhile. */
		c->waitRetries++;
		c->timer = BRAIN_WAIT_RETRY_SECS;
		brainEmit(out, cap, n, BRAIN_EV_NO_CANDIDATE, c, MACHINE_INVALID,
			  0, 0, 0);
		return false;
	}
	/* NO NULL ARM HERE, and the reason is the same one agent.c used when it
	 * removed its fill's boardClaim guard: selChoose returns a handle from
	 * the very registry it was handed (sim/selection.h: "a candidate is a
	 * live machine"), and nothing runs between that call and this read, so
	 * the handle names a live machine by construction. A guard here would
	 * be a branch that can never be false. */
	m = machineGetConst(&w->machines, chosen);
	errandBindTarget(e, ERRAND_TARGET_MACHINE, chosen,
			 JOB_TILE_PACK(m->tileX, m->tileZ));
	c->target = chosen;
	score = selScore(ent, m, &w->select, c->policy);
	distance = (m->tileX > ent->tileX ? m->tileX - ent->tileX
					  : ent->tileX - m->tileX) +
		   (m->tileZ > ent->tileZ ? m->tileZ - ent->tileZ
					  : ent->tileZ - m->tileZ);
	brainEmit(out, cap, n, BRAIN_EV_TASK_BEGIN, c, chosen, m->fee, score,
		  distance);
	return true;
}

/* The USE_MACHINE half of a leg: on arrival claim the machine, pay the fee and
 * begin the load; when the job's own wait (the load) ends, start the run and
 * hand the errand its gate. The job's lifecycle does the walking and the
 * timing — this is only the half no framework module may hold. */
static void brainDriveLoad(SimWorld *w, SimCustomer *c, Agent *a, Errand *e,
			   Job *j, Entity *ent, BrainEvent *out, int cap, int *n)
{
	Machine *m;

	/* A step with NOTHING bound is not a step whose machine vanished: it is
	 * a step waiting for the world to find one, which is the bounded
	 * no-candidate wait's business (brainSupplyTarget), not a failure.
	 * Treating it as one would re-plan every tick, and the re-plan resets
	 * the retry countdown — so the customer would ask the world once per
	 * tick and wait for nothing. */
	if (c->target == MACHINE_INVALID && e->targetTile == ERRAND_INVALID)
		return;
	m = machineGet(&w->machines, c->target);
	if (m == NULL) {		/* machine vanished mid-leg */
		brainReplan(w, c, e, out, cap, n, BRAIN_EV_CLAIM_DENIED);
		return;
	}
	if (c->actStep == BRAIN_STEP_PAY) {
		/* The job is WAITING, which on this leg means the body has
		 * arrived: agentsUpdate walks the job's target and the tick
		 * enters the activity there. */
		if (j == NULL || j->state != JOB_WAITING)
			return;
		/* Claiming here is the exclusivity guarantee (machines.h), and a
		 * refusal is NOT a blacklist of the machine: the FREE filter
		 * already excludes it while it is somebody else's, so the
		 * bounded wait lets this customer take it once it frees. */
		if (!machineClaim(m, c->handle)) {
			/* Genuine contention: fail the job so the errand
			 * re-selects, and leave the machine to its owner. (j
			 * is non-NULL here by construction: the branch above
			 * returned unless it was.) */
			jobStep(j, JOB_DENIED);
			brainReplan(w, c, e, out, cap, n, BRAIN_EV_CLAIM_DENIED);
			return;
		}
		/* `ent` is non-NULL here by construction: brainDrive returns
		 * early unless the body exists, and this is the only call site. */
		if (ent->money < m->fee) {
			/* Claimed and cannot pay: give the machine straight back
			 * and give the errand up. Half a transaction is not a
			 * transaction. */
			machineRelease(m);
			brainAbandonErrand(w, c, a, e, out, cap, n);
			return;
		}
		ent->money -= m->fee;
		brainEmit(out, cap, n, BRAIN_EV_PAY, c, c->target, m->fee, 0, 0);
		brainEmit(out, cap, n, BRAIN_EV_LOAD, c, c->target, 0, 0, 0);
		c->actStep = BRAIN_STEP_LOAD;
		return;
	}
	/* THE CLAIMED MACHINE AND A JOB THAT DIED. Preemption, a lost target
	 * and a denied step all fail the job this errand is standing on, and a
	 * failed job releases whatever it holds — that is the spec's "cancel
	 * releases held resources", and here it is the difference between a
	 * washer and a customer walking out. */
	if (c->actStep == BRAIN_STEP_LOAD && j != NULL &&
	    j->state == JOB_FAILED) {
		brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
		return;
	}
	/* The load's own wait has run out: the machine goes. */
	if (c->actStep != BRAIN_STEP_LOAD || j == NULL ||
	    j->state != JOB_DONE)
		return;
	if (!machineStartRun(m))
		return;		/* not ours any more: nothing to start */
	c->actStep = BRAIN_STEP_NONE;
	brainEmit(out, cap, n, BRAIN_EV_RUN_START, c, c->target, 0, 0, 0);
	/* THE GATE'S DEADLINE. Bounded by the run plus a slack, exactly as the
	 * old local waitBudget was — but it now rides on the wait job, so an
	 * expired bound FAILS that job (JOB_TIMEOUT) instead of hanging this
	 * customer on a machine that never delivers its wake. */
	c->gateDeadline = w->now + m->runLeft + BRAIN_MACHINE_WAIT_SLACK;
	errandGateSeconds(e, m->runLeft + BRAIN_MACHINE_WAIT_SLACK);
	/* THE OWNER WALKS AWAY. The gate's stand-by job targets the wait spot,
	 * not the machine: the run is the machine's now, and a customer that
	 * stood on its own washing would be standing in the way of the next
	 * customer. This is the demo's old "walk to the wait tile" line, and it
	 * is a JOB doing it rather than a phase doing it. */
	errandBindTarget(e, ERRAND_TARGET_PLACE, JOB_TILE_PACK(w->waitX, w->waitZ),
			 JOB_TILE_PACK(w->waitX, w->waitZ));
}

/* The gate half: the machine's own wake ends the wait, a broken machine or an
 * expired bound fails it, and either way the errand re-selects. The machine is
 * released only if this customer still owns it — a broken machine has already
 * dropped its owner (machines.h), and releasing somebody else's claim would
 * be the worst possible bug here. */
static void brainDriveGate(SimWorld *w, SimCustomer *c, Errand *e, Job *j,
			   BrainEvent *out, int cap, int *n)
{
	/* The machine this gate is waiting on is the CUSTOMER's record of it,
	 * not the errand's binding: the gate deliberately re-binds the errand
	 * to the WAIT SPOT (the owner walks away), so e->targetId is a tile at
	 * this point and reading it as a machine handle would be reading the
	 * washer as if it were the dryer. */
	Machine *m = machineGet(&w->machines, c->target);
	int wake = c->pendingWake;

	/* THE BOUND, AND THE JOB_TIMEOUT IT PRODUCES. The errand parks this
	 * gate's wait job with the deadline as the job's DURATION, but the job's
	 * own timer only starts once the body is parked — after the walk to the
	 * wait spot — so it can never be the earlier of the two. This clock,
	 * started when the run started, always is, and so the bounded wait ends
	 * as a TIMEOUT the lifecycle can see rather than as a quiet completion
	 * that reads as success. Only the two cells that accept TIMEOUT are
	 * fired at: a deadline must never kill a job that is waiting on
	 * nothing, and a job that is still walking to the wait spot is not
	 * waiting on it yet. */
	if (c->gateDeadline > 0.0f && w->now >= c->gateDeadline &&
	    j != NULL && (j->state == JOB_WAITING ||
			  j->state == JOB_INTERACTING)) {
		jobStep(j, JOB_TIMEOUT);
		c->gateDeadline = 0.0f;
	}

	if (wake == MACHINE_WAKE_INTERRUPTED) {
		/* Broke mid-run: the garment is untouched, the machine is BROKEN
		 * (so selection skips it), and the errand picks another. */
		c->pendingWake = -1;
		brainEmit(out, cap, n, BRAIN_EV_INTERRUPTED, c, c->target, 0, 0, 0);
		c->gateDeadline = 0.0f;
		brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
		return;
	}
	if (wake == MACHINE_WAKE_PHASE_DONE) {
		if (m == NULL || m->owner != c->handle) {
			/* The wake names a machine we no longer own: there is
			 * nothing to collect, so re-plan rather than walk to a
			 * stranger's washing. */
			c->pendingWake = -1;
			brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
			return;
		}
		/* THE WAKE ENDS THE WAIT — but only where the wait IS. A job
		 * ends through the lifecycle, and PHASE_DONE is the WAITING
		 * cell's, so a wake that lands while the owner is still
		 * WALKING to the wait spot has nowhere to go yet. It is not
		 * dropped: the machine is DONE and stays DONE, and this
		 * customer's claim on it is what keeps it that way, so the
		 * wake simply waits the few ticks until the job is parked.
		 * Delivering it earlier would end the walk — and the
		 * framework's own table says WALKING + PHASE_DONE is a
		 * handled no-op, so it would be dropped on the floor. */
		if (j == NULL || j->state != JOB_WAITING)
			return;
		jobStep(j, JOB_PHASE_DONE);
		c->pendingWake = -1;
		c->gateDeadline = 0.0f;
		brainEmit(out, cap, n, BRAIN_EV_MACHINE_DONE, c, c->target, 0, 0,
			  0);
	}
	if (j != NULL && j->state == JOB_FAILED) {
		/* The bound expired: the wait job failed through the lifecycle,
		 * which is the one path that can end a gate nobody answered. */
		brainEmit(out, cap, n, BRAIN_EV_WAIT_TIMEOUT, c, c->target, 0, 0,
			  0);
		/* AND THE ERRAND WILL NOT WAIT ON IT AGAIN. The blacklist is the
		 * errand's, but the machine is the CALLER's to name: this gate's
		 * binding is the wait spot, so the id that actually went wrong
		 * can only come from here. (A machine that merely broke does not
		 * need this — BROKEN is excluded from selection already — but a
		 * machine that simply never reports back is exactly what the
		 * blacklist is for.) */
		{
			/* BEFORE the re-plan, which clears the machine the
			 * customer was waiting on. */
			MachineHandle waited = c->target;

			brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
			errandBlacklist(e, waited);
		}
	}
}

/* The COLLECT half: on arrival take the load out (releasing the machine, which
 * is what advances the garment), and when the job's wait ends the errand is
 * done with this machine. A machine that broke or was taken between the gate
 * and the arrival sends the errand back to the USE_MACHINE step that produced
 * it — which is what the old driver did by re-driving the whole plan task. */
static void brainDriveCollect(SimWorld *w, SimCustomer *c, Errand *e, Job *j,
			      BrainEvent *out, int cap, int *n)
{
	Machine *m = machineGet(&w->machines, c->target);
	const ErrandStep *s = brainStepOf(e);

	if (c->actStep == BRAIN_STEP_NONE) {
		if (j != NULL && j->state == JOB_INTERACTING) {
			/* At the machine, with the load still ours and the machine
			 * DONE. If it is gone or was broken there is nothing to
			 * collect, and walking the errand on would hand it a
			 * step it cannot finish. */
			if (m == NULL || m->owner != c->handle) {
				brainReplan(w, c, e, out, cap, n, BRAIN_EV_REPLAN);
				errandRewindToJob(e, JOB_USE_MACHINE);
				return;
			}
			c->actStep = BRAIN_STEP_COLLECT;
			brainEmit(out, cap, n, BRAIN_EV_COLLECT, c, c->target, 0, 0,
				  0);
			return;
		}
	}
	/* The collect's own wait has run out: the machine goes back on the
	 * free list and the LOAD is one step further along. Releasing first is
	 * the point — the machine is released whether or not the garment
	 * transition is the one this step's machine kind can drive. */
	if (c->actStep != BRAIN_STEP_COLLECT || j == NULL ||
	    j->state != JOB_DONE)
		return;
	c->actStep = BRAIN_STEP_NONE;
	if (m != NULL && m->owner == c->handle)
		machineRelease(m);
	if (s != NULL && garmentAdvance(&c->garment, s->machineKind))
		brainEmit(out, cap, n, BRAIN_EV_GARMENT, c, c->target, 0, 0, 0);
	c->target = MACHINE_INVALID;
}

/* The walk out, whoever started it: the errand's LEAVE step or the abandon
 * path. Watched through the customer's own leaveJob handle so both start
 * points converge on one ending — and a door that cannot be walked fails the
 * job, which is how the customer leaves where it stands (still honest). */
static void brainDriveLeave(SimCustomer *c, Agent *a, BrainEvent *out, int cap,
			    int *n)
{
	Job *j;

	if (c->leaveJob == ERRAND_INVALID)
		return;
	j = boardJob(&a->personal, c->leaveJob);
	if (j == NULL || j->state == JOB_DONE || j->state == JOB_FAILED) {
		c->leaveJob = ERRAND_INVALID;
		brainEmit(out, cap, n, BRAIN_EV_DEPART, c, MACHINE_INVALID, 0, 0,
			  0);
		c->wantDespawn = true;
	}
}

/* Blocked time is what satisfaction is for: the spec's tier-1 reading is that
 * being blocked is an inconvenience, and a want that never moved would be a
 * counter in disguise. The decay is written by the WORLD (need.h is explicit
 * that influences write needs; this is one) on top of the role decay
 * agentsDecay already applies. */
/* Called from inside `if (errandBlocked(e))` — the blocked-ness is the CALLER's
 * decision (the same test decides whether blockedFor advances), so repeating it
 * here would be a branch that cannot be false. That is also why this function
 * needs no errand of its own: it writes a WANT, and the errand was only ever
 * the reason to write it. */
static void brainDecaySatisfaction(Agent *a, float dt)
{
	float v = a->needs.value[NEED_SATISFACTION] - ERRAND_BLOCKED_DECAY * dt;
	a->needs.value[NEED_SATISFACTION] = (v > 0.0f) ? v : 0.0f;
}

/* THE WALK OUT, and which half of it decides. Blocked time is the patience
 * budget (ERRAND_LEAVE_THRESHOLD_WINDOW) and satisfaction is the necessary
 * condition (ERRAND_LEAVE_THRESHOLD) — but under the framework's current need
 * arithmetic the want sits at the floor for every agent, so today the WINDOW
 * is what ends a wait and the threshold is a guard rail it cannot open. Both
 * are checked, both are named in errand.h, and neither is pretended to be
 * more load-bearing than it is: read agents/errand.h's note on
 * ERRAND_LEAVE_THRESHOLD before changing either half. */
/* THE WINDOW ALONE DECIDES, and the errand's state is not part of it — not
 * because the caller guarantees ACTIVE (it does NOT: the advance loop breaks
 * on a terminal errand and carries on to this call), but because a terminal
 * errand can never satisfy the rule: errandBlocked() is false for anything
 * that is not ERRAND_ACTIVE (errand.c), so the blocked clock is reset to zero
 * one statement earlier and `0.0f < ERRAND_LEAVE_THRESHOLD_WINDOW` answers
 * false here. The state check that used to stand above it was therefore
 * redundant rather than unreachable, which is the reason it was removed. */
static bool brainShouldLeave(const Agent *a, const Errand *e)
{
	if (e->blockedFor < ERRAND_LEAVE_THRESHOLD_WINDOW)
		return false;
	return a->needs.value[NEED_SATISFACTION] < ERRAND_LEAVE_THRESHOLD;
}

/* The step wants a machine this errand has not been given one for. Its one
 * caller has already returned on a NULL step, so there is no NULL arm here. */
static bool brainStepNeedsMachine(const ErrandStep *s)
{
	return s->kind == STEP_JOB &&
	       (s->jobKind == JOB_USE_MACHINE || s->jobKind == JOB_COLLECT);
}

/* SUPPLY THE CURRENT STEP'S TARGET from the world. An unbound step enqueues
 * nothing at all (errand.c), so this is the only place a machine is ever
 * chosen, the only place the door is bound, and the only place the bounded
 * retry countdown runs. Called once per errandAdvance, because each of those
 * may be looking at a different step.
 *
 * IT ALSO UNTIES A BINDING THAT IS RIGHT FOR NOBODY. Steps share a binding
 * deliberately — the collect step collects from the machine the gate was
 * waiting on — which means a step can also inherit a binding meant for a
 * different step altogether: the second USE_MACHINE step inherits the first
 * one's WASHER, and the gate inherits the wait spot. Binding something the
 * wrong kind of machine is the one mistake that would quietly wash the wrong
 * thing, so the kind is checked here rather than trusted. */
static void brainSupplyTarget(SimWorld *w, SimCustomer *c, Agent *a, Errand *e,
			      Entity *ent, float dt, BrainEvent *out, int cap,
			      int *n)
{
	const ErrandStep *s = brainStepOf(e);
	const Machine *bound;

	if (s == NULL)
		return;

	if (s->kind == STEP_JOB && s->jobKind == JOB_LEAVE &&
	    e->targetTile == ERRAND_INVALID) {
		/* The door is the world's business, so the driver binds it; the
		 * catalogue row says "leaving", not "leaving through THAT
		 * door". */
		errandBindTarget(e, ERRAND_TARGET_PLACE,
				 JOB_TILE_PACK(w->exitX, w->exitZ),
				 JOB_TILE_PACK(w->exitX, w->exitZ));
	}

	if (s->kind == STEP_JOB &&
	    (s->jobKind == JOB_USE_MACHINE || s->jobKind == JOB_COLLECT)) {
		if (s->jobKind == JOB_COLLECT &&
		    e->targetKind != ERRAND_TARGET_MACHINE &&
		    c->target != MACHINE_INVALID) {
			/* The collect step collects from the machine the gate was
			 * waiting on, and the gate's binding is the WAIT SPOT. */
			const Machine *m = machineGetConst(&w->machines,
							   c->target);

			if (m != NULL)
				errandBindTarget(e, ERRAND_TARGET_MACHINE,
						 c->target,
						 JOB_TILE_PACK(m->tileX,
							       m->tileZ));
		}
		bound = (e->targetKind == ERRAND_TARGET_MACHINE)
				? machineGetConst(&w->machines, e->targetId)
				: NULL;
		if (bound != NULL && bound->kind != s->machineKind)
			errandBindTarget(e, ERRAND_TARGET_NONE, ERRAND_INVALID,
					 ERRAND_INVALID);
		/* A USE_MACHINE step must also find its machine FREE — unless it
		 * has already claimed it, which is what BRAIN_STEP_PAY says:
		 * before the claim the binding is a wish, and after it the claim
		 * is the machine's own state, not something to re-decide. */
		bound = (e->targetKind == ERRAND_TARGET_MACHINE)
				? machineGetConst(&w->machines, e->targetId)
				: NULL;
		if (bound != NULL && s->jobKind == JOB_USE_MACHINE &&
		    bound->state != MACHINE_STATE_FREE &&
		    c->actStep == BRAIN_STEP_PAY)
			errandBindTarget(e, ERRAND_TARGET_NONE, ERRAND_INVALID,
					 ERRAND_INVALID);
	}

	if (!brainStepNeedsMachine(s) || e->targetTile != ERRAND_INVALID) {
		if (e->targetTile != ERRAND_INVALID) {
			c->waitRetries = 0;
			c->timer = 0.0f;
		}
		return;
	}
	if (s->jobKind == JOB_COLLECT) {
		/* A collect step with no machine to collect from has nothing
		 * left to do: the wash it was going to pick up is gone, and the
		 * honest move is to wash again. */
		errandRewindToJob(e, JOB_USE_MACHINE);
		s = brainStepOf(e);
		if (s == NULL || e->targetTile != ERRAND_INVALID)
			return;
	}
	if (c->timer > 0.0f) {
		/* Still inside the bounded no-candidate wait: the world has not
		 * been asked again yet, and the errand's blocked-time clock is
		 * running meanwhile. */
		c->timer -= dt;
		return;
	}
	(void)brainSelectMachine(w, c, ent, e, out, cap, n);
	if (c->waitRetries > BRAIN_MAX_WAIT_RETRIES)
		brainAbandonErrand(w, c, a, e, out, cap, n);
}

/* One customer, one tick. */
static void brainDrive(SimWorld *w, SimCustomer *c, float dt, BrainEvent *out,
		       int cap, int *n)
{
	Agent *a;
	Errand *e;
	Entity *ent;
	Job *j;
	int guard;

	ent = entityGet(&w->entities, c->handle);
	a = agentGet(&w->agents, c->agent);
	if (a == NULL || ent == NULL) {
		/* THE BODY (OR THE MIND) DIED UNDER US. The record is
		 * reclaimed, and anything it was still holding goes back first:
		 * a machine whose owner has been deleted is claimed for ever,
		 * and nothing in the registry would ever free it. The release
		 * reads the CUSTOMER, so it runs whether or not there is still a
		 * mind — and a mind that has gone has no errand to clear, which
		 * is the whole reason for the test: an agent destroyed while it
		 * holds a machine (pinned by
		 * test_the_mind_vanishing_releases_the_machine). */
		brainReplan(w, c, (a != NULL) ? errandActive(a) : NULL, out,
			    cap, n, BRAIN_EV_REPLAN);
		c->wantDespawn = true;
		return;
	}
	/* Already walking out: nothing else matters, the errand is terminal. */
	if (c->leaveJob != ERRAND_INVALID) {
		brainDriveLeave(c, a, out, cap, n);
		return;
	}
	e = errandActive(a);
	if (e == NULL) {
		/* No live errand. Either it finished at its LEAVE step, or it
		 * ended some other way (a template whose steps ran out). Either
		 * way this customer has nowhere left to be, and the reported
		 * flag keeps it from repeating the announcement every tick. */
		if (!c->reported) {
			c->reported = true;
			/* THROUGH THE RELEASE, like every other way out. An errand
			 * can go terminal while its step still holds a machine
			 * (a job it is standing on is retired behind its back),
			 * and the customer is walking out of the building — so
			 * whatever it holds goes back on the way to the door. */
			brainReplan(w, c, NULL, out, cap, n, BRAIN_EV_REPLAN);
			brainGoToExit(w, c, a, out, cap, n);
		}
		brainDriveLeave(c, a, out, cap, n);
		return;
	}
	c->leg = brainLegOfStep(e);
	/* A WAKE THE CURRENT LEG IS NOT WAITING ON IS STALE. It named a machine
	 * phase that has already been dealt with, and parking it would let it
	 * fire in some later leg. (A gate's own wake is deliberately NOT
	 * dropped: it can arrive while the owner is still walking to the wait
	 * spot, and it is delivered when the wait job reaches its phase.) */
	if (c->pendingWake != -1 && c->leg != BRAIN_LEG_GATE)
		c->pendingWake = -1;
	/* Entering a USE_MACHINE step starts its sub-step at "claim and pay have
	 * not happened yet". The sub-step lives on the customer rather than on
	 * the errand because the CLAIM is the world's, and the same leg re-run
	 * after a failure must pay again. */
	if (c->leg == BRAIN_LEG_LOAD && c->actStep == BRAIN_STEP_NONE)
		c->actStep = BRAIN_STEP_PAY;

	/* THE WORLD'S EFFECTS on the step in flight, before the errand is
	 * advanced: a job that has just ended is still readable here, and the
	 * effects it needs (release the machine, move the garment) must happen
	 * while the errand is still on that step. */
	j = brainStepJob(a, e);
	switch (c->leg) {
	case BRAIN_LEG_LOAD:
		brainDriveLoad(w, c, a, e, j, ent, out, cap, n);
		break;
	case BRAIN_LEG_GATE:
		brainDriveGate(w, c, e, j, out, cap, n);
		break;
	case BRAIN_LEG_COLLECT:
		brainDriveCollect(w, c, e, j, out, cap, n);
		break;
	case BRAIN_LEG_LEAVE:
		/* The errand's own LEAVE step enqueues its own job; the walk
		 * out is the same job reached from the abandon path. Watch
		 * whichever one this customer has — so that the step's job and
		 * the abandon path's job converge on ONE ending, announced
		 * once. */
		if (e->jobHandle != ERRAND_INVALID &&
		    c->leaveJob == ERRAND_INVALID) {
			c->leaveJob = e->jobHandle;
			brainEmit(out, cap, n, BRAIN_EV_LEAVE_BEGIN, c,
				  MACHINE_INVALID, 0, 0, 0);
		}
		brainDriveLeave(c, a, out, cap, n);
		return;
	default:
		break;
	}
	if (e->state != ERRAND_ACTIVE) {
		brainDriveLeave(c, a, out, cap, n);
		return;
	}

	/* THE ERRAND'S OWN ADVANCE, as many times as it keeps doing something.
	 *
	 * One call advances one thing, which is what makes the errand layer
	 * simple — but a driver that called it once would leave a one-frame hole
	 * between two steps: the finished job is retired, the next step's job
	 * has not been enqueued yet, and agentsUpdate's fill generator fires on
	 * EXACTLY that condition (an empty board). The filler it invented would
	 * then out-band the errand's next job and the customer would nap through
	 * their own wash. So the driver keeps going until the errand stops
	 * making progress, and the loop is bounded by the template's own step
	 * count: a step that has just enqueued a job is already doing nothing
	 * further this tick (the job cannot end again before it has run), so the
	 * bound is never the thing that stops it. */
	for (guard = 0; guard < ERRAND_MAX_STEPS + 2; guard++) {
		int step0 = e->step;
		int job0 = e->jobHandle;

		brainSupplyTarget(w, c, a, e, ent, dt, out, cap, n);
		if (e->state != ERRAND_ACTIVE)
			break;
		errandAdvance(e, a, w->now, &a->personal);
		if (e->state != ERRAND_ACTIVE)
			break;
		if (e->step == step0 && e->jobHandle == job0)
			break;		/* the errand is waiting on the world */
	}

	/* The errand's own blocked-time clock, and the want being blocked
	 * decays. A step that is moving resets it: patience is spent waiting,
	 * not working. */
	if (errandBlocked(e)) {
		e->blockedFor += dt;
		brainDecaySatisfaction(a, dt);
	} else {
		e->blockedFor = 0.0f;
	}
	/* ANNOUNCE THE FILL, ONCE, WHILE BLOCKED. This is the spec's "an agent
	 * does secondary activities when blocked" made visible: agentsUpdate
	 * generates the fill itself and reports nothing, so without this the
	 * most interesting thing an idle customer does — standing beside a
	 * washer somebody else is using, doing a coffee break — leaves no
	 * trace at all.
	 *
	 * It is announced rather than repeated because a fill can run for
	 * fifteen seconds at sixty frames a second, and the flag clears as
	 * soon as the agent is back on the errand's own work (or the errand
	 * stops being blocked), so a second fill on the same errand is a
	 * second event, not a silence. The engaged job is the FILL exactly when
	 * it is not the job the errand enqueued. */
	if (a->engagedJob != JOB_INVALID && !a->engagedShared &&
	    a->engagedJob != e->jobHandle) {
		const Job *fj = boardJob(&a->personal, a->engagedJob);

		if (fj != NULL && !c->reportedFill) {
			c->reportedFill = true;
			brainEmit(out, cap, n, BRAIN_EV_FILL, c, MACHINE_INVALID,
				  fj->kind, 0, 0);
			if (out != NULL && cap > 0 && n != NULL && *n > 0)
				out[*n - 1].band = fj->band;
		}
	} else {
		c->reportedFill = false;
	}
	if (brainShouldLeave(a, e))
		brainAbandonErrand(w, c, a, e, out, cap, n);
}

void simWorldInit(SimWorld *w, const Voxmap *map, int doorX, int doorZ,
		  int exitX, int exitZ, int waitX, int waitZ)
{
	if (w == NULL)
		return;
	entitiesInit(&w->entities, map);
	machinesInit(&w->machines);
	agentsInit(&w->agents);
	boardInit(&w->boards.shared, true);
	w->customerCount = 0;
	w->map = map;
	w->doorX = doorX;
	w->doorZ = doorZ;
	w->exitX = exitX;
	w->exitZ = exitZ;
	w->waitX = waitX;
	w->waitZ = waitZ;
	w->spawnEnabled = false;
	w->spawnInterval = BRAIN_SPAWN_INTERVAL;
	w->spawnTimer = 0.0f;
	w->spawnCap = BRAIN_SPAWN_CAP;
	w->spawnCursor = 0;
	w->nextId = 0;
	w->now = 0.0f;
	w->wakeCount = 0;
	w->customerWidth = 1.0f;
	w->customerHeight = 1.5f;
	w->customerMaterial = -1;
	for (int i = 0; i < SEL_POLICY_COUNT; i++)
		w->policyTint[i] = 0xffffffffu;
	for (int i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		w->customers[i] = (SimCustomer){ 0 };
		w->customers[i].handle = ENTITY_INVALID;
		w->customers[i].agent = AGENT_INVALID;
		w->customers[i].leaveJob = ERRAND_INVALID;
	}
	selWorldInit(&w->select, &w->machines);
}

void simSetSpawner(SimWorld *w, bool enabled, float interval, int cap)
{
	if (w == NULL)
		return;
	w->spawnEnabled = enabled;
	w->spawnInterval = interval > 0.0f ? interval : BRAIN_SPAWN_INTERVAL;
	w->spawnCap = cap > 0 ? cap : BRAIN_SPAWN_CAP;
	if (cap > BRAIN_MAX_CUSTOMERS)
		w->spawnCap = BRAIN_MAX_CUSTOMERS;
	w->spawnTimer = 0.0f;
}

EntityHandle simSpawnCustomer(SimWorld *w, int policy, int money,
			      BrainEvent *out, int cap, int *nOut)
{
	SimCustomer *c = NULL;
	AgentHandle ah;
	EntityHandle h;
	Entity *e;
	int slot = -1;
	int i;

	if (w == NULL)
		return ENTITY_INVALID;
	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++)
		if (!brainSlotUsed(&w->customers[i])) {
			slot = i;
			break;
		}
	if (slot < 0)
		return ENTITY_INVALID;
	h = entityCreate(&w->entities);
	if (h == ENTITY_INVALID)
		return ENTITY_INVALID;
	e = entityGet(&w->entities, h);
	if (e == NULL || !entityPlace(e, w->doorX, w->doorZ)) {
		entityDestroy(&w->entities, h);
		return ENTITY_INVALID;
	}
	/* THE MIND BEFORE THE FLOW: a customer is an agent, so it adopts an
	 * errand by need before its first tick rather than being told what to
	 * do by a switch. The adoption is what puts its first job on a board. */
	ah = agentCreate(&w->agents, ROLE_CUSTOMER);
	if (ah == AGENT_INVALID) {
		entityDestroy(&w->entities, h);
		return ENTITY_INVALID;
	}
	e->speed = BRAIN_CUSTOMER_SPEED;
	e->animated = true;
	e->easing = EASE_IN_OUT;
	e->money = money;
	c = &w->customers[slot];
	*c = (SimCustomer){ 0 };
	c->handle = h;
	c->agent = ah;
	c->id = w->nextId++;
	c->policy = selPolicyValid(policy) ? policy : SEL_POLICY_DEFAULT;
	c->garment = GARMENT_DIRTY;
	entitySetSprite(e, w->customerWidth, w->customerHeight,
			w->policyTint[c->policy], w->customerMaterial);
	c->target = MACHINE_INVALID;
	c->pendingWake = -1;
	c->waitRetries = 0;
	c->timer = 0.0f;
	c->gateDeadline = 0.0f;
	c->leaveJob = ERRAND_INVALID;
	c->wantDespawn = false;
	c->abandoned = false;
	c->reported = false;
	c->reportedFill = false;
	{
		Agent *a = agentGet(&w->agents, ah);

		a->entity = h;
	}
	w->customerCount++;
	brainEmit(out, cap, nOut, BRAIN_EV_SPAWN, c, MACHINE_INVALID, money, 0,
		  0);
	{
		Agent *a = agentGet(&w->agents, ah);
		int tmpl = errandAdoptByNeed(a, w->now);
		const ErrandTemplate *t;

		if (tmpl != ERRAND_INVALID) {
			int band = 0;

			t = errandsTemplate(tmpl);
			/* The band is the want that CHOSE it, read back off the
			 * agent's own needs, so the log line says why and not
			 * merely what. */
			for (i = 0; i < NEED_COUNT; i++) {
				int b;

				if (t == NULL || !(t->serves[i] > 0.0f))
					continue;
				b = (int)((float)wantUrgency(&a->needs, i) *
					  t->serves[i] + 0.5f);
				if (b > band)
					band = b;
			}
			if (out != NULL && cap > 0 && nOut != NULL &&
			    *nOut < cap) {
				brainEmit(out, cap, nOut, BRAIN_EV_ERRAND_ADOPT,
					  c, MACHINE_INVALID, 0, 0, 0);
				out[*nOut - 1].tmpl = tmpl;
				out[*nOut - 1].band = band;
			}
		}
	}
	/* PRIME THE ERRAND. The adoption has put a goal on the customer's
	 * record but nothing on its board yet, and a brand-new agent on an
	 * empty board is exactly the case agentsUpdate fills — so without this
	 * the customer would spend its first 1.5 s (the dwell) on a filler
	 * before its own errand could be arbitrated against it. One drive
	 * with dt = 0: the errand chooses its first machine and enqueues its
	 * first job, and nothing is timed. */
	brainDrive(w, c, 0.0f, out, cap, nOut);
	return h;
}

bool simBreakMachine(SimWorld *w, MachineHandle h)
{
	Machine *m;
	MachineWake wake;
	int i;

	if (w == NULL)
		return false;
	m = machineGet(&w->machines, h);
	if (m == NULL)
		return false;
	wake.machine = MACHINE_INVALID;
	wake.owner = MACHINE_NO_OWNER;
	wake.kind = MACHINE_WAKE_INTERRUPTED;
	if (!machineBreak(m, &wake))
		return false;
	if (wake.machine == MACHINE_INVALID || wake.owner < MACHINE_OWNER_MIN)
		return true;
	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		SimCustomer *c = &w->customers[i];

		if (!brainSlotUsed(c))
			continue;
		if (c->handle == wake.owner) {
			c->pendingWake = MACHINE_WAKE_INTERRUPTED;
			break;
		}
	}
	return true;
}

/* Hand a wake to the customer that owns the machine it names. */
static void brainRouteWake(SimWorld *w, const MachineWake *wake)
{
	int j;

	if (wake->machine == MACHINE_INVALID ||
	    wake->owner < MACHINE_OWNER_MIN)
		return;
	for (j = 0; j < BRAIN_MAX_CUSTOMERS; j++) {
		SimCustomer *c = &w->customers[j];

		if (!brainSlotUsed(c))
			continue;
		if (c->handle == wake->owner) {
			c->pendingWake = wake->kind;
			return;
		}
	}
}

int simUpdate(SimWorld *w, float dt, BrainEvent *out, int cap)
{
	int n = 0;
	int nw;
	int i;

	if (w == NULL)
		return 0;
	/* NaN-safe clamp, matching the entity / machine / agent walkers. */
	if (!(dt > 0.0f))
		dt = 0.0f;
	else if (dt > BRAIN_MAX_DT)
		dt = BRAIN_MAX_DT;
	w->now += dt;

	if (w->spawnEnabled) {
		w->spawnTimer -= dt;
		if (w->spawnTimer <= 0.0f) {
			if (simCustomerCount(w) < w->spawnCap) {
				int policy = w->spawnCursor % SEL_POLICY_COUNT;

				(void)simSpawnCustomer(w, policy,
						       BRAIN_START_MONEY, out,
						       cap, &n);
				w->spawnCursor++;
			}
			w->spawnTimer = w->spawnInterval;
		}
	}

	/* Reservations are swept against the world's own clock, which is
	 * accumulated dt and never a wall clock (agents/board.h). A RUNNING
	 * job's claim is not touched by this at all — only a claim still
	 * sitting in JOB_CLAIMED. */
	boardExpire(&w->boards.shared, w->now);
	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		Agent *a;

		if (!brainSlotUsed(&w->customers[i]))
			continue;
		a = agentGet(&w->agents, w->customers[i].agent);
		if (a != NULL)
			boardExpire(&a->personal, w->now);
	}

	nw = machinesUpdate(&w->machines, dt, w->wakes, BRAIN_MAX_WAKES);
	w->wakeCount = nw;
	for (i = 0; i < nw; i++)
		brainRouteWake(w, &w->wakes[i]);

	/* THE FRAMEWORK'S TICK: needs decay, the one switch decision, and the
	 * job lifecycle for every customer's errand step. The driver below
	 * reads what it left behind. */
	agentsUpdate(&w->agents, &w->entities, &w->boards, w->map, dt, NULL);

	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		SimCustomer *c = &w->customers[i];

		if (!brainSlotUsed(c))
			continue;
		brainDrive(w, c, dt, out, cap, &n);
	}

	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		SimCustomer *c = &w->customers[i];

		if (!brainSlotUsed(c) || !c->wantDespawn)
			continue;
		/* The mind goes with the body: a live agent whose body has left
		 * would keep decaying and keep its personal board. */
		agentDestroy(&w->agents, c->agent);
		entityDestroy(&w->entities, c->handle);
		*c = (SimCustomer){ 0 };
		c->handle = ENTITY_INVALID;
		c->agent = AGENT_INVALID;
		c->leaveJob = ERRAND_INVALID;
		w->customerCount--;
	}
	return n;
}

int simCustomerCount(const SimWorld *w)
{
	return w == NULL ? 0 : w->customerCount;
}

const SimCustomer *simCustomerFor(const SimWorld *w, EntityHandle h)
{
	int i;

	if (w == NULL || h == ENTITY_INVALID)
		return NULL;
	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++)
		if (brainSlotUsed(&w->customers[i]) &&
		    w->customers[i].handle == h)
			return &w->customers[i];
	return NULL;
}

const Errand *simCustomerErrand(const SimWorld *w, EntityHandle h)
{
	const SimCustomer *c = simCustomerFor(w, h);
	AgentRegistry *r;
	Agent *a;

	if (c == NULL)
		return NULL;
	r = (AgentRegistry *)&w->agents;
	a = agentGet(r, c->agent);
	if (a == NULL)
		return NULL;
	return errandActive(a);
}

const char *brainEventName(int kind)
{
	switch (kind) {
	case BRAIN_EV_SPAWN:
		return "SPAWN";
	case BRAIN_EV_ERRAND_ADOPT:
		return "ERRAND_ADOPT";
	case BRAIN_EV_FILL:
		return "FILL";
	case BRAIN_EV_TASK_BEGIN:
		return "TASK_BEGIN";
	case BRAIN_EV_NO_CANDIDATE:
		return "NO_CANDIDATE";
	case BRAIN_EV_CLAIM_DENIED:
		return "CLAIM_DENIED";
	case BRAIN_EV_PAY:
		return "PAY";
	case BRAIN_EV_LOAD:
		return "LOAD";
	case BRAIN_EV_RUN_START:
		return "RUN_START";
	case BRAIN_EV_MACHINE_DONE:
		return "MACHINE_DONE";
	case BRAIN_EV_INTERRUPTED:
		return "INTERRUPTED";
	case BRAIN_EV_REPLAN:
		return "REPLAN";
	case BRAIN_EV_COLLECT:
		return "COLLECT";
	case BRAIN_EV_GARMENT:
		return "GARMENT";
	case BRAIN_EV_ABANDON:
		return "ABANDON";
	case BRAIN_EV_WAIT_TIMEOUT:
		return "WAIT_TIMEOUT";
	case BRAIN_EV_LEAVE_BEGIN:
		return "LEAVE_BEGIN";
	case BRAIN_EV_DEPART:
		return "DEPART";
	default:
		return "?";
	}
}
