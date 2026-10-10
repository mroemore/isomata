/*
 * The customer brain (see sim/brain.h).
 *
 * Pure glue over the T22 machine layer and the T23 plan / selection / garment
 * modules. No allocation, no SDL, no time source: the world advances only by
 * the caller's dt, so a fake-dt scenario is deterministic.
 *
 * EXECUTOR OWNERSHIP (stated plainly, because it is easy to over-claim): the
 * driver decides with each customer's OWN task-leg machine (`c->phase`, a
 * BrainPhase). Every customer also carries a T21 `Fsm`, and this file steps it
 * with the same events so its states stay coherent as a transition model — but
 * nothing here reads the FSM's state or result to choose what happens next.
 * Folding the driver onto the FSM (or retiring the unused FSM) is recorded
 * design debt in the T23 report.
 */

#include "sim/brain.h"

#include "ai/nav.h"

#include <stddef.h>

/* True when the entity has finished its whole route: no active segment and an
 * empty queue. (The `arrived` pulse is one update wide, so the glue reads the
 * durable state instead of the pulse.) */
static bool brainArrived(const Entity *e)
{
	return e != NULL && !e->moving && e->pathCount == 0;
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
	(*n)++;
}

/* Add `m` to the task instance's failure blacklist (idempotent). */
static void brainExclude(SimCustomer *c, MachineHandle m)
{
	int i;

	if (m == MACHINE_INVALID)
		return;
	for (i = 0; i < c->failedCount; i++)
		if (c->failed[i] == m)
			return;
	if (c->failedCount < BRAIN_MAX_FAILED)
		c->failed[c->failedCount++] = m;
}

/* Begin the LEAVE leg: path to the exit and switch to BRAIN_PH_LEAVE (or
 * despawn in place when the exit is not reachable, still honest). */
static void brainGoToExit(SimWorld *w, SimCustomer *c, Entity *e,
			  BrainEvent *out, int cap, int *n)
{
	brainEmit(out, cap, n, BRAIN_EV_LEAVE_BEGIN, c, MACHINE_INVALID, 0, 0,
		  0);
	if (entityPathTo(e, w->exitX, w->exitZ) < 0) {
		/* No route (void / unreachable): leave where we stand. */
		brainEmit(out, cap, n, BRAIN_EV_DEPART, c, MACHINE_INVALID, 0, 0,
			  0);
		c->wantDespawn = true;
		c->phase = BRAIN_PH_DONE;
		return;
	}
	c->phase = BRAIN_PH_LEAVE;
}

/* Give up the goal: mark abandoned, then leave (the load keeps its state). */
static void brainAbandon(SimWorld *w, SimCustomer *c, Entity *e,
			 BrainEvent *out, int cap, int *n)
{
	c->abandoned = true;
	brainEmit(out, cap, n, BRAIN_EV_ABANDON, c, c->target, 0, 0, 0);
	brainGoToExit(w, c, e, out, cap, n);
}

/* Start the next USE_MACHINE task leg: record the goal on the FSM, step START
 * (MOVE_TO) and issue a fresh A* route. Returns false when the route could not
 * be computed (the caller then re-plans). */
static bool brainStartMove(SimCustomer *c, Entity *e, const Machine *m)
{
	if (entityPathTo(e, m->tileX, m->tileZ) < 0)
		return false;
	fsmSetGoal(&c->fsm, m->tileX, m->tileZ);
	fsmStep(&c->fsm, FSM_EVENT_START);
	c->phase = BRAIN_PH_APPROACH;
	return true;
}

