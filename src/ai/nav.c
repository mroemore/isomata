/*
 * Navigation glue (see ai/nav.h): queue a computed route onto an entity.
 */

#include "ai/nav.h"

#include "ai/pathfind.h"

bool entityFollowPath(Entity *e, const int (*tiles)[2], int n)
{
	return entityWalkPath(e, tiles, n);
}

int entityPathTo(Entity *e, int tx, int tz)
{
	int tiles[ENTITY_MAX_PATH][2];
	int n = 0;
	int fromX;
	int fromZ;

	if (e == NULL || e->map == NULL)
		return -1;
	/* Replan from the committed tile: while moving that is the in-flight
	 * segment target, so clearing the queue continues the walk forward
	 * instead of teleporting the entity back to its last arrival. */
	if (e->moving) {
		fromX = e->toX;
		fromZ = e->toZ;
	} else {
		fromX = e->tileX;
		fromZ = e->tileZ;
	}
	if (findPath(e->map, fromX, fromZ, tx, tz, tiles, ENTITY_MAX_PATH,
		     &n) != PATH_OK)
		return -1;

	/* Replace the remaining route. entityWalkPath references the in-flight
	 * segment target when the queue is emptied, matching `fromX/fromZ`. */
	e->pathHead = 0;
	e->pathCount = 0;
	if (n > 0 && !entityWalkPath(e, tiles, n))
		return -1;	/* unreachable for a findPath result; fail safe */
	return n;
}
