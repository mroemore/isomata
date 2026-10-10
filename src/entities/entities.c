/*
 * Pure entity registry + tile-based movement (see entities/entities.h).
 *
 * No allocation: the registry is a fixed array of ENTITY_MAX slots. A handle
 * is a slot index; `alive[]` gates validity, so a freed slot can be reused but
 * two live entities never alias. Movement is a per-segment lerp between two
 * tile centres; the easing curve shapes the lerp when `animated`, otherwise
 * the entity holds the from tile and snaps at segment end.
 *
 * Everything is deterministic: no time source, no randomness, no libm beyond
 * sqrtf/fabsf, and entitiesUpdate clamps dt so a single call advances a bounded
 * distance.
 */

#include "entities/entities.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ENTITY_DEFAULT_SPEED 2.0f

static void entityUpdatePosition(Entity *e)
{
	float fx;
	float fz;
	float tx;
	float tz;
	float fy;
	float ty;
	float t;

	if (!e->moving) {
		e->x = (float)e->tileX + 0.5f;
		e->z = (float)e->tileZ + 0.5f;
		e->y = voxmapSurfaceY(e->map, e->tileX, e->tileZ);
		return;
	}
	/* A non-animated entity holds the from tile for the segment's duration
	 * (t stays 0) and snaps in entityCompleteSegment. */
	t = e->animated ? easingApply(e->easing, e->segT) : 0.0f;
	fx = (float)e->fromX + 0.5f;
	fz = (float)e->fromZ + 0.5f;
	tx = (float)e->toX + 0.5f;
	tz = (float)e->toZ + 0.5f;
	fy = voxmapSurfaceY(e->map, e->fromX, e->fromZ);
	ty = voxmapSurfaceY(e->map, e->toX, e->toZ);
	e->x = fx + (tx - fx) * t;
	e->z = fz + (tz - fz) * t;
	e->y = fy + (ty - fy) * t;
}

/* Begin a segment from the entity's current tile to (tx, tz). Assumes the
 * caller validated walkability. */
static void entityBeginSegment(Entity *e, int tx, int tz)
{
	float dx = (float)(tx - e->tileX);
	float dz = (float)(tz - e->tileZ);
	float dist = sqrtf(dx * dx + dz * dz);

	e->fromX = e->tileX;
	e->fromZ = e->tileZ;
	e->toX = tx;
	e->toZ = tz;
	e->segT = 0.0f;
	e->moving = true;
	/* A non-positive speed means an instant arrival (duration 0). */
	e->segDuration = e->speed > 0.0f ? dist / e->speed : 0.0f;
	entityUpdatePosition(e);
}

/* Finish the current segment: arrive on its target tile, pulse `arrived`, then
 * start the next queued step if there is one. */
static void entityCompleteSegment(Entity *e)
{
	int nx;
	int nz;

	e->arrived = true;
	e->segT = 0.0f;
	e->moving = false;
	e->tileX = e->toX;
	e->tileZ = e->toZ;
	e->x = (float)e->tileX + 0.5f;
	e->z = (float)e->tileZ + 0.5f;
	e->y = voxmapSurfaceY(e->map, e->tileX, e->tileZ);

	if (e->pathCount == 0)
		return;
	nx = e->path[e->pathHead][0];
	nz = e->path[e->pathHead][1];
	e->pathHead++;
	e->pathCount--;
	if (e->pathCount == 0)
		e->pathHead = 0;
	entityBeginSegment(e, nx, nz);
}

void entitiesInit(EntityRegistry *reg, const Voxmap *map)
{
	if (reg == NULL)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->map = map;
}