/* SELECT phase: pick the task's machine (or begin LEAVE), or wait/abandon. */
static void brainSelect(SimWorld *w, SimCustomer *c, Entity *e, float dt,
			BrainEvent *out, int cap, int *n)
{
	const PlanTask *task = planTaskAt(&c->plan, c->taskIndex);
	MachineHandle chosen;
	const Machine *m;
	int score;
	int distance;

	(void)dt;
	if (task == NULL) {
		/* A plan with no LEAVE (malformed): just leave. */
		brainGoToExit(w, c, e, out, cap, n);
		return;
	}
	if (task->kind == PLAN_TASK_LEAVE) {
		brainGoToExit(w, c, e, out, cap, n);
		return;
	}
	chosen = selChoose(&w->machines, e, &w->select, c->policy,
			   task->machineKind, c->failed, c->failedCount);
	if (chosen == MACHINE_INVALID) {
		if (c->waitRetries >= BRAIN_MAX_WAIT_RETRIES) {
			brainAbandon(w, c, e, out, cap, n);
			return;
		}
		c->waitRetries++;
		brainEmit(out, cap, n, BRAIN_EV_NO_CANDIDATE, c, MACHINE_INVALID,
			  0, 0, 0);
		c->phase = BRAIN_PH_NO_CANDIDATE;
		c->timer = BRAIN_WAIT_RETRY_SECS;
		return;
	}
	m = machineGetConst(&w->machines, chosen);
	if (m == NULL) {		/* raced away: retry selection */
		brainExclude(c, chosen);
		return;
	}
	c->target = chosen;
	score = selScore(e, m, &w->select, c->policy);
	distance = (m->tileX > e->tileX ? m->tileX - e->tileX
					: e->tileX - m->tileX) +
		   (m->tileZ > e->tileZ ? m->tileZ - e->tileZ
					: e->tileZ - m->tileZ);
	brainEmit(out, cap, n, BRAIN_EV_TASK_BEGIN, c, chosen, m->fee, score,
		  distance);
	if (!brainStartMove(c, e, m)) {
		brainExclude(c, chosen);
		c->phase = BRAIN_PH_SELECT;
	}
}

/* ACT phase: the load sub-steps (pay, load) or the collect sub-step. */
static void brainAct(SimWorld *w, SimCustomer *c, Entity *e, float dt,
		     BrainEvent *out, int cap, int *n)
{
	Machine *m = machineGet(&w->machines, c->target);
	const PlanTask *task = planTaskAt(&c->plan, c->taskIndex);

	if (m == NULL) {		/* machine vanished mid-task */
		brainExclude(c, c->target);
		c->phase = BRAIN_PH_SELECT;
		return;
	}
	if (c->leg == BRAIN_LEG_COLLECT) {
		c->timer -= dt;
		if (c->timer > 0.0f)
			return;
		machineRelease(m);
		if (task != NULL && garmentAdvance(&c->garment,
						   task->machineKind))
			brainEmit(out, cap, n, BRAIN_EV_GARMENT, c, c->target, 0,
				  0, 0);
		/* Close the leg through the FSM's zero-length end wait. */
		c->fsm.waitReason = BRAIN_WAIT_TASK_END;
		c->fsm.waitSeconds = 0.0f;
		c->fsm.step = c->actStep;
		fsmStep(&c->fsm, FSM_EVENT_PHASE_DONE);
		c->phase = BRAIN_PH_TASK_END;
		c->timer = 0.0f;
		return;
	}
	/* LOAD leg. */
	if (c->actStep == BRAIN_STEP_PAY) {
		if (e->money < m->fee) {
			machineRelease(m);	/* we had claimed it */
			brainAbandon(w, c, e, out, cap, n);
			return;
		}
		e->money -= m->fee;
		brainEmit(out, cap, n, BRAIN_EV_PAY, c, c->target, m->fee, 0, 0);
		c->actStep = BRAIN_STEP_LOAD;
		brainEmit(out, cap, n, BRAIN_EV_LOAD, c, c->target, 0, 0, 0);
		c->timer = BRAIN_LOAD_SECS;
		return;
	}
	/* LOAD in place. */
	c->timer -= dt;
	if (c->timer > 0.0f)
		return;
	machineStartRun(m);
	brainEmit(out, cap, n, BRAIN_EV_RUN_START, c, c->target, 0, 0, 0);
	c->fsm.waitReason = BRAIN_WAIT_MACHINE;
	c->fsm.waitSeconds = m->runLeft + BRAIN_MACHINE_WAIT_SLACK;
	c->fsm.step = c->actStep;
	fsmStep(&c->fsm, FSM_EVENT_PHASE_DONE);	/* ACTING -> WAITING */
	c->waitBudget = m->runLeft + BRAIN_MACHINE_WAIT_SLACK;
	c->phase = BRAIN_PH_RUN_WAIT;
	/* Walk away while the machine runs ("owner away"). */
	(void)entityPathTo(e, w->waitX, w->waitZ);
}

