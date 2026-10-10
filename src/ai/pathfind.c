/*
 * A* on the tile grid (see ai/pathfind.h). Fixed node store, linear-scan open
 * selection by (f, x, z), neighbour relaxation in N/E/S/W order: deterministic
 * and allocation-free.
 */

#include "ai/pathfind.h"

#include "entities/entities.h"

/* Neighbour order: N, E, S, W (north = -z). */
static const int PATHFIND_DIR[4][2] = {
	{ 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
};

typedef struct PathNode {
	int x;
	int z;
	int parent;		/* index into the store, -1 for the start */
	int g;			/* cost from the start */
	int f;			/* g + Manhattan heuristic */
	bool closed;
} PathNode;

/* Linear find by tile; -1 when absent. count <= PATHFIND_MAX_NODES. */
static int nodeFind(const PathNode *nodes, int count, int x, int z)
{
	int i;

	for (i = 0; i < count; i++)
		if (nodes[i].x == x && nodes[i].z == z)
			return i;
	return -1;
}

int pathfindManhattan(int fromX, int fromZ, int toX, int toZ)
{
	int dx = toX - fromX;
	int dz = toZ - fromZ;

	if (dx < 0)
		dx = -dx;
	if (dz < 0)
		dz = -dz;
	return dx + dz;
}

int findPath(const Voxmap *map, int fromX, int fromZ, int toX, int toZ,
	     int (*out)[2], int cap, int *outLen)
{
	PathNode nodes[PATHFIND_MAX_NODES];
	int count = 0;
	int startIdx;
	int goalIdx = -1;
	int cur;
	int i;

	if (outLen != NULL)
		*outLen = 0;
	if (out == NULL || cap < 0)
		cap = 0;

	if (map == NULL)
		return PATH_BAD_START;
	if (voxmapSurfaceY(map, fromX, fromZ) < 0.0f)
		return PATH_BAD_START;
	if (voxmapSurfaceY(map, toX, toZ) < 0.0f)
		return PATH_BAD_GOAL;
	if (fromX == toX && fromZ == toZ)
		return PATH_OK;		/* empty path, success */

	nodes[0].x = fromX;
	nodes[0].z = fromZ;
	nodes[0].parent = -1;
	nodes[0].g = 0;
	nodes[0].f = pathfindManhattan(fromX, fromZ, toX, toZ);
	nodes[0].closed = false;
	startIdx = 0;
	count = 1;

	for (;;) {
		/* Open node with the lexicographically smallest (f, x, z). */
		cur = -1;
		for (i = 0; i < count; i++) {
			if (nodes[i].closed)
				continue;
			if (cur < 0) {
				cur = i;
				continue;
			}
			if (nodes[i].f < nodes[cur].f ||
			    (nodes[i].f == nodes[cur].f &&
			     (nodes[i].x < nodes[cur].x ||
			      (nodes[i].x == nodes[cur].x &&
			       nodes[i].z < nodes[cur].z)))) {
				cur = i;
			}
		}
		if (cur < 0)
			return PATH_NO_PATH;	/* open exhausted */

		if (nodes[cur].x == toX && nodes[cur].z == toZ) {
			goalIdx = cur;
			break;
		}
		nodes[cur].closed = true;

		for (i = 0; i < 4; i++) {
			int nx = nodes[cur].x + PATHFIND_DIR[i][0];
			int nz = nodes[cur].z + PATHFIND_DIR[i][1];
			int ni = nodeFind(nodes, count, nx, nz);

			if (ni >= 0 && nodes[ni].closed)
				continue;
			if (!entityWalkableStep(map, nodes[cur].x,
						nodes[cur].z, nx, nz))
				continue;
			if (ni >= 0) {
				int ng = nodes[cur].g + 1;

				if (ng < nodes[ni].g) {
					nodes[ni].g = ng;
					nodes[ni].f = ng + pathfindManhattan(
							      nx, nz, toX, toZ);
					nodes[ni].parent = cur;
				}
			} else {
				if (count >= PATHFIND_MAX_NODES)
					return PATH_TOO_LONG;
				nodes[count].x = nx;
				nodes[count].z = nz;
				nodes[count].g = nodes[cur].g + 1;
				nodes[count].f = nodes[count].g +
						 pathfindManhattan(nx, nz, toX,
								   toZ);
				nodes[count].parent = cur;
				nodes[count].closed = false;
				count++;
			}
		}
	}

	/* Reconstruct goal -> start into `out`, then reverse in place. The
	 * start tile is excluded (the walk begins from it). */
	{
		int len = 0;
		int c = goalIdx;

		while (c != startIdx) {
			if (len >= cap)
				return PATH_TOO_LONG;
			out[len][0] = nodes[c].x;
			out[len][1] = nodes[c].z;
			len++;
			c = nodes[c].parent;
		}
		for (i = 0; i < len / 2; i++) {
			int tx = out[i][0];
			int tz = out[i][1];

			out[i][0] = out[len - 1 - i][0];
			out[i][1] = out[len - 1 - i][1];
			out[len - 1 - i][0] = tx;
			out[len - 1 - i][1] = tz;
		}
		if (outLen != NULL)
			*outLen = len;
		return PATH_OK;
	}
}
