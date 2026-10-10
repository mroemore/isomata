#ifndef ISOMATA_ENTITIES_ENTITIES_H
#define ISOMATA_ENTITIES_ENTITIES_H

/*
 * Pure entity registry + tile-based movement with an optional easing tween.
 * No SDL, no dynamic allocation: the registry is a caller-owned fixed array.
 *
 * MODEL. An entity lives on a tile cell (int tileX, tileZ) and carries a
 * continuous world position (x, y, z) — the billboard anchor (bottom-centre)
 * of its linked sprite. Movement is strictly tile-to-tile: a move is a
 * segment between two tile centres that takes `distance / speed` seconds;
 * during the segment the position lerps from the from-tile centre to the
 * to-tile centre, with y following the walkable surface (voxmapSurfaceY).
 * When `animated` is true the per-axis lerp uses the entity's easing curve;
 * when false the entity holds the from tile for the segment's duration and
 * snaps to the to tile at segment end (tile-stepped, no interpolation).
 *
 * A far target given to entityMoveTo is a single straight segment (adjacent
 * or far). A path queued with entityWalkPath is a list of 4-adjacent
 * waypoints the follower consumes one segment at a time, so a long walk is
 * always tile-to-tile (no Bresenham/diagonal shortcut; pathfinding is out of
 * scope). Because the surface height is only sampled at the two segment
 * endpoints, a far single-segment move interpolates y linearly and does not
 * follow terrain undulations — use a queued path for that (documented).
 *
 * Walker layer keeps movement deterministic: entitiesUpdate clamps dt to
 * ENTITY_MAX_DT and advances every moving entity; an "arrived at tile" flag
 * pulses true for the update in which a segment completes.
 */

#include "entities/easing.h"
#include "render/drawlist.h"
#include "render/voxmap.h"

#include <stdbool.h>
#include <stdint.h>

/* Registry capacity (slots). Fixed, no allocation. */
#define ENTITY_MAX 32
/* Most queued 4-adjacent steps an entity may hold. */
#define ENTITY_MAX_PATH 64
/* entitiesUpdate clamps a frame's dt to at most this many seconds, so a long
 * stall advances a bounded amount (never a teleport across the map). */
#define ENTITY_MAX_DT 0.1f
/* An invalid / dead handle. */
#define ENTITY_INVALID (-1)

typedef int EntityHandle;	/* 0..ENTITY_MAX-1, or ENTITY_INVALID */

typedef struct Entity {
	/* Tile cell (the tile the entity last arrived at / started from). */
	int tileX;
	int tileZ;
	/* Continuous world position (billboard anchor, bottom-centre). */
	float x;
	float y;
	float z;
	/* Movement tuning. */
	float speed;		/* world units per second */
	bool animated;		/* false = snap at segment end */
	EasingFn easing;
	/* Sprite link (mirrors render/sprites.h::SpriteEntity). */
	float width;
	float height;
	uint32_t tint;		/* RGBA (see DRAW_TINT) */
	int16_t material;	/* atlas material id; -1 = fallback region */
	/* Current segment. */
	bool moving;
	int fromX;
	int fromZ;
	int toX;
	int toZ;
	float segT;		/* raw progress in [0, 1] (eased for position) */
	float segDuration;	/* seconds for the current segment */
	bool arrived;		/* one-update pulse: reached a tile */
	/* Queued 4-adjacent steps (a ring-free FIFO: pathHead + pathCount). */
	int path[ENTITY_MAX_PATH][2];
	int pathHead;
	int pathCount;
	/* T20 path-debug: the tile this journey began from — the entity's tile
	 * when the active move (entityMoveTo) or queued path (entityWalkPath)
	 * was issued. It stays fixed as individual segments complete, so it is
	 * the "path start" marker. Meaningful only while `moving || pathCount >
	 * 0`; entitiesDebugMarkers ignores it otherwise. */
	int pathStartX;
	int pathStartZ;
	const Voxmap *map;	/* borrowed; surfaces + walkability */
} Entity;

typedef struct EntityRegistry {
	Entity slots[ENTITY_MAX];
	bool alive[ENTITY_MAX];
	int liveCount;
	const Voxmap *map;	/* borrowed; copied into each created entity */
} EntityRegistry;

/* Reset the registry to empty and bind the borrowed map (may be NULL: the
 * registry still works, but movement rejects every step). NULL `reg` is a
 * no-op. */
void entitiesInit(EntityRegistry *reg, const Voxmap *map);

/* Allocate a slot and return its handle, or ENTITY_INVALID when the registry
 * is full / NULL. The new entity has default tuning (speed 2, animated,
 * EASE_IN_OUT, 1x1 white f-fallback billboard) at tile (0, 0); call
 * entityPlace to put it on real ground. */
EntityHandle entityCreate(EntityRegistry *reg);

/* Free a slot. Returns false for a NULL registry or an unknown / already dead
 * handle. A freed slot may be handed out again, but two live entities never
 * share a slot. */
bool entityDestroy(EntityRegistry *reg, EntityHandle id);

/* True when `id` names a live entity. */
bool entityAlive(const EntityRegistry *reg, EntityHandle id);

/* Mutable pointer to a live entity, or NULL for a NULL registry / dead
 * handle. */
Entity *entityGet(EntityRegistry *reg, EntityHandle id);

/* Live entity count (0 for NULL). */
int entityCount(const EntityRegistry *reg);

/* Iterate live handles in slot order:
 *   for (EntityHandle h = entityFirst(reg); h != ENTITY_INVALID;
 *        h = entityNext(reg, h)) ...
 * entityFirst returns the lowest live handle or ENTITY_INVALID; entityNext the
 * lowest live handle greater than `id`. */
