/*
 * Scene vtable + scene stack tests (CTOL rung 1: unit + boundary).
 *
 * Covers: inline payload allocation, sceneName/scenePayload helpers,
 * absent-callback no-ops, NULL safe endpoints, deferred push/pop/replace
 * applied in order at the next updateSceneStack call, init-before-update
 * and unload-before-init ordering on transitions, retained underlying
 * scenes under an overlay, the only-the-top update/draw rule, capacity and
 * pending-queue bounds, a queued push whose init fails, and destroy
 * hygiene for live scenes and unapplied pending ops (leak-checked by the
 * valgrind run).
 *
 * Pure: links only scene.c plus the Unity subset; no SDL. Every fake
 * callback asserts that the App argument is exactly the pointer the test
 * passed in (NULL) — no test ever dereferences App.
 *
 * Harness convention: this file belongs to the ONE headless executable
 * (test_pure). It must not define main()/setUp()/tearDown(); it owns its
 * state per test and exposes a single run_test_scene() that test_main.c
 * declares and calls.
 */

#include "unity.h"

#include "scene.h"

#include <string.h>

/* --- instrumented fake scenes ----------------------------------------- */

/* Each fake scene stores this in its inline payload; `mark` identifies the
 * instance in the shared call log. */
typedef struct FakeState {
	char mark;
	int inited;
	int updated;
	int drawn;
	int unloaded;
} FakeState;

/* Shared call log: every fake callback appends its instance mark followed
 * by the phase char ('i' init, 'u' update, 'd' draw, 'x' unload), so exact
 * vtable call order is pinned as one string. */
static char g_log[80];
static int g_log_n;

/* Counters for the failing-init vtable. Statics, not the payload: a queued
 * push whose init fails is destroyed before the test can read its payload. */
static int g_failInits;
static int g_failUnloads;

static void logPhase(FakeState *st, char phase)
{
	if (g_log_n + 2 <= (int)sizeof(g_log)) {
		g_log[g_log_n++] = st->mark;
		g_log[g_log_n++] = phase;
	}
}

static bool fake_init(void *self, App *app)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	st->inited++;
	logPhase(st, 'i');
	return true;
}

static void fake_update(void *self, App *app, float dt)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	(void)dt;
	st->updated++;
	logPhase(st, 'u');
}

static void fake_draw(void *self, App *app)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	st->drawn++;
	logPhase(st, 'd');
}

static void fake_unload(void *self, App *app)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	st->unloaded++;
	logPhase(st, 'x');
}

/* Failing-init variant: init counts in a static and returns false. */
static bool fake_init_fails(void *self, App *app)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	g_failInits++;
	logPhase(st, 'i');
	return false;
}

static void fake_unload_fails(void *self, App *app)
{
	FakeState *st = scenePayload(self);

	TEST_ASSERT_NULL(app);
	g_failUnloads++;
	logPhase(st, 'x');
}

static const SceneVt fakeVt = {
	fake_init, fake_update, fake_draw, fake_unload,
	sizeof(FakeState), "fake",
};

static const SceneVt fakeVtFailInit = {
	fake_init_fails, fake_update, fake_draw, fake_unload_fails,
	sizeof(FakeState), "failing",
};

/* All callbacks absent: every hook must be a safe no-op. */
static const SceneVt emptyVt = { NULL, NULL, NULL, NULL, 0, "empty" };

static Scene *makeFake(const SceneVt *vt, char mark)
{
	Scene *scene = createScene(vt);
	FakeState *st;

	TEST_ASSERT_NOT_NULL(scene);
	st = scenePayload(scene);
	st->mark = mark;
	return scene;
}

static FakeState *fakeState(Scene *scene)
{
	return scenePayload(scene);
}

static void resetGlobals(void)
{
	memset(g_log, 0, sizeof(g_log));
	g_log_n = 0;
	g_failInits = 0;
	g_failUnloads = 0;
}