/* APPROACH phase: on arrival, claim (LOAD leg) or begin collect (COLLECT leg). */
static void brainApproach(SimWorld *w, SimCustomer *c, Entity *e,
			  BrainEvent *out, int cap, int *n)
{
	Machine *m = machineGet(&w->machines, c->target);

	if (m == NULL) {
		brainExclude(c, c->target);
		c->phase = BRAIN_PH_SELECT;
		return;
	}
	if (!brainArrived(e))
		return;
	if (c->leg == BRAIN_LEG_COLLECT) {
		/* We already own this machine (it is DONE). If it is gone or
		 * was broken, re-plan instead of collecting. */
		if (m->owner != c->handle || m->state == MACHINE_STATE_BROKEN) {
			brainExclude(c, c->target);
			c->phase = BRAIN_PH_SELECT;
			return;
		}
		fsmStep(&c->fsm, FSM_EVENT_ARRIVED);	/* -> ACTING */
		c->actStep = BRAIN_STEP_COLLECT;
		c->timer = BRAIN_COLLECT_SECS;
		brainEmit(out, cap, n, BRAIN_EV_COLLECT, c, c->target, 0, 0, 0);
		c->phase = BRAIN_PH_ACT;
		return;
	}
	/* LOAD leg: claim the machine we walked to. A transient refusal
	 * (someone else won the race) does NOT blacklist the machine: the FREE
	 * filter already removes it while it is occupied, and the bounded wait
	 * lets this customer take it once it frees (the contention oracle). */
	if (!machineClaim(m, c->handle)) {
		brainEmit(out, cap, n, BRAIN_EV_CLAIM_DENIED, c, c->target, 0,
			  0, 0);
		fsmStep(&c->fsm, FSM_EVENT_DENIED);	/* -> DONE */
		c->phase = BRAIN_PH_SELECT;
		return;
	}
	fsmStep(&c->fsm, FSM_EVENT_ARRIVED);	/* -> ACTING */
	c->actStep = BRAIN_STEP_PAY;
	c->phase = BRAIN_PH_ACT;
}

/* RUN_WAIT phase: wait for the machine's wake (bounded). */
static void brainRunWait(SimWorld *w, SimCustomer *c, Entity *e, float dt,
			 BrainEvent *out, int cap, int *n)
{
	Machine *m = machineGet(&w->machines, c->target);

	if (c->pendingWake == MACHINE_WAKE_INTERRUPTED) {
		brainEmit(out, cap, n, BRAIN_EV_INTERRUPTED, c, c->target, 0, 0,
			  0);
		fsmStep(&c->fsm, FSM_EVENT_INTERRUPTED);	/* -> DONE */
		brainExclude(c, c->target);
		c->pendingWake = -1;
		c->phase = BRAIN_PH_SELECT;	/* garment untouched */
		return;
	}
	if (c->pendingWake == MACHINE_WAKE_PHASE_DONE) {
		brainEmit(out, cap, n, BRAIN_EV_MACHINE_DONE, c, c->target, 0, 0,
			  0);
		fsmStep(&c->fsm, FSM_EVENT_PHASE_DONE);	/* -> DONE */
		c->pendingWake = -1;
		if (m == NULL || m->owner != c->handle) {
			brainExclude(c, c->target);
			c->phase = BRAIN_PH_SELECT;
			return;
		}
		/* Begin the collect leg: re-target the machine. */
		c->leg = BRAIN_LEG_COLLECT;
		if (!brainStartMove(c, e, m)) {
			brainExclude(c, c->target);
			c->phase = BRAIN_PH_SELECT;
		}
		return;
	}
	c->waitBudget -= dt;
	if (c->waitBudget <= 0.0f) {
		brainEmit(out, cap, n, BRAIN_EV_WAIT_TIMEOUT, c, c->target, 0, 0,
			  0);
		if (m != NULL)
			machineRelease(m);
		fsmStep(&c->fsm, FSM_EVENT_TIMEOUT);	/* -> DONE */
		brainExclude(c, c->target);
		c->phase = BRAIN_PH_SELECT;
	}
}

