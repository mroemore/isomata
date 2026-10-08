#ifndef ISOMATA_SCENE_H
#define ISOMATA_SCENE_H

/*
 * Vtable-driven scenes and the bounded scene stack that runs them. This
 * module is headless: App is an opaque forward declaration, no SDL header
 * is included here or in scene.c, and tests drive every API with a NULL App
 * (the pointers are only passed through to vtable callbacks).
 *
 * Scenes (vtable layout):
 *
 * - A Scene is a vtable pointer plus an inline payload of SceneVt::size
 *   bytes, zeroed at creation; scenePayload() returns it and sceneName()
 *   names the scene for diagnostics. Size 0 means a stateless scene: the
 *   payload pointer is then valid address arithmetic but must never be
 *   dereferenced.
 * - init/unload bracket a scene's life. All four SceneVt callbacks are
 *   optional: absent callbacks are no-ops, and an absent init counts as
 *   success (initScene returns true). Callbacks receive the owning Scene as
 *   their `self` argument.
 * - A Scene returned by createScene is owned by the caller until the caller
 *   hands it to a stack mutator; destroyScene frees it. The stack's
 *   unloadScene is the scene's shutdown call; destroyScene itself does not
 *   call any vtable callback.
 *
 * The stack (bounded, LIFO; the active scene is the top):
 *
 * - MUTATIONS ARE DEFERRED. pushScene/popScene/replaceScene only enqueue a
 *   pending operation and return true; the stack applies pending ops at the
 *   start of the next updateSceneStack call (so a returned true means
 *   "recorded", not "done"). The rationale: event-bus callbacks (events.h)
 *   fire mid-frame, and letting them unload the executing scene would
 *   invalidate it; the deferral also concentrates stack changes into one
 *   well-defined mutation point per frame.
 * - Pending ops apply in the order they were recorded (FIFO). Composing
 *   that rule: a scene pushed and popped in the same frame is initialized
 *   and then unloaded without ever running update/draw.
 * - Applying a queued push initializes the scene; if init fails the scene
 *   is destroyed and NOT pushed (this is silent — no status channel — and
 *   later queued ops simply act on the stack as it stands). updateSceneStack
 *   then updates ONLY the top scene; drawSceneStack draws ONLY the top
 *   scene. Underneath scenes stay loaded (their unload does not run) but
 *   are frozen: not updated, not drawn. drawSceneStackAll is the overlay
 *   exception: it draws EVERY applied scene bottom-to-top, so a retained
 *   underlay (a menu behind a settings/pause overlay) renders beneath the
 *   overlay. Updates remain top-only.
 * - The active scene is the APPLIED top only. Queued-but-unapplied ops are
 *   never visible through activeScene — a queued push may fail init and
 *   never run, so reporting it as active would lie about what is running;
 *   on an applied-empty stack activeScene returns NULL until a push applies.
 * - Destroy happens AT APPLY TIME: popScene unloads and destroys the
 *   (applied) top; replaceScene unloads and destroys EVERY scene it
 *   removes. Destroyed scenes' payloads MUST NOT be touched afterwards;
 *   lifecycle evidence lives in the callbacks' own observable behavior
 *   (ordering logs, counters scoped to surviving scenes).
 * - popScene: unload AND destroy the current (applied) top, at apply time.
 *   replaceScene: after its own scene is recorded, applying it unloads and
 *   destroys EVERY scene on the stack, top-down, then initializes and
 *   pushes its own scene as the only remaining scene — the stack's depth
 *   becomes 1.
 * - A scene may enter the stack at most once: while it is already applied
 *   (live) or already queued inside a pending op, pushScene and
 *   replaceScene REJECT it and record nothing. The scan is a cheap
 *   identity comparison and rules out double-ownership/UB sequences like
 *   push(s); pop() racing a second push of the same s.
 * - Recording-time validation, against the projected depth (live depth plus
 *   the net effect of queued ops): pushScene fails when the projection would
 *   exceed capacity; popScene fails when the projected stack is empty.
 *   Anything a mutator rejects (NULL stack/scene, bounds, aliasing) means
 *   nothing was recorded and the caller keeps ownership of the scene. Accepted pending
 *   scenes are owned by the stack until applied (then owned by the stack's
 *   scene array) — the caller must not destroy or reuse them.
 * - The pending queue holds up to capacity entries (the same bound as the
 *   stack); when it is full the mutator returns false without recording.
 *   updateSceneStack drains it.
 * - destroySceneStack unloads and destroys the live scenes top-down, then
 *   destroys the scenes still queued in unapplied pending ops (those were
 *   never initialized and get no unload), and frees the queue. It takes no
 *   App, so it unloads with a NULL App; the other stack APIs forward the App
 *   their caller passed.
 */

#include <stdbool.h>
#include <stddef.h>

/* Opaque app shell (app.h), forward-declared to keep this header, scene.c,
 * and the tests free of SDL. */
typedef struct App App;

typedef struct SceneVt SceneVt;
typedef struct Scene Scene;
typedef struct SceneStack SceneStack;

struct SceneVt {
	bool (*init)(void *self, App *app);
	void (*update)(void *self, App *app, float dt);
	void (*draw)(void *self, App *app);
	void (*unload)(void *self, App *app);
	size_t size;		/* inline payload bytes; 0 = stateless */
	const char *name;	/* diagnostics; may be NULL */
};

struct Scene {
	const SceneVt *vt;
	char _inline[];		/* SceneVt::size bytes of instance state */
};

/* Scene lifecycle. createScene allocates sizeof(Scene) + vt->size and
 * zeroes it; populating the payload is up to the vtable callbacks or the
 * creator via scenePayload(). */
Scene *createScene(const SceneVt *vt);
void destroyScene(Scene *scene);

/* Vtable entry points. Absent callbacks are no-ops; initScene returns
 * false only when a present init callback fails. initScene(NULL, ...)
 * returns false; the rest tolerate NULL scenes. */
bool initScene(Scene *scene, App *app);
void updateScene(Scene *scene, App *app, float dt);
void drawScene(Scene *scene, App *app);
void unloadScene(Scene *scene, App *app);

/* Accessors. scenePayload returns the inline state (do not dereference for
 * a zero-size vtable); sceneName returns the vtable's name, which may be
 * NULL. */
void *scenePayload(Scene *scene);
const char *sceneName(const Scene *scene);

/* Stack lifecycle. capacity bounds both the live depth and the pending op
 * queue; a zero-capacity stack is rejected (returns NULL). */
SceneStack *createSceneStack(size_t capacity);
void destroySceneStack(SceneStack *stack);

/* Deferred mutations (see the invariant block): each returns true when the
 * pending op was recorded. On false the caller keeps ownership. */
bool pushScene(SceneStack *stack, Scene *scene);
bool popScene(SceneStack *stack);
bool replaceScene(SceneStack *stack, Scene *scene);

/* The applied top scene, or NULL when the stack is empty. */
Scene *activeScene(const SceneStack *stack);

/* The frame entry points: apply pending ops in recorded order, then update
 * / draw the top scene only. Both tolerate NULL. drawSceneStackAll draws
 * every APPLIED scene bottom-to-top (retained underlays render beneath
 * overlays); it does not apply pending ops. */
void updateSceneStack(SceneStack *stack, App *app, float dt);
void drawSceneStack(SceneStack *stack, App *app);
void drawSceneStackAll(SceneStack *stack, App *app);

#endif /* ISOMATA_SCENE_H */