/* --- tests ------------------------------------------------------------ */

/* createScene hands out sizeof(Scene) + vt->size bytes; the tail is the
 * inline payload, zeroed at creation and writable from the test. */
static void test_create_scene_allocates_inline_payload(void)
{
	Scene *scene = makeFake(&fakeVt, 'P');
	FakeState *st = fakeState(scene);

	TEST_ASSERT_NOT_NULL(st);
	memset(st, 0xAB, sizeof(*st));
	/* The payload is the allocated tail: the pattern survives re-reads. */
	st->mark = 'P';
	TEST_ASSERT_EQUAL_MEMORY("P", &st->mark, 1);
	TEST_ASSERT_EQUAL_MEMORY("\xAB\xAB\xAB", &st->updated, 3);

	TEST_ASSERT_EQUAL_STRING("fake", sceneName(scene));
	destroyScene(scene);	/* leak-checked by the valgrind run */
}

/* A vtable with absent callbacks: init counts as success, the other hooks
 * are no-ops, and a zero-size payload is still addressable — but never
 * dereferenced, so this fake is created bare (no mark written into it). */
static void test_absent_callbacks_are_no_ops(void)
{
	Scene *scene = createScene(&emptyVt);

	TEST_ASSERT_NOT_NULL(scene);
	TEST_ASSERT_EQUAL_STRING("empty", sceneName(scene));
	TEST_ASSERT_NOT_NULL(scenePayload(scene));
	TEST_ASSERT_TRUE(initScene(scene, NULL));
	updateScene(scene, NULL, 0.5f);
	drawScene(scene, NULL);
	unloadScene(scene, NULL);
	destroyScene(scene);
}

/* Every endpoint tolerates NULL instances/stacks without touching memory. */
static void test_null_arguments_are_safe(void)
{
	SceneStack *stack = createSceneStack(4);

	TEST_ASSERT_NOT_NULL(stack);

	TEST_ASSERT_NULL(createScene(NULL));
	TEST_ASSERT_NULL(createSceneStack(0));
	destroyScene(NULL);
	destroySceneStack(NULL);

	TEST_ASSERT_FALSE(initScene(NULL, NULL));
	updateScene(NULL, NULL, 1.0f);
	drawScene(NULL, NULL);
	unloadScene(NULL, NULL);
	TEST_ASSERT_NULL(scenePayload(NULL));
	TEST_ASSERT_NULL(sceneName(NULL));

	TEST_ASSERT_NULL(activeScene(NULL));

	TEST_ASSERT_FALSE(pushScene(NULL, NULL));
	TEST_ASSERT_FALSE(popScene(NULL));
	TEST_ASSERT_FALSE(replaceScene(NULL, NULL));
	updateSceneStack(NULL, NULL, 1.0f);
	drawSceneStack(NULL, NULL);

	destroySceneStack(stack);
}

/* Deferral: pushScene records, nothing is initialized and the active scene
 * is unchanged until the next updateSceneStack applies it; the freshly
 * applied top inits and then updates in that same call. */
static void test_push_defers_then_init_updates_top(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	FakeState *sa = fakeState(a);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	/* Queued-but-unapplied: not visible until it applies ("recorded,
	 * not done" — it may even fail init and never run). */
	TEST_ASSERT_NULL(activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sa->inited);	/* deferred */

	updateSceneStack(stack, NULL, 1.0f);
	/* Init precedes the same scene's first update. */
	TEST_ASSERT_EQUAL_STRING("AiAu", g_log);
	TEST_ASSERT_EQUAL_INT(1, sa->inited);
	TEST_ASSERT_EQUAL_INT(1, sa->updated);

	drawSceneStack(stack, NULL);
	TEST_ASSERT_EQUAL_STRING("AiAuAd", g_log);
	TEST_ASSERT_EQUAL_INT(1, sa->drawn);

	destroySceneStack(stack);
}

