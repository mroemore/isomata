/*
 * The agent record + flat-slot registry (see agents/agent.h for the model).
 *
 * Mirrors EntityRegistry in src/entities/entities.c on purpose: the same
 * fixed-capacity slots + alive[] + liveCount shape, the same handle guards,
 * the same refusal-without-a-diagnostic style at capacity (the caller reads
 * AGENT_INVALID; there is no logging in a pure module).
 *
 * Pure: no SDL, no allocation, no time source.
 */

#include "agents/agent.h"

#include <string.h>

/* The per-role curve table. A role is a ROW: adding one is a value here plus
 * an AgentRole constant, and no call site changes. Order matches Need. */
static const RoleProfile g_roleProfiles[ROLE_COUNT] = {
	/* CUSTOMER: the baseline. Everything decays at the base rate and every
	 * need counts as panicking below 0.15. */
	[ROLE_CUSTOMER] = {
		.decayMul = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		.panicThreshold = { 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f },
	},
	/* STAFF: a shift tires you (energy 1.5), and someone is always around
	 * plus the job itself supplies the sense of purpose (social and
	 * fulfilment 0.5). */
	[ROLE_STAFF] = {
		.decayMul = { 1.0f, 1.5f, 0.5f, 1.0f, 1.0f, 0.5f },
		.panicThreshold = { 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f },
	},
	/* CRITTER: needs little company (social 0.25), is not bothered by
	 * esteem at all (0.0 — it never decays, so it never panics), and being
	 * blocked annoys it half as much. It also trips earlier: 0.10. */
	[ROLE_CRITTER] = {
		.decayMul = { 1.0f, 1.0f, 0.25f, 0.5f, 0.0f, 1.0f },
		.panicThreshold = { 0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f },
	},
};

void agentsInit(AgentRegistry *r)
{
	if (r == NULL)
		return;
	/* Zeroes the slots, alive[] and liveCount as well as nextId, so a
	 * re-initialised registry is byte-identical to a fresh one. */
	memset(r, 0, sizeof(*r));
}

AgentHandle agentCreate(AgentRegistry *r, int role)
{
	int i;

	if (r == NULL)
		return AGENT_INVALID;
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a;

		if (r->alive[i])
			continue;
		a = &r->slots[i];
		/* Scrub first: a reused slot must never show the previous
		 * occupant's state, even for a field nobody set. */
		*a = (Agent){ 0 };
		a->id = r->nextId++;
		a->role = role;
		a->alive = true;
		needSetInit(&a->needs);
		a->panicMask = needPanicMask(&a->needs);
		a->entity = ENTITY_INVALID;
		a->personalBoard = -1;
		a->engagedJob = -1;
		a->dwellLeft = 0.0f;
		r->alive[i] = true;
		r->liveCount++;
		return i;
	}
	return AGENT_INVALID;	/* full */
}

Agent *agentGet(AgentRegistry *r, AgentHandle h)
{
	if (r == NULL || h < 0 || h >= AGENT_MAX || !r->alive[h])
		return NULL;
	return &r->slots[h];
}

void agentDestroy(AgentRegistry *r, AgentHandle h)
{
	if (r == NULL || h < 0 || h >= AGENT_MAX || !r->alive[h])
		return;
	r->alive[h] = false;
	r->slots[h] = (Agent){ 0 };
	r->liveCount--;
}

void agentsDecay(AgentRegistry *r, float dt)
{
	int i;

	if (r == NULL)
		return;
	if (dt > AGENT_MAX_DT)
		dt = AGENT_MAX_DT;
	for (i = 0; i < AGENT_MAX; i++) {
		Agent *a = &r->slots[i];
		const RoleProfile *p;

		if (!r->alive[i])
			continue;
		/* Read the role's row once, here, and pass it down: the decay
		 * helpers are pure functions of (state, multipliers, dt) and
		 * know nothing about roles. A NULL row is a bad data row, not
		 * a reason to crash every agent in the registry — skip this
		 * agent's tick and leave it exactly as it was. */
		p = agentRoleProfile(a->role);
		if (p == NULL)
			continue;
		needDecay(&a->needs, p->decayMul, dt);
		needPanicDecayRole(&a->needs, p->panicThreshold, dt);
		a->panicMask = needPanicMask(&a->needs);
	}
}

const RoleProfile *agentRoleProfile(int role)
{
	if (role < 0 || role >= ROLE_COUNT)
		return NULL;
	return &g_roleProfiles[role];
}

void agentsStats(AgentStats *out)
{
	if (out == NULL)
		return;
	/* Nothing decides or paths yet (that is the activity layer), so both
	 * counters read zero — but the zeroing IS the contract later phases
	 * rely on when they report a delta across their own span. */
	memset(out, 0, sizeof(*out));
}