/* Step one customer's brain by dt. */
static void brainStep(SimWorld *w, SimCustomer *c, float dt, BrainEvent *out,
		      int cap, int *n)
{
	Entity *e = entityGet(&w->entities, c->handle);

	if (e == NULL) {		/* handle died under us: drop the record */
		c->wantDespawn = true;
		c->phase = BRAIN_PH_DONE;
		return;
	}
	switch (c->phase) {
	case BRAIN_PH_SELECT:
		brainSelect(w, c, e, dt, out, cap, n);
		break;
	case BRAIN_PH_NO_CANDIDATE:
		c->timer -= dt;
		if (c->timer <= 0.0f)
			c->phase = BRAIN_PH_SELECT;
		break;
	case BRAIN_PH_APPROACH:
		brainApproach(w, c, e, out, cap, n);
		break;
	case BRAIN_PH_ACT:
		brainAct(w, c, e, dt, out, cap, n);
		break;
	case BRAIN_PH_RUN_WAIT:
		brainRunWait(w, c, e, dt, out, cap, n);
		break;
	case BRAIN_PH_TASK_END:
		c->timer -= dt;
		if (c->timer <= 0.0f) {
			fsmStep(&c->fsm, FSM_EVENT_PHASE_DONE);	/* -> DONE */
			c->taskIndex++;
			c->failedCount = 0;
			c->waitRetries = 0;
			c->leg = BRAIN_LEG_LOAD;
			c->phase = BRAIN_PH_SELECT;
		}
		break;
	case BRAIN_PH_LEAVE:
		if (brainArrived(e)) {
			brainEmit(out, cap, n, BRAIN_EV_DEPART, c, MACHINE_INVALID,
				  0, 0, 0);
			c->wantDespawn = true;
			c->phase = BRAIN_PH_DONE;
		}
		break;
	case BRAIN_PH_DONE:
		c->wantDespawn = true;
		break;
	default:
		break;
	}
}