/* Replace unloads and destroys EVERY current scene (top-down) before the
 * new scene is initialized; the new scene becomes the only scene. */
static void test_replace_unloads_all_and_installs_new_top_down(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	Scene *c = makeFake(&fakeVt, 'C');
	FakeState *sa = fakeState(a);
	FakeState *sb = fakeState(b);
	FakeState *sc = fakeState(c);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);	/* Ai Au */
	TEST_ASSERT_TRUE(pushScene(stack, b));
	updateSceneStack(stack, NULL, 1.0f);	/* Bi Bu */

	TEST_ASSERT_TRUE(replaceScene(stack, c));
	/* Deferred: the old scenes keep running until the next update. */
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sc->inited);
	TEST_ASSERT_EQUAL_INT(0, sa->unloaded);
	TEST_ASSERT_EQUAL_INT(0, sb->unloaded);

	updateSceneStack(stack, NULL, 1.0f);
	/* The apply unloads AND destroys a and b, top-down, before c's init;
	 * their payloads are gone, so the lifecycle is evidenced by the call
	 * log alone (Bx then Ax: (destroyed) then c inits and updates). */
	TEST_ASSERT_EQUAL_STRING("AiAuBiBuBxAxCiCu", g_log);
	TEST_ASSERT_EQUAL_PTR(c, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(1, sc->inited);

	destroySceneStack(stack);
}

/* Overlay push: the underlying scene stays loaded (no unload) but is frozen
 * — no updates, no draws — until the overlay pops back off. */
static void test_overlay_retains_underlying_scene_loaded(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	FakeState *sa = fakeState(a);
	FakeState *sb = fakeState(b);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);	/* Ai Au */

	TEST_ASSERT_TRUE(pushScene(stack, b));
	updateSceneStack(stack, NULL, 1.0f);	/* Bi Bu */
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sa->unloaded);	/* retained, still loaded */
	TEST_ASSERT_EQUAL_INT(1, sb->inited);

	TEST_ASSERT_TRUE(popScene(stack));
	/* Deferred: b is still the applied top until the next update. */
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));

	updateSceneStack(stack, NULL, 1.0f);	/* Bx then Au */
	TEST_ASSERT_EQUAL_STRING("AiAuBiBuBxAu", g_log);
	TEST_ASSERT_EQUAL_PTR(a, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sa->unloaded);	/* a was never unloaded */
	TEST_ASSERT_EQUAL_INT(2, sa->updated);	/* frozen only while covered */

	destroySceneStack(stack);
}

/* Only the top scene updates and draws; the depth of the freeze is exact
 * for several frames. */
static void test_only_the_active_scene_updates_and_draws(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	FakeState *sa = fakeState(a);
	FakeState *sb = fakeState(b);

	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);	/* Ai Au */
	drawSceneStack(stack, NULL);		/* Ad */
	resetGlobals();

	TEST_ASSERT_TRUE(pushScene(stack, b));
	updateSceneStack(stack, NULL, 1.0f);	/* Bi Bu */
	drawSceneStack(stack, NULL);		/* Bd: only the top draws */
	TEST_ASSERT_EQUAL_INT(1, sa->drawn);
	TEST_ASSERT_EQUAL_INT(1, sb->drawn);

	updateSceneStack(stack, NULL, 1.0f);
	drawSceneStack(stack, NULL);
	TEST_ASSERT_EQUAL_INT(2, sb->updated);	/* top keeps updating */
	TEST_ASSERT_EQUAL_INT(1, sa->updated);	/* underlying stays frozen */
	TEST_ASSERT_EQUAL_INT(2, sb->drawn);
	TEST_ASSERT_EQUAL_INT(1, sa->drawn);

	destroySceneStack(stack);
	(void)sb;
}

/* A scene pushed and popped in the same frame: pending ops apply in FIFO
 * order, so it inits then unloads without ever running update/draw. */
