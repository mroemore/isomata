#ifndef ISOMATA_SIM_SELECTION_H
#define ISOMATA_SIM_SELECTION_H

/*
 * Machine-selection heuristics (T23 layer 3): pure scoring functions
 *   score(entity, machine, world)
 * behind a NAMED policy aggregator. Lower is better; the aggregator scans
 * candidates in registry-handle order and keeps the strictly-lowest score, so
 * ties break deterministically by the LOWEST machine handle (pinned below).
 *
 * SCORES ARE INTEGERS and their exact values are pinned by the suite:
 *
 *   closest   Manhattan tile distance from the entity to the machine.
 *
 *   spread_k  `closest` PLUS SEL_SPREAD_PENALTY when the candidate is within
 *             `world->spreadK` tiles (Manhattan) of any OTHER occupied machine
 *             (one with an owner). The penalty is far larger than any map
 *             distance, so the aggregator prefers ANY unpenalised candidate and
 *             only falls back to distance among peers. This generalises the
 *             user's "closest machine that is >2 machines away from an occupied
 *             one"; K is configurable and the penalty is documented here so the
 *             demo's choices are explainable.
 *
 *   cheapest  fee * SEL_FEE_SCALE + distance: fee first, distance as the
 *             tie-break. SEL_FEE_SCALE exceeds any reachable distance, so the
 *             fee dominates and distance only orders equal fees.
 *
 * Pure: no SDL, no allocation, no time source. The world passed to a score is
 * the machine registry (for occupancy) plus the spread radius; a NULL registry
 * scores every candidate as if it were alone.
 */

#include "entities/entities.h"
#include "sim/machines.h"

#include <stdbool.h>

#define SEL_POLICY_CLOSEST 0
#define SEL_POLICY_SPREAD_K 1
#define SEL_POLICY_CHEAPEST 2
#define SEL_POLICY_COUNT 3
/* The default / fallback policy (the demo's spawner cycles the mix so two
 * customers visibly choose differently). */
#define SEL_POLICY_DEFAULT SEL_POLICY_SPREAD_K

/* Penalty added to a candidate within K tiles of an occupied machine. It must
 * exceed the largest possible distance term so an unpenalised candidate always
 * beats a penalised one; the map diagonal is at most 2*VOXMAP_MAX_DIM. */
#define SEL_SPREAD_PENALTY 100000
/* Default spread radius (tiles). K=0 disables the penalty. */
#define SEL_SPREAD_DEFAULT_K 2

/* Fee multiplier for `cheapest`; must exceed any reachable distance. */
#define SEL_FEE_SCALE 1000

/* A score for a NULL / unknown input; larger than any real score. */
#define SEL_SCORE_INVALID 2147483647

/* The world a score reads. `machines` may be NULL (no occupancy data). */
typedef struct SelWorld {
	const MachineRegistry *machines;
	int spreadK;
} SelWorld;

/* Point a world at `machines` with the default spread radius. NULL-safe. */
void selWorldInit(SelWorld *w, const MachineRegistry *machines);

/* The score of machine `m` for entity `e` under `policy`. Returns
 * SEL_SCORE_INVALID for a NULL entity/machine or an unknown policy. Pure. */
int selScore(const Entity *e, const Machine *m, const SelWorld *w, int policy);

/* Choose the best machine of `requiredKind` for `e`. A candidate is a live
 * machine whose kind == `requiredKind`, whose state is FREE (broken and
 * occupied machines are excluded up front), and whose handle is not in
 * `exclude[0..nExclude)`. Returns the winning handle, or MACHINE_INVALID when
 * there is no candidate (or NULL `machines`/`e`). `exclude` may be NULL when
 * nExclude <= 0. Pure; O(candidates x machines); no allocation. */
MachineHandle selChoose(const MachineRegistry *machines, const Entity *e,
			const SelWorld *w, int policy, int requiredKind,
			const MachineHandle *exclude, int nExclude);

/* Named policy lookup: "closest" / "spread_k" / "cheapest" -> the id; anything
 * else (including NULL) -> -1. selPolicyName(unknown) -> "?". */
int selPolicyByName(const char *name);
const char *selPolicyName(int policy);
bool selPolicyValid(int policy);

#endif /* ISOMATA_SIM_SELECTION_H */