EntityHandle entityFirst(const EntityRegistry *reg);
EntityHandle entityNext(const EntityRegistry *reg, EntityHandle id);

/* Snap the entity to the centre of tile (tileX, tileZ) and to that tile's
 * surface height, cancelling any move / queued path. Returns false (leaving
 * the entity untouched) when it is NULL, has no map, or the tile is void
 * (voxmapSurfaceY < 0). */
bool entityPlace(Entity *e, int tileX, int tileZ);

/* Set the linked sprite geometry / tint / material. NULL `e` is a no-op. */
void entitySetSprite(Entity *e, float width, float height, uint32_t tint,
		     int16_t material);

/* Start a single tween segment to tile (tx, tz), replacing any queued path.
 * A 4-adjacent target must pass entityWalkableStep; a far target only needs a
 * surface (>= 0). Returns false (entity unchanged) for a NULL entity / no
 * map, an out-of-reach target, or a void target. A move to the current tile
 * is a no-op that returns true. */
bool entityMoveTo(Entity *e, int tx, int tz);

/* Queue `n` waypoints (each [x, z]); every waypoint must be 4-adjacent to the
 * previous one (the first to the entity's current tile / last queued step)
 * and walkable. The whole call is rejected (nothing queued, stderr
 * diagnostic) if any step fails, if it would overflow ENTITY_MAX_PATH, or for
 * a NULL entity / no map; n == 0 is a true no-op. If the entity is idle the
 * first queued step starts immediately. */
bool entityWalkPath(Entity *e, const int (*tiles)[2], int n);

/* True iff (toX, toZ) is exactly one 4-neighbour step from (fromX, fromZ),
 * has a surface (>= 0), and |surfaceY(to) - surfaceY(from)| <= 1.0 (a one-step
 * climb / ramp transition is walkable; a cliff or void is not). Pure; false
 * for a NULL map. */
bool entityWalkableStep(const Voxmap *map, int fromX, int fromZ, int toX,
			int toZ);

/* Advance every moving entity by `dt` seconds (clamped to [0, ENTITY_MAX_DT]).
 * Deterministic; each entity's `arrived` flag is reset then set true when a
 * segment completes this update. A NULL registry is a no-op. */
void entitiesUpdate(EntityRegistry *reg, float dt);

/* --- T20 path-debug overlay -------------------------------------------- *
 *
 * A pure derivation of the world markers the light-debug view overlays on
 * top of the entities: each live entity with an active move or queued path
 * contributes a START marker (where the journey began), one PATH marker per
 * tile the entity is still to visit (the segment target currently being
 * walked toward counts as the first), and a DEST marker on the final
 * waypoint. The DEST tile is the same tile as the last PATH marker when a
 * queue is present (the overlay stays honest to the request: the route and
 * its destination are both drawn), so the render draws the destination last
 * and smaller than the start, letting a coincident start/destination both
 * read. An idle entity (no active move, empty queue) contributes nothing.
 *
 * Colors are tints (RGBA, DRAW_TINT packing) and the widths are world-space
 * billboard sizes; both are the pinned look the render tier applies. */

typedef enum EntityDebugKind {
	ENTITY_DEBUG_START = 0,	/* where the current path began */
	ENTITY_DEBUG_PATH = 1,	/* a tile on the remaining route */
	ENTITY_DEBUG_DEST = 2,	/* the final queued waypoint */
} EntityDebugKind;

typedef struct EntityDebugMarker {
	int x;			/* tile column */
	int z;			/* tile row */
	uint8_t kind;		/* EntityDebugKind */
} EntityDebugMarker;

/* Path-marker look (pinned by the entity suite). Lift is the world-unit
 * clearance above the tile surface so a marker sits on the ground; widths are
 * the billboard size. The start marker is the largest, the destination is the
 * smallest: the demo paths are closed loops, so a start and its destination
 * land on the same tile, and this lets the destination (drawn last) sit inside
 * the start as a dot instead of hiding it. */
#define ENTITY_DEBUG_MARKER_LIFT 0.05f
#define ENTITY_DEBUG_MARKER_PATH_WIDTH 0.30f
#define ENTITY_DEBUG_MARKER_START_WIDTH 0.44f
#define ENTITY_DEBUG_MARKER_DEST_WIDTH 0.24f

#define ENTITY_DEBUG_TINT_PATH DRAW_TINT(0, 220, 220, 255)	/* cyan */
#define ENTITY_DEBUG_TINT_START DRAW_TINT(60, 220, 80, 255)	/* green */
#define ENTITY_DEBUG_TINT_DEST DRAW_TINT(230, 50, 50, 255)	/* red */

/* Write the debug markers for every live entity, in registry slot order
 * (stable), to `out` (capacity `cap`). Returns the number written, which is
 * never more than `cap`: a full buffer truncates the remaining markers (a
 * deterministic prefix) rather than overflowing. Returns 0 for a NULL
 * registry, NULL `out`, or `cap <= 0`. Pure; no allocation. */
int entitiesDebugMarkers(const EntityRegistry *reg, EntityDebugMarker *out,
			 int cap);

/* The pinned RGBA tint for a marker kind (cyan / green / red); an unknown
 * kind falls back to the path tint. */
uint32_t entityDebugMarkerTint(int kind);

/* The pinned world-width for a marker kind: the start is the largest, the
 * destination the smallest, and path waypoints in between. */
float entityDebugMarkerWidth(int kind);

#endif /* ISOMATA_ENTITIES_ENTITIES_H */
