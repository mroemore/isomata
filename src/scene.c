/*
 * Scene vtable and scene stack. See scene.h for the invariant block:
 * optional callbacks, deferred (FIFO) stack mutations applied at the start
 * of updateSceneStack, only-the-top update/draw, recording-time bounds
 * checks against the projected depth, and destroy hygiene for live and
 * pending scenes.
 *
 * Storage:
 * - live: fixed array of `capacity` Scene pointers, stacked at the back
 *   (live[0] is the bottom, live[depth - 1] the top).
 * - queue: fixed ring of `capacity` pending ops; head/count track the
 *   live span so recordings keep coming while a partially drained queue
 *   runs.
 *
 * Destroy is immediate at apply time: popped and replaced scenes are
 * unloaded and destroyed the moment their op runs, so the stack never
 * holds foreign memory and callers must not touch a destroyed scene. The
 * projection helpers simulate the queue's effect on the applied depth for
 * recording-time validation (see scene.h).
 */

#include "scene.h"

#include <stdlib.h>

typedef enum SceneOpKind {
	SCENE_OP_PUSH,
	SCENE_OP_POP,
	SCENE_OP_REPLACE,
} SceneOpKind;

typedef struct PendingOp {
	SceneOpKind kind;
	Scene *scene;		/* push/replace operand; NULL for pop */
} PendingOp;

struct SceneStack {
	Scene **live;		/* capacity slots; the back is the top */
	size_t depth;

	PendingOp *queue;	/* capacity ring slots */
	size_t head;
	size_t count;
	size_t capacity;
};

/* --- scene lifecycle ------------------------------------------------------- */

Scene *createScene(const SceneVt *vt)
{
	Scene *scene;

	if (vt == NULL)
		return NULL;
	/* One allocation: the Scene head plus the vt->size inline payload,
	 * zeroed (a zero-size payload reduces to plain sizeof(Scene)). */
	scene = calloc(1, sizeof(*scene) + vt->size);
	if (scene == NULL)
		return NULL;
	scene->vt = vt;
	return scene;
}

void destroyScene(Scene *scene)
{
	free(scene);		/* never calls vtable callbacks */
}

/* --- vtable entry points ----------------------------------------------------- */

bool initScene(Scene *scene, App *app)
{
	if (scene == NULL || scene->vt == NULL)
		return false;
	if (scene->vt->init == NULL)
		return true;	/* absent init counts as success */
	return scene->vt->init(scene, app);
}

void updateScene(Scene *scene, App *app, float dt)
{
	if (scene == NULL || scene->vt == NULL || scene->vt->update == NULL)
		return;
	scene->vt->update(scene, app, dt);
}

void drawScene(Scene *scene, App *app)
{
	if (scene == NULL || scene->vt == NULL || scene->vt->draw == NULL)
		return;
	scene->vt->draw(scene, app);
}

void unloadScene(Scene *scene, App *app)
{
	if (scene == NULL || scene->vt == NULL || scene->vt->unload == NULL)
		return;
	scene->vt->unload(scene, app);
}

/* --- accessors --------------------------------------------------------------- */

void *scenePayload(Scene *scene)
{
	if (scene == NULL)
		return NULL;
	return scene->_inline;	/* zero-size payload: valid, never deref */
}

const char *sceneName(const Scene *scene)
{
	if (scene == NULL || scene->vt == NULL)
		return NULL;
	return scene->vt->name;
}

/* --- stack lifecycle ----------------------------------------------------------- */

SceneStack *createSceneStack(size_t capacity)
{
	SceneStack *stack;

	if (capacity == 0)
		return NULL;
	stack = calloc(1, sizeof(*stack));
	if (stack == NULL)
		return NULL;
	stack->live = calloc(capacity, sizeof(*stack->live));
	stack->queue = calloc(capacity, sizeof(*stack->queue));
	if (stack->live == NULL || stack->queue == NULL) {
		free(stack->live);
		free(stack->queue);
		free(stack);
		return NULL;
	}
	stack->capacity = capacity;
	return stack;
}

void destroySceneStack(SceneStack *stack)
{
	size_t i;

	if (stack == NULL)
		return;

	/* Applied scenes: unload + destroy, top-down. */
	while (stack->depth > 0) {
		unloadScene(stack->live[stack->depth - 1], NULL);
		destroyScene(stack->live[--stack->depth]);
	}

	/* Unapplied pending ops: queued scenes were never initialized, so
	 * they are destroyed WITHOUT unload (a pop carries no scene). */
	for (i = 0; i < stack->count; i++) {
		const PendingOp *op = &stack->queue[(stack->head + i) %
						    stack->capacity];

		destroyScene(op->scene);
	}

	free(stack->live);
	free(stack->queue);
	free(stack);
}

/* --- projection ------------------------------------------------------------------ */

/* Simulate one op over a virtual scene stack. Recorded sequences provably
 * stay inside capacity (see scene.h), and the apply pass floors at an
 * underflow the same way, so the push guard mirrors apply-time behavior. */
static void simOp(SceneOpKind kind, Scene *scene, Scene **virt,
		  size_t capacity, size_t *depth)
{
	switch (kind) {
	case SCENE_OP_PUSH:
		if (*depth < capacity) {
			virt[*depth] = scene;
			(*depth)++;
		}
		break;
	case SCENE_OP_POP:
		if (*depth > 0)
			(*depth)--;
		break;
	case SCENE_OP_REPLACE:
		virt[0] = scene;
		*depth = 1;
		break;
	}
}