static void test_same_frame_push_then_pop_applies_in_order(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	FakeState *sa = fakeState(a);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);	/* Ai Au */

	TEST_ASSERT_TRUE(pushScene(stack, b));
	TEST_ASSERT_TRUE(popScene(stack));	/* pops the (pending) b */

	updateSceneStack(stack, NULL, 1.0f);
	/* FIFO: b inits (Bi), is pushed, then unloads and is destroyed (Bx)
	 * — with no Bu or Bd ever in the log. The destroyed payload is not
	 * touched; the log pins the lifecycle. */
	TEST_ASSERT_EQUAL_STRING("AiAuBiBxAu", g_log);
	TEST_ASSERT_EQUAL_PTR(a, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sa->unloaded);

	destroySceneStack(stack);
}

/* Recording-time bounds: pushes beyond capacity and pops of an empty stack
 * are rejected; a full pending queue rejects further mutations. */
static void test_capacity_and_pending_queue_bounds(void)
{
	SceneStack *stack = createSceneStack(2);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	Scene *c = makeFake(&fakeVt, 'C');
	FakeState *sa = fakeState(a);
	FakeState *sb = fakeState(b);
	FakeState *sc = fakeState(c);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	TEST_ASSERT_TRUE(pushScene(stack, b));		/* 2 pending: queue full */
	TEST_ASSERT_FALSE(replaceScene(stack, c));	/* queue full: rejected */

	updateSceneStack(stack, NULL, 1.0f);		/* Ai Bi Bu */
	TEST_ASSERT_EQUAL_STRING("AiBiBu", g_log);
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));
	TEST_ASSERT_EQUAL_INT(0, sc->inited);		/* c untouched */
	TEST_ASSERT_EQUAL_INT(1, sa->inited);
	TEST_ASSERT_EQUAL_INT(1, sb->inited);

	updateSceneStack(stack, NULL, 1.0f);		/* Bu only */
	TEST_ASSERT_EQUAL_INT(2, sb->updated);
	/* a is underneath: frozen, never updated by either call (the same
	 * rule test_only_the_active_scene_updates_and_draws pins). */
	TEST_ASSERT_EQUAL_INT(0, sa->updated);

	/* At capacity: a third push must not be recorded. */
	TEST_ASSERT_FALSE(pushScene(stack, c));
	TEST_ASSERT_EQUAL_INT(0, sc->inited);
	updateSceneStack(stack, NULL, 1.0f);
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));
	destroySceneStack(stack);

	/* Separate stack: an empty stack refuses to pop. */
	stack = createSceneStack(3);
	TEST_ASSERT_NOT_NULL(stack);
	TEST_ASSERT_FALSE(popScene(stack));
	TEST_ASSERT_NULL(activeScene(stack));
	/* Empty stacks are harmless to run and draw. */
	updateSceneStack(stack, NULL, 1.0f);
	drawSceneStack(stack, NULL);
	destroySceneStack(stack);

	/* c was never recorded: the test still owns it. */
	destroyScene(c);
	(void)sb;
	(void)sa;
	(void)sc;
}

/* A queued push whose init fails: the scene is destroyed and NOT pushed;
 * the stack continues with whatever it had, and no unload runs for the
 * never-inited scene. */
static void test_queued_push_with_failing_init_is_destroyed(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *f = makeFake(&fakeVtFailInit, 'F');
	Scene *a = makeFake(&fakeVt, 'A');

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, f));
	updateSceneStack(stack, NULL, 1.0f);
	/* Init ran and failed; the scene was dropped, never pushed. */
	TEST_ASSERT_NULL(activeScene(stack));
	TEST_ASSERT_EQUAL_INT(1, g_failInits);
	TEST_ASSERT_EQUAL_INT(0, g_failUnloads);	/* no unload for it */

	/* The failed push leaves the stack usable. */
	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);
	TEST_ASSERT_EQUAL_STRING("FiAiAu", g_log);
	TEST_ASSERT_EQUAL_PTR(a, activeScene(stack));

	destroySceneStack(stack);
}

