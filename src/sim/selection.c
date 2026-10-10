/*
 * Machine-selection heuristics (see sim/selection.h).
 *
 * Three pure integer scorers behind a named table; selChoose scans candidates
 * in handle order and keeps the lowest score (ties -> lowest handle). No
 * allocation, no SDL, no time source.
 */

#include "sim/selection.h"

#include <stddef.h>
#include <string.h>

/* Manhattan tile distance between an entity and a machine. */
static int selDistEntity(const Entity *e, const Machine *m)
{
	int dx = m->tileX - e->tileX;
	int dz = m->tileZ - e->tileZ;

	if (dx < 0)
		dx = -dx;
	if (dz < 0)
		dz = -dz;
	return dx + dz;
}

static int selDistTiles(int ax, int az, int bx, int bz)
{
	int dx = bx - ax;
	int dz = bz - az;

	if (dx < 0)
		dx = -dx;
	if (dz < 0)
		dz = -dz;
	return dx + dz;
}

static int scoreClosest(const Entity *e, const Machine *m, const SelWorld *w)
{
	(void)w;
	return selDistEntity(e, m);
}

static int scoreSpreadK(const Entity *e, const Machine *m, const SelWorld *w)
{
	int score = selDistEntity(e, m);
	int k = w != NULL ? w->spreadK : SEL_SPREAD_DEFAULT_K;

	if (w != NULL && w->machines != NULL && k > 0) {
		MachineHandle h;

		for (h = machineFirst(w->machines); h != MACHINE_INVALID;
		     h = machineNext(w->machines, h)) {
			const Machine *other = machineGetConst(w->machines, h);

			if (other == NULL || other == m)
				continue;
			if (other->owner < MACHINE_OWNER_MIN)
				continue;	/* free: not "occupied" */
			if (selDistTiles(m->tileX, m->tileZ, other->tileX,
					 other->tileZ) <= k) {
				score += SEL_SPREAD_PENALTY;
				break;
			}
		}
	}
	return score;
}

static int scoreCheapest(const Entity *e, const Machine *m, const SelWorld *w)
{
	(void)w;
	return m->fee * SEL_FEE_SCALE + selDistEntity(e, m);
}

typedef int (*SelScoreFn)(const Entity *e, const Machine *m,
			  const SelWorld *w);

static const struct {
	const char *name;
	SelScoreFn score;
} SEL_TABLE[SEL_POLICY_COUNT] = {
	{ "closest", scoreClosest },
	{ "spread_k", scoreSpreadK },
	{ "cheapest", scoreCheapest },
};

void selWorldInit(SelWorld *w, const MachineRegistry *machines)
{
	if (w == NULL)
		return;
	w->machines = machines;
	w->spreadK = SEL_SPREAD_DEFAULT_K;
}

int selScore(const Entity *e, const Machine *m, const SelWorld *w, int policy)
{
	if (e == NULL || m == NULL || !selPolicyValid(policy))
		return SEL_SCORE_INVALID;
	return SEL_TABLE[policy].score(e, m, w);
}

/* True when `h` is in the exclusion list. */
static bool selExcluded(MachineHandle h, const MachineHandle *exclude,
			int nExclude)
{
	int i;

	if (exclude == NULL || nExclude <= 0)
		return false;
	for (i = 0; i < nExclude; i++)
		if (exclude[i] == h)
			return true;
	return false;
}

MachineHandle selChoose(const MachineRegistry *machines, const Entity *e,
			const SelWorld *w, int policy, int requiredKind,
			const MachineHandle *exclude, int nExclude)
{
	MachineHandle best = MACHINE_INVALID;
	int bestScore = 0;
	MachineHandle h;

	if (machines == NULL || e == NULL)
		return MACHINE_INVALID;
	for (h = machineFirst(machines); h != MACHINE_INVALID;
	     h = machineNext(machines, h)) {
		const Machine *m = machineGetConst(machines, h);
		int score;

		if (m == NULL || m->kind != requiredKind ||
		    m->state != MACHINE_STATE_FREE)
			continue;
		if (selExcluded(h, exclude, nExclude))
			continue;
		score = selScore(e, m, w, policy);
		if (best == MACHINE_INVALID || score < bestScore) {
			best = h;
			bestScore = score;
		}
	}
	return best;
}

int selPolicyByName(const char *name)
{
	int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < SEL_POLICY_COUNT; i++)
		if (strcmp(name, SEL_TABLE[i].name) == 0)
			return i;
	return -1;
}

const char *selPolicyName(int policy)
{
	if (!selPolicyValid(policy))
		return "?";
	return SEL_TABLE[policy].name;
}

bool selPolicyValid(int policy)
{
	return policy >= 0 && policy < SEL_POLICY_COUNT;
}