void simWorldInit(SimWorld *w, const Voxmap *map, int doorX, int doorZ,
		  int exitX, int exitZ, int waitX, int waitZ)
{
	if (w == NULL)
		return;
	entitiesInit(&w->entities, map);
	machinesInit(&w->machines);
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
	w->wakeCount = 0;
	w->customerWidth = 1.0f;
	w->customerHeight = 1.5f;
	w->customerMaterial = -1;
	for (int i = 0; i < SEL_POLICY_COUNT; i++)
		w->policyTint[i] = 0xffffffffu;
	for (int i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		w->customers[i] = (SimCustomer){ 0 };
		w->customers[i].phase = BRAIN_PH_INACTIVE;
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
	EntityHandle h;
	Entity *e;
	int slot = -1;
	int i;

	if (w == NULL)
		return ENTITY_INVALID;
	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++)
		if (w->customers[i].phase == BRAIN_PH_INACTIVE) {
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
	e->speed = BRAIN_CUSTOMER_SPEED;
	e->animated = true;
	e->easing = EASE_IN_OUT;
	e->money = money;
	c = &w->customers[slot];
	*c = (SimCustomer){ 0 };
	c->handle = h;
	fsmInit(&c->fsm);
	c->id = w->nextId++;
	c->policy = selPolicyValid(policy) ? policy : SEL_POLICY_DEFAULT;
	c->garment = GARMENT_DIRTY;
	entitySetSprite(e, w->customerWidth, w->customerHeight,
			w->policyTint[c->policy], w->customerMaterial);
	c->phase = BRAIN_PH_SELECT;
	c->taskIndex = 0;
	c->leg = BRAIN_LEG_LOAD;
	c->target = MACHINE_INVALID;
	c->pendingWake = -1;
	c->failedCount = 0;
	c->waitRetries = 0;
	c->wantDespawn = false;
	c->abandoned = false;
	if (!planExpand(PLAN_GOAL_LEAVE_WITH_CLEAN_DRY, &c->plan)) {
		*c = (SimCustomer){ 0 };
		c->phase = BRAIN_PH_INACTIVE;
		entityDestroy(&w->entities, h);
		return ENTITY_INVALID;
	}
	w->customerCount++;
	brainEmit(out, cap, nOut, BRAIN_EV_SPAWN, c, MACHINE_INVALID, money, 0,
		  0);
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

		if (c->phase == BRAIN_PH_INACTIVE)
			continue;
		if (c->handle == wake.owner) {
			c->pendingWake = MACHINE_WAKE_INTERRUPTED;
			break;
		}
	}
	return true;
}

int simUpdate(SimWorld *w, float dt, BrainEvent *out, int cap)
{
	int n = 0;
	int nw;
	int i;

	if (w == NULL)
		return 0;
	/* NaN-safe clamp, matching the entity / machine walkers. */
	if (!(dt > 0.0f))
		dt = 0.0f;
	else if (dt > BRAIN_MAX_DT)
		dt = BRAIN_MAX_DT;

	if (w->spawnEnabled) {
		w->spawnTimer -= dt;
		if (w->spawnTimer <= 0.0f) {
			if (simCustomerCount(w) < w->spawnCap) {
				int policy = w->spawnCursor % SEL_POLICY_COUNT;

				(void)simSpawnCustomer(w, policy, BRAIN_START_MONEY,
						       out, cap, &n);
				w->spawnCursor++;
			}
			w->spawnTimer = w->spawnInterval;
		}
	}

	nw = machinesUpdate(&w->machines, dt, w->wakes, BRAIN_MAX_WAKES);
	w->wakeCount = nw;

	for (i = 0; i < nw; i++) {
		int j;

		if (w->wakes[i].machine == MACHINE_INVALID ||
		    w->wakes[i].owner < MACHINE_OWNER_MIN)
			continue;
		for (j = 0; j < BRAIN_MAX_CUSTOMERS; j++) {
			SimCustomer *c = &w->customers[j];

			if (c->phase == BRAIN_PH_INACTIVE)
				continue;
			if (c->handle == w->wakes[i].owner) {
				c->pendingWake = w->wakes[i].kind;
				break;
			}
		}
	}

	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		SimCustomer *c = &w->customers[i];

		if (c->phase == BRAIN_PH_INACTIVE)
			continue;
		brainStep(w, c, dt, out, cap, &n);
		/* One wake per customer per frame: consume it now so a stale
		 * wake can never leak into a later phase. */
		c->pendingWake = -1;
	}

	for (i = 0; i < BRAIN_MAX_CUSTOMERS; i++) {
		SimCustomer *c = &w->customers[i];

		if (c->phase == BRAIN_PH_INACTIVE || !c->wantDespawn)
			continue;
		entityDestroy(&w->entities, c->handle);
		*c = (SimCustomer){ 0 };
		c->phase = BRAIN_PH_INACTIVE;
		c->handle = ENTITY_INVALID;
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
		if (w->customers[i].phase != BRAIN_PH_INACTIVE &&
		    w->customers[i].handle == h)
			return &w->customers[i];
	return NULL;
}

const char *brainEventName(int kind)
{
	switch (kind) {
	case BRAIN_EV_SPAWN:
		return "SPAWN";
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
