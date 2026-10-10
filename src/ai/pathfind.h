#ifndef ISOMATA_AI_PATHFIND_H
#define ISOMATA_AI_PATHFIND_H

/*
 * A* pathfinding on the voxmap tile grid (T21). Pure: no SDL, no allocation,
 * fixed-size stores. Walkability is exactly the entity rule
 * (entities.h::entityWalkableStep): a step between 4-adjacent tiles is legal
 * iff the target has a surface (not void / out of bounds) and the surface
 * heights differ by <= 1. Uniform cost 1 per step (terrain weights are a
 * documented future extension; the cost is a constant here).
 *
 * DETERMINISM. The open set is the fixed node store; the next node is the open
 * entry with the lexicographically smallest (f, x, z) — a linear scan, no
 * heap, no tie-break by insertion time. Neighbours are relaxed in the fixed
 * order N, E, S, W (north = -z). Two runs with identical inputs therefore
 * return the identical tile list, bit for bit.
 *
 * BOUNDS. At most PATHFIND_MAX_NODES nodes (open + closed) are stored; if a
 * relaxation would need a new node and the store is full, the search stops
 * with PATH_TOO_LONG (fail safely, never overflow). A found path longer than
 * the caller's `cap` is also PATH_TOO_LONG. The heuristic is Manhattan
 * distance, admissible for uniform cost.
 */

#include "render/voxmap.h"

/* findPath result. */
#define PATH_OK 0		/* a path (possibly empty) was found */
#define PATH_NO_PATH 1		/* the goal is unreachable */
#define PATH_TOO_LONG 2		/* the node or output cap was exceeded */
#define PATH_BAD_START 3	/* start is void / out of bounds */
#define PATH_BAD_GOAL 4		/* goal is void / out of bounds */

/* Fixed node-store capacity per findPath call (open + closed combined). */
#define PATHFIND_MAX_NODES 1024

/* A* from (fromX, fromZ) to (toX, toZ). Writes the tile list to `out`
 * (capacity `cap`): the START tile is excluded, the GOAL tile included, so a
 * successful path of length L yields *outLen == L and out[L-1] == (toX,toZ).
 *
 * Returns PATH_OK on success (including start == goal, which yields
 * *outLen == 0), or a failure reason. A NULL map is PATH_BAD_START; the start
 * is validity-checked before the goal. A NULL `out` is treated as cap 0 (only
 * start == goal succeeds). `*outLen` is written only on PATH_OK (set to 0
 * otherwise). Never writes past `cap` tiles. */
int findPath(const Voxmap *map, int fromX, int fromZ, int toX, int toZ,
	     int (*out)[2], int cap, int *outLen);

/* Manhattan (4-neighbour) tile distance between two tiles, >= 0. The
 * straight-line comparison the demo logs against the real path length. */
int pathfindManhattan(int fromX, int fromZ, int toX, int toZ);

#endif /* ISOMATA_AI_PATHFIND_H */