EntityHandle entityCreate(EntityRegistry *reg)
{
	int i;

	if (reg == NULL)
		return ENTITY_INVALID;
	for (i = 0; i < ENTITY_MAX; i++) {
		Entity *e;

		if (reg->alive[i])
			continue;
		e = &reg->slots[i];
		*e = (Entity){ 0 };
		e->x = 0.5f;
		e->z = 0.5f;
		e->speed = ENTITY_DEFAULT_SPEED;
		e->animated = true;
		e->easing = EASE_IN_OUT;
		e->width = 1.0f;
		e->height = 1.0f;
		e->tint = 0xffffffffu;
		e->material = -1;
		e->map = reg->map;
		reg->alive[i] = true;
		reg->liveCount++;
		return i;
	}
	return ENTITY_INVALID;
}

bool entityDestroy(EntityRegistry *reg, EntityHandle id)
{
	if (reg == NULL || id < 0 || id >= ENTITY_MAX || !reg->alive[id])
		return false;
	reg->alive[id] = false;
	reg->slots[id] = (Entity){ 0 };
	reg->liveCount--;
	return true;
}

bool entityAlive(const EntityRegistry *reg, EntityHandle id)
{
	return reg != NULL && id >= 0 && id < ENTITY_MAX && reg->alive[id];
}

Entity *entityGet(EntityRegistry *reg, EntityHandle id)
{
	if (!entityAlive(reg, id))
		return NULL;
	return &reg->slots[id];
}

int entityCount(const EntityRegistry *reg)
{
	return reg == NULL ? 0 : reg->liveCount;
}

EntityHandle entityFirst(const EntityRegistry *reg)
{
	return entityNext(reg, ENTITY_INVALID);
}

EntityHandle entityNext(const EntityRegistry *reg, EntityHandle id)
{
	int i;

	if (reg == NULL)
		return ENTITY_INVALID;
	for (i = id + 1; i < ENTITY_MAX; i++)
		if (reg->alive[i])
			return i;
	return ENTITY_INVALID;
}

bool entityPlace(Entity *e, int tileX, int tileZ)
{
	float y;

	if (e == NULL || e->map == NULL)
		return false;
	y = voxmapSurfaceY(e->map, tileX, tileZ);
	if (y < 0.0f)
		return false;
	e->moving = false;
	e->segT = 0.0f;
	e->arrived = false;
	e->pathHead = 0;
	e->pathCount = 0;
	e->tileX = tileX;
	e->tileZ = tileZ;
	e->x = (float)tileX + 0.5f;
	e->z = (float)tileZ + 0.5f;
	e->y = y;
	return true;
}

void entitySetSprite(Entity *e, float width, float height, uint32_t tint,
		     int16_t material)
{
	if (e == NULL)
		return;
	e->width = width;
	e->height = height;
	e->tint = tint;
	e->material = material;
}

bool entityWalkableStep(const Voxmap *map, int fromX, int fromZ, int toX,
			int toZ)
{
	int dx;
	int dz;
	float fy;
	float ty;

	if (map == NULL)
		return false;
	dx = toX - fromX;
	dz = toZ - fromZ;
	if (dx * dx + dz * dz != 1)	/* exactly one 4-neighbour step */
		return false;
	ty = voxmapSurfaceY(map, toX, toZ);
	if (ty < 0.0f)			/* void / out of bounds */
		return false;
	fy = voxmapSurfaceY(map, fromX, fromZ);
	return fabsf(ty - fy) <= 1.0f;
}

bool entityMoveTo(Entity *e, int tx, int tz)
{
	int dx;
	int dz;

	if (e == NULL || e->map == NULL)
		return false;
	/* A direct move replaces any queued path. */
	e->pathHead = 0;
	e->pathCount = 0;

	if (tx == e->tileX && tz == e->tileZ) {
		if (e->moving) {
			e->moving = false;
			e->segT = 0.0f;
			entityUpdatePosition(e);
		}
		return true;
	}
	dx = tx - e->tileX;
	dz = tz - e->tileZ;
	if (dx * dx + dz * dz == 1) {
		if (!entityWalkableStep(e->map, e->tileX, e->tileZ, tx, tz)) {
			fprintf(stderr,
				"entities: move to (%d,%d) rejected (not walkable)\n",
				tx, tz);
			return false;
		}
	} else if (voxmapSurfaceY(e->map, tx, tz) < 0.0f) {
		fprintf(stderr,
			"entities: move to (%d,%d) rejected (void)\n", tx, tz);
		return false;
	}
	e->arrived = false;
	entityBeginSegment(e, tx, tz);
	return true;
}