/* Run the pending queue over the applied stack in a scratch copy: VLA is
 * fine here — capacity is the stack's own bound and every caller runs
 * against an already-allocated stack. Only slots below depth are read. */
static void simulateQueue(const SceneStack *stack, Scene **virt, size_t *depth)
{
	size_t i;

	*depth = stack->depth;
	for (i = 0; i < stack->depth; i++)
		virt[i] = stack->live[i];
	for (i = 0; i < stack->count; i++) {
		const PendingOp *op = &stack->queue[(stack->head + i) %
						    stack->capacity];

		simOp(op->kind, op->scene, virt, stack->capacity, depth);
	}
}

/* Stack depth before the candidate op, once the current queue has run. */
static size_t projectedDepth(const SceneStack *stack)
{
	Scene *virt[stack->capacity];
	size_t depth;

	simulateQueue(stack, virt, &depth);
	return depth;
}

/* --- deferred mutations --------------------------------------------------------- */

/* A scene may enter the stack at most once: applied (live) or already
 * queued in a pending op means the stack is holding it. Cheap identity
 * scan; keeps double-free sequences like push(s); push(s); pop() off the
 * stack. */
static bool sceneHeld(const SceneStack *stack, const Scene *scene)
{
	size_t i;

	for (i = 0; i < stack->depth; i++)
		if (stack->live[i] == scene)
			return true;
	for (i = 0; i < stack->count; i++) {
		const PendingOp *op = &stack->queue[(stack->head + i) %
						    stack->capacity];

		if (op->scene == scene)
			return true;	/* pops carry NULL, never a match */
	}
	return false;
}

bool pushScene(SceneStack *stack, Scene *scene)
{
	if (stack == NULL || scene == NULL)
		return false;
	if (sceneHeld(stack, scene))
		return false;	/* already live or queued */
	if (stack->count >= stack->capacity)
		return false;	/* pending queue full */
	if (projectedDepth(stack) + 1 > stack->capacity)
		return false;	/* projection would exceed capacity */
	stack->queue[(stack->head + stack->count) % stack->capacity] =
		(PendingOp){ SCENE_OP_PUSH, scene };
	stack->count++;
	return true;
}

bool popScene(SceneStack *stack)
{
	if (stack == NULL)
		return false;
	if (stack->count >= stack->capacity)
		return false;	/* pending queue full */
	if (projectedDepth(stack) == 0)
		return false;	/* there is no scene to pop */
	stack->queue[(stack->head + stack->count) % stack->capacity] =
		(PendingOp){ SCENE_OP_POP, NULL };
	stack->count++;
	return true;
}

bool replaceScene(SceneStack *stack, Scene *scene)
{
	if (stack == NULL || scene == NULL)
		return false;
	if (sceneHeld(stack, scene))
		return false;	/* already live or queued */
	if (stack->count >= stack->capacity)
		return false;	/* pending queue full */
	/* A replace leaves its own scene as the only one, so no depth
	 * projection can bound it — only queue space applies. */
	stack->queue[(stack->head + stack->count) % stack->capacity] =
		(PendingOp){ SCENE_OP_REPLACE, scene };
	stack->count++;
	return true;
}

/* --- frame entry points --------------------------------------------------------------- */

/* The applied top, or NULL on an applied-empty stack. Queued ops are
 * never visible: "true" from a mutator is recorded, not done. */
Scene *activeScene(const SceneStack *stack)
{
	if (stack == NULL || stack->depth == 0)
		return NULL;
	return stack->live[stack->depth - 1];
}

/* Drain the pending queue, applying each op FIFO against the stack as it
 * stands. Pop and replace unload AND destroy at apply time — the dropped
 * scenes' memory is gone the moment their callbacks run. A queued push
 * whose init fails is destroyed and dropped (silent); later queued ops
 * simply run against the stack that remains. */
static void applyPending(SceneStack *stack, App *app)
{
	while (stack->count > 0) {
		const PendingOp op = stack->queue[stack->head];

		stack->head = (stack->head + 1) % stack->capacity;
		stack->count--;

		switch (op.kind) {
		case SCENE_OP_PUSH:
			if (!initScene(op.scene, app)) {
				destroyScene(op.scene);	/* no unload */
				break;
			}
			stack->live[stack->depth++] = op.scene;
			break;

		case SCENE_OP_POP:
			if (stack->depth > 0) {
				Scene *top = stack->live[--stack->depth];

				unloadScene(top, app);
				destroyScene(top);
			}
			break;

		case SCENE_OP_REPLACE:
			while (stack->depth > 0) {
				Scene *top = stack->live[--stack->depth];

				unloadScene(top, app);
				destroyScene(top);
			}
			if (!initScene(op.scene, app)) {
				destroyScene(op.scene);
				break;
			}
			stack->live[stack->depth++] = op.scene;
			break;
		}
	}
}

void updateSceneStack(SceneStack *stack, App *app, float dt)
{
	if (stack == NULL)
		return;
	applyPending(stack, app);
	updateScene(activeScene(stack), app, dt);
}

void drawSceneStack(SceneStack *stack, App *app)
{
	if (stack == NULL)
		return;
	drawScene(activeScene(stack), app);
}