/* Aliasing: a scene may enter the stack at most once. While a pointer is
 * already queued or applied, pushScene/replaceScene reject it and record
 * nothing — making the pathological push(s); push(s); pop() double-free
 * sequence impossible. */
static void test_aliased_scene_is_rejected_until_dropped(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	FakeState *sa = fakeState(a);

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	TEST_ASSERT_FALSE(pushScene(stack, a));		/* a queued */
	TEST_ASSERT_FALSE(replaceScene(stack, a));	/* a queued */

	updateSceneStack(stack, NULL, 1.0f);		/* a applies: Ai Au */
	TEST_ASSERT_EQUAL_STRING("AiAu", g_log);
	TEST_ASSERT_EQUAL_PTR(a, activeScene(stack));
	TEST_ASSERT_FALSE(pushScene(stack, a));		/* a live */
	TEST_ASSERT_FALSE(replaceScene(stack, a));	/* a live */

	/* The aliasing scan never touches distinct scenes. */
	TEST_ASSERT_TRUE(pushScene(stack, b));
	updateSceneStack(stack, NULL, 1.0f);
	TEST_ASSERT_EQUAL_STRING("AiAuBiBu", g_log);
	TEST_ASSERT_EQUAL_PTR(b, activeScene(stack));
	TEST_ASSERT_FALSE(replaceScene(stack, b));	/* b live */
	TEST_ASSERT_EQUAL_INT(1, sa->inited);	/* a untouched by b's entry */

	destroySceneStack(stack);
}

/* Destroying a stack with pending unapplied ops: live scenes unload
 * top-down, queued scenes (never inited) are destroyed without unload, and
 * nothing leaks. */
static void test_destroy_unloads_live_and_frees_pending(void)
{
	SceneStack *stack = createSceneStack(4);
	Scene *a = makeFake(&fakeVt, 'A');
	Scene *b = makeFake(&fakeVt, 'B');
	Scene *c = makeFake(&fakeVt, 'C');

	resetGlobals();
	TEST_ASSERT_TRUE(pushScene(stack, a));
	updateSceneStack(stack, NULL, 1.0f);	/* Ai Au */
	TEST_ASSERT_EQUAL_INT(0, fakeState(a)->unloaded);

	TEST_ASSERT_TRUE(pushScene(stack, b));		/* pending */
	TEST_ASSERT_TRUE(replaceScene(stack, c));	/* pending, on top */

	/* Applied state unchanged; a is still the live scene. */
	TEST_ASSERT_EQUAL_PTR(a, activeScene(stack));

	destroySceneStack(stack);
	/* a unloads exactly once; b and c never init or unload. */
	TEST_ASSERT_EQUAL_STRING("AiAuAx", g_log);
}

/* --- runner ----------------------------------------------------------- */

/* Called by test_main.c (the one headless executable's runner). */
void run_test_scene(void);

void run_test_scene(void)
{
	RUN_TEST(test_create_scene_allocates_inline_payload);
	RUN_TEST(test_absent_callbacks_are_no_ops);
	RUN_TEST(test_null_arguments_are_safe);
	RUN_TEST(test_push_defers_then_init_updates_top);
	RUN_TEST(test_replace_unloads_all_and_installs_new_top_down);
	RUN_TEST(test_overlay_retains_underlying_scene_loaded);
	RUN_TEST(test_only_the_active_scene_updates_and_draws);
	RUN_TEST(test_same_frame_push_then_pop_applies_in_order);
	RUN_TEST(test_capacity_and_pending_queue_bounds);
	RUN_TEST(test_aliased_scene_is_rejected_until_dropped);
	RUN_TEST(test_queued_push_with_failing_init_is_destroyed);
	RUN_TEST(test_destroy_unloads_live_and_frees_pending);
}