bool entityWalkPath(Entity *e, const int (*tiles)[2], int n)
{
	int refX;
	int refZ;
	int i;

	if (e == NULL || e->map == NULL || tiles == NULL) {
		fprintf(stderr, "entities: walk path rejected (null entity/map/tiles)\n");
		return false;
	}
	if (n < 0 || n > ENTITY_MAX_PATH) {
		fprintf(stderr,
			"entities: walk path of %d rejected (max %d)\n", n,
			ENTITY_MAX_PATH);
		return false;
	}
	if (n == 0)
		return true;
	if (e->pathHead + e->pathCount + n > ENTITY_MAX_PATH) {
		fprintf(stderr,
			"entities: path of %d rejected (queue has %d/%d)\n", n,
			e->pathHead + e->pathCount, ENTITY_MAX_PATH);
		return false;
	}

	/* Reference tile: the last queued waypoint, else the segment target we
	 * are heading to, else where we stand. */
	if (e->pathCount > 0) {
		refX = e->path[e->pathHead + e->pathCount - 1][0];
		refZ = e->path[e->pathHead + e->pathCount - 1][1];
	} else if (e->moving) {
		refX = e->toX;
		refZ = e->toZ;
	} else {
		refX = e->tileX;
		refZ = e->tileZ;
	}

	/* Validate the whole path before mutating: rejection is atomic. */
	for (i = 0; i < n; i++) {
		int nx = tiles[i][0];
		int nz = tiles[i][1];

		if (!entityWalkableStep(e->map, refX, refZ, nx, nz)) {
			fprintf(stderr,
				"entities: path step %d (%d,%d) rejected (not a walkable 4-neighbour)\n",
				i, nx, nz);
			return false;
		}
		refX = nx;
		refZ = nz;
	}

	for (i = 0; i < n; i++) {
		int idx = e->pathHead + e->pathCount + i;

		e->path[idx][0] = tiles[i][0];
		e->path[idx][1] = tiles[i][1];
	}
	e->pathCount += n;

	if (!e->moving) {
		int nx = e->path[e->pathHead][0];
		int nz = e->path[e->pathHead][1];

		e->pathHead++;
		e->pathCount--;
		if (e->pathCount == 0)
			e->pathHead = 0;
		e->arrived = false;
		entityBeginSegment(e, nx, nz);
	}
	return true;
}

void entitiesUpdate(EntityRegistry *reg, float dt)
{
	int i;

	if (reg == NULL)
		return;
	/* NaN-safe clamp: !(dt > 0) is true for NaN and negatives. */
	if (!(dt > 0.0f))
		dt = 0.0f;
	else if (dt > ENTITY_MAX_DT)
		dt = ENTITY_MAX_DT;

	for (i = 0; i < ENTITY_MAX; i++) {
		Entity *e = &reg->slots[i];
		float remaining;

		if (!reg->alive[i])
			continue;
		e->arrived = false;
		if (!e->moving)
			continue;

		remaining = dt;
		while (e->moving && remaining > 0.0f) {
			float need;

			if (e->segDuration <= 0.0f) {
				/* Degenerate (speed <= 0): arrive instantly. */
				entityCompleteSegment(e);
				continue;
			}
			need = (1.0f - e->segT) * e->segDuration;
			if (remaining < need) {
				e->segT += remaining / e->segDuration;
				remaining = 0.0f;
			} else {
				remaining -= need;
				entityCompleteSegment(e);
			}
		}
		entityUpdatePosition(e);
	}
}
