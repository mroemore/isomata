#ifndef ISOMATA_AI_NAV_H
#define ISOMATA_AI_NAV_H

/*
 * Navigation glue between A* (ai/pathfind.h) and the entity walker
 * (entities/entities.h): the two helpers the sim (T23) and the demo use to
 * turn a computed route into tile-stepped movement. Pure — no SDL, no
 * allocation; the A* node store is a fixed local array.
 *
 * RE-PATH SEMANTICS (pinned). entityPathTo REPLACES the entity's remaining
 * route: it drops every queued waypoint and queues the freshly computed route.
 * An in-flight segment is left alone, so the new route is computed from that
 * segment's target tile and the entity never snaps backwards; an idle entity
 * computes from its current tile and starts walking immediately. On failure
 * (no map, void/unreachable target, or a route longer than ENTITY_MAX_PATH)
 * the entity is untouched and nothing is queued. entityFollowPath, by
 * contrast, appends a caller-validated route to the queue exactly like
 * entityWalkPath (it is a named pass-through for use by the sim; it does not
 * clear). Use entityPathTo for a mid-walk re-plan.
 */

#include "entities/entities.h"

#include <stdbool.h>

/* Validate and queue `n` 4-adjacent waypoints (a thin pass-through to
 * entityWalkPath, which owns the adjacency/walkability/overflow rules).
 * Returns false (nothing queued) on any rejection. NULL-safe. */
bool entityFollowPath(Entity *e, const int (*tiles)[2], int n);

/* Compute an A* route from the entity's current tile to (tx, tz) and replace
 * the entity's route with it (see the semantics note). Returns the number of
 * tiles queued (>= 0), or -1 when it rejects (NULL entity / no map, void or
 * unreachable target, PATH_TOO_LONG, or a queue overflow). */
int entityPathTo(Entity *e, int tx, int tz);

#endif /* ISOMATA_AI_NAV_H */
