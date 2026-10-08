/*
 * Element base tree tests (CTOL rung 1: unit + boundary).
 *
 * Covers: inline payload allocation and uiElementPayload, NULL-vt
 * rejection, append/remove ordering and ownership rules (already-owned
 * children rejected, self-append rejected, unlink mid-list), parent/
 * sibling accessors, rect/weight accessors, NULL safety of every
 * endpoint, UI_INTENT_COUNT, the destroy call order (self before
 * children), the uiLayout walk (self's layout() then children, top
 * down), the uiDraw walk (self then children in order, recorded ctx),
 * and both input walks (last child's subtree first, then the child,
 * then self; first consume wins). Leak-checked by the valgrind run.
 *
 * Pure: links only element.c plus the Unity subset; no SDL.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_element() for test_main.c.
 */

#include "unity.h"

#include "ui/element.h"

#include "ui_test_util.h"

#include <string.h>

/* --- fake elements --------------------------------------------------------- */

/* Each fake stores this in its inline payload; callbacks log "mark +
 * phase" into a shared string so exact call order is pinned as text. */
typedef struct FakeState {
	char mark;
} FakeState;

static char g_log[64];
static int g_log_n;
static bool g_intentConsumed;
static bool g_pointerConsumed;

static void fakeLog(const FakeState *st, char phase)
{
	if (g_log_n + 2 <= (int)sizeof(g_log)) {
		g_log[g_log_n++] = st->mark;
		g_log[g_log_n++] = phase;
	}
}

static void fake_layout(Element *self)
{
	fakeLog(uiElementPayload(self), 'l');
}

static void fake_draw(Element *self, UiDrawCtx *ctx)
{
	(void)ctx;
	fakeLog(uiElementPayload(self), 'd');
}

static bool fake_intent(Element *self, UiIntent intent)
{
	(void)intent;
	fakeLog(uiElementPayload(self), 'i');
	return g_intentConsumed;
}

static bool fake_pointer(Element *self, int x, int y)
{
	(void)x;
	(void)y;
	fakeLog(uiElementPayload(self), 't');
	return g_pointerConsumed;
}

/* destroy logs and never recurses: children are framework work. */
static void fake_destroy(Element *self)
{
	fakeLog(uiElementPayload(self), 'x');
}

static const ElementVt fakeVt = {
	fake_layout, fake_draw, fake_intent, fake_pointer,
	fake_destroy, sizeof(FakeState), "fake",
};

/* A vtable with every callback absent (payload still size 1 for marks). */
static const ElementVt bareVt = {
	NULL, NULL, NULL, NULL, NULL, sizeof(FakeState), "bare",
};

static Element *makeFake(char mark)
{
	Element *element = uiCreateElement(&fakeVt);
	FakeState *st;

	TEST_ASSERT_NOT_NULL(element);
	st = uiElementPayload(element);
	st->mark = mark;
	return element;
}

static void resetFakes(void)
{
	memset(g_log, 0, sizeof(g_log));
	g_log_n = 0;
	g_intentConsumed = false;
	g_pointerConsumed = false;
}

/* The log holds mark+phase pairs only: "RxAxBx" etc. */
static void expectLog(const char *expected)
{
	size_t len = strlen(expected);

	TEST_ASSERT_EQUAL_INT((int)len, g_log_n);
	TEST_ASSERT_EQUAL_MEMORY(expected, g_log, len);
}

/* --- creation and payload --------------------------------------------------- */

static void test_create_allocates_payload(void)
{
	Element *element = makeFake('P');
	FakeState *st = uiElementPayload(element);

	TEST_ASSERT_NOT_NULL(st);
	/* The payload is the allocation tail, distinct from the head. */
	TEST_ASSERT_TRUE((const char *)st != (const char *)element);
	TEST_ASSERT_EQUAL_MEMORY("P", &st->mark, 1);
	TEST_ASSERT_EQUAL_STRING("fake", element->vt->name);

	/* Default geometry and weight: zeroed. */
	int x, y, w, h;
	uiGetRect(element, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(0, x);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(0, w);
	TEST_ASSERT_EQUAL_INT(0, h);
	TEST_ASSERT_EQUAL_INT(0, uiWeight(element));
	TEST_ASSERT_NULL(uiParent(element));
	TEST_ASSERT_NULL(uiFirstChild(element));
	TEST_ASSERT_NULL(uiNextSibling(element));

	uiDestroyElement(element);
}

static void test_create_rejects_null_vt(void)
{
	TEST_ASSERT_NULL(uiCreateElement(NULL));
}

/* uiCopyText: bounded, always NUL-terminated, NULL src = empty. */
static void test_copy_text_bounds(void)
{
	char buf[6];

	uiCopyText(buf, sizeof(buf), "abc");
	TEST_ASSERT_EQUAL_STRING("abc", buf);

	uiCopyText(buf, sizeof(buf), "abcdefgh");	/* truncated to 5 */
	TEST_ASSERT_EQUAL_STRING("abcde", buf);

	uiCopyText(buf, sizeof(buf), NULL);
	TEST_ASSERT_EQUAL_STRING("", buf);

	buf[0] = 'x';
	uiCopyText(buf, 0, "abc");	/* cap 0: no write */
	TEST_ASSERT_EQUAL_INT('x', buf[0]);

	uiCopyText(NULL, sizeof(buf), "abc");	/* NULL dst: no crash */
}

/* UI_INTENT_COUNT pins the intent enum size (the input mapping relies on
 * the exact set). */
static void test_intent_enum_layout(void)
{
	TEST_ASSERT_EQUAL_INT(6, (int)UI_INTENT_COUNT);
	TEST_ASSERT_EQUAL_INT(0, (int)UI_NAV_UP);
	TEST_ASSERT_EQUAL_INT(3, (int)UI_NAV_RIGHT);
	TEST_ASSERT_EQUAL_INT(4, (int)UI_ACTIVATE);
	TEST_ASSERT_EQUAL_INT(5, (int)UI_CANCEL);
}

/* --- ownership and tree shape ------------------------------------------------- */

static void test_append_child_ownership_rules(void)
{
	Element *parent = makeFake('P');
	Element *a = makeFake('A');
	Element *b = makeFake('B');

	uiAppendChild(parent, a);
	uiAppendChild(parent, b);

	TEST_ASSERT_EQUAL_PTR(parent, uiParent(a));
	TEST_ASSERT_EQUAL_PTR(parent, uiParent(b));
	TEST_ASSERT_EQUAL_PTR(a, uiFirstChild(parent));
	TEST_ASSERT_EQUAL_PTR(b, uiNextSibling(a));
	TEST_ASSERT_EQUAL_PTR(b, parent->lastChild);
	TEST_ASSERT_NULL(uiNextSibling(b));

	/* Already owned: rejected silently, still parent's child. */
	uiAppendChild(a, b);
	TEST_ASSERT_EQUAL_PTR(parent, uiParent(b));
	TEST_ASSERT_NULL(uiFirstChild(a));

	/* Self-append: rejected (would corrupt the list). */
	uiAppendChild(parent, parent);
	TEST_ASSERT_EQUAL_PTR(a, uiFirstChild(parent));

	/* NULLs: no-ops. */
	uiAppendChild(NULL, a);
	uiAppendChild(parent, NULL);
	TEST_ASSERT_EQUAL_PTR(b, uiNextSibling(a));

	/* Everything is still linked: one destroy covers the tree. */
	uiDestroyElement(parent);
}

/* A child taken out of the tree is unlinked, NOT destroyed; siblings
 * close ranks; removal is re-appendable. */
static void test_remove_child_unlinks_and_keeps_sibling_order(void)
{
	Element *parent = makeFake('P');
	Element *a = makeFake('A');
	Element *b = makeFake('B');
	Element *c = makeFake('C');

	uiAppendChild(parent, a);
	uiAppendChild(parent, b);
	uiAppendChild(parent, c);

	uiRemoveChild(parent, b);
	TEST_ASSERT_EQUAL_PTR(a, uiFirstChild(parent));
	TEST_ASSERT_EQUAL_PTR(c, uiNextSibling(a));
	TEST_ASSERT_NULL(uiNextSibling(c));
	TEST_ASSERT_NULL(uiParent(b));
	TEST_ASSERT_NULL(uiNextSibling(b));

	/* A removed child may be re-appended (elsewhere). */
	uiAppendChild(c, b);
	TEST_ASSERT_EQUAL_PTR(c, uiParent(b));

	/* Not-a-child: no-op (nothing unlinked, no crash). */
	uiRemoveChild(parent, b);
	TEST_ASSERT_EQUAL_PTR(a, uiFirstChild(parent));
	TEST_ASSERT_EQUAL_PTR(b, uiFirstChild(c));	/* b's re-home intact */

	uiDestroyElement(parent);	/* destroys a, c (and b under c) */
}

/* Removing the TAIL must reset the cached lastChild to its predecessor,
 * not NULL: a later append would otherwise dereference a NULL tail. */
static void test_remove_tail_keeps_append_working(void)
{
	Element *parent = makeFake('P');
	Element *a = makeFake('A');
	Element *b = makeFake('B');
	Element *c = makeFake('C');

	uiAppendChild(parent, a);
	uiAppendChild(parent, b);
	uiRemoveChild(parent, b);	/* tail removed: a becomes the tail */
	TEST_ASSERT_EQUAL_PTR(a, parent->lastChild);

	uiAppendChild(parent, c);	/* must land after a, not crash */
	TEST_ASSERT_EQUAL_PTR(a, uiFirstChild(parent));
	TEST_ASSERT_EQUAL_PTR(c, uiNextSibling(a));
	TEST_ASSERT_EQUAL_PTR(c, parent->lastChild);

	/* Removing the only child empties the list cleanly. */
	uiRemoveChild(parent, a);
	uiRemoveChild(parent, c);
	TEST_ASSERT_NULL(uiFirstChild(parent));
	TEST_ASSERT_NULL(parent->lastChild);

	uiDestroyElement(parent);
	uiDestroyElement(a);	/* a and c were detached above */
	uiDestroyElement(c);
	uiDestroyElement(b);
}

/* --- accessors ------------------------------------------------------------------ */

static void test_rect_and_weight_accessors(void)
{
	Element *element = makeFake('R');

	uiSetRect(element, 3, 5, 10, 20);
	int x, y, w, h;
	uiGetRect(element, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(3, x);
	TEST_ASSERT_EQUAL_INT(5, y);
	TEST_ASSERT_EQUAL_INT(10, w);
	TEST_ASSERT_EQUAL_INT(20, h);

	/* NULL out pointers are skipped, others written. */
	x = -1;
	w = -1;
	uiGetRect(element, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(5, y);
	TEST_ASSERT_EQUAL_INT(20, h);
	TEST_ASSERT_EQUAL_INT(-1, x);
	TEST_ASSERT_EQUAL_INT(-1, w);

	uiSetWeight(element, 2);
	TEST_ASSERT_EQUAL_INT(2, uiWeight(element));

	/* NULL endpoints: no crash, no write. */
	x = 3;
	uiSetRect(NULL, 1, 2, 3, 4);
	uiSetWeight(NULL, 5);
	uiGetRect(NULL, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(3, x);
	TEST_ASSERT_NULL(uiParent(NULL));
	TEST_ASSERT_NULL(uiFirstChild(NULL));
	TEST_ASSERT_NULL(uiNextSibling(NULL));

	uiDestroyElement(element);
}

/* --- teardown --------------------------------------------------------------------- */

/* Order: each element's destroy() runs before its subtree dies, first
 * child to last, nothing twice. The log proves the full sequence. */
static void test_destroy_order_self_before_children(void)
{
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *b = makeFake('B');

	uiAppendChild(root, a);
	uiAppendChild(root, b);

	resetFakes();
	uiDestroyElement(root);
	expectLog("RxAxBx");

	/* NULL destroy: no-op. */
	uiDestroyElement(NULL);
}

/* Nested: R > A > C destroys R, then A's subtree (A, then C). */
static void test_destroy_recurses_nested(void)
{
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *c = makeFake('C');

	uiAppendChild(root, a);
	uiAppendChild(a, c);

	resetFakes();
	uiDestroyElement(root);
	expectLog("RxAxCx");
}

/* Destroy with absent callbacks must be safe (no crash on NULL destroy). */
static void test_destroy_null_callbacks(void)
{
	Element *root = uiCreateElement(&bareVt);
	Element *a = uiCreateElement(&bareVt);

	TEST_ASSERT_NOT_NULL(root);
	TEST_ASSERT_NOT_NULL(a);
	uiAppendChild(root, a);
	uiDestroyElement(root);
}

/* Every walk endpoint tolerates NULL without touching memory. */
static void test_null_arguments_are_safe_on_walks(void)
{
	uiLayout(NULL);
	uiDraw(NULL, NULL);
	TEST_ASSERT_FALSE(uiHandleIntent(NULL, UI_ACTIVATE));
	TEST_ASSERT_FALSE(uiHandlePointer(NULL, 1, 1));
	uiRemoveChild(NULL, NULL);
	TEST_ASSERT_NULL(uiElementPayload(NULL));
}

/* --- layout walk -------------------------------------------------------------------- */

static void test_layout_walk_order_and_noop(void)
{
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *b = makeFake('B');

	uiAppendChild(root, a);
	uiAppendChild(root, b);

	resetFakes();
	uiLayout(root);
	expectLog("RlAlBl");

	/* A layout-less root: the walk is still safe, nothing is called. */
	resetFakes();
	Element *bare = uiCreateElement(&bareVt);
	uiLayout(bare);
	TEST_ASSERT_EQUAL_INT(0, g_log_n);

	uiDestroyElement(bare);
	uiDestroyElement(root);
}

/* --- draw walk ------------------------------------------------------------------------ */

static void test_draw_walk_order_and_recording(void)
{
	static TestDrawLog log;
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *b = makeFake('B');

	uiAppendChild(root, a);
	uiAppendChild(root, b);

	resetFakes();
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(root, &ctx);

	/* Draw is IN order: self, then children first to last. */
	expectLog("RdAdBd");
	/* The recorder saw the fakes' calls (they passed the ctx through
	 * and it went nowhere — no fills/texts). */
	TEST_ASSERT_EQUAL_INT(0, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(root);
}

/* A NULL draw ctx is a no-op: nothing to render into. */
static void test_draw_null_ctx_is_noop(void)
{
	Element *root = makeFake('R');

	resetFakes();
	uiDraw(root, NULL);
	TEST_ASSERT_EQUAL_INT(0, g_log_n);
	uiDestroyElement(root);
}

/* --- intent walk --------------------------------------------------------------------------- */

static void test_intent_walk_children_first_reverse_then_self(void)
{
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *b = makeFake('B');

	uiAppendChild(root, a);
	uiAppendChild(root, b);

	/* Nobody consumes: every handler still runs — B's subtree, then
	 * A's, then self (reverse child order, then self). */
	resetFakes();
	TEST_ASSERT_FALSE(uiHandleIntent(root, UI_NAV_DOWN));
	expectLog("BiAiRi");

	/* The LAST child consumes: A and root never see the intent. */
	resetFakes();
	g_intentConsumed = true;
	TEST_ASSERT_TRUE(uiHandleIntent(root, UI_ACTIVATE));
	expectLog("Bi");

	/* On a subtree root the same pattern applies one level down. */
	resetFakes();
	g_intentConsumed = true;
	TEST_ASSERT_TRUE(uiHandleIntent(a, UI_ACTIVATE));
	expectLog("Ai");

	uiDestroyElement(root);
}

/* Absent handlers count as not consuming; the intent reaches self. */
static void test_intent_walk_absent_handlers(void)
{
	Element *root = makeFake('R');
	Element *bare = uiCreateElement(&bareVt);

	uiAppendChild(root, bare);
	resetFakes();
	g_intentConsumed = true;
	TEST_ASSERT_TRUE(uiHandleIntent(root, UI_NAV_UP));
	expectLog("Ri");	/* the bare child consumed nothing */

	uiDestroyElement(root);
}

/* --- pointer walk ---------------------------------------------------------------------------- */

static void test_pointer_walk_deepest_last_first(void)
{
	/* Structure R(A, B(B1)): pointer walk order is B1's subtree,
	 * then B1, then B, then A (no subtree), then R. */
	Element *root = makeFake('R');
	Element *a = makeFake('A');
	Element *b = makeFake('B');
	Element *b1 = makeFake('1');

	uiAppendChild(root, a);
	uiAppendChild(root, b);
	uiAppendChild(b, b1);

	resetFakes();
	TEST_ASSERT_FALSE(uiHandlePointer(root, 10, 10));
	expectLog("1tBtAtRt");

	/* The deepest element consumes; nothing else is visited. */
	resetFakes();
	g_pointerConsumed = true;
	TEST_ASSERT_TRUE(uiHandlePointer(root, 999, 0));
	expectLog("1t");

	uiDestroyElement(root);
}

static void test_pointer_walk_single_child(void)
{
	Element *root = makeFake('R');
	Element *a = makeFake('A');

	uiAppendChild(root, a);
	resetFakes();
	TEST_ASSERT_FALSE(uiHandlePointer(root, 0, 0));
	expectLog("AtRt");
	uiDestroyElement(root);
}

void run_test_element(void);

void run_test_element(void)
{
	RUN_TEST(test_create_allocates_payload);
	RUN_TEST(test_create_rejects_null_vt);
	RUN_TEST(test_copy_text_bounds);
	RUN_TEST(test_intent_enum_layout);
	RUN_TEST(test_append_child_ownership_rules);
	RUN_TEST(test_remove_child_unlinks_and_keeps_sibling_order);
	RUN_TEST(test_remove_tail_keeps_append_working);
	RUN_TEST(test_rect_and_weight_accessors);
	RUN_TEST(test_destroy_order_self_before_children);
	RUN_TEST(test_destroy_recurses_nested);
	RUN_TEST(test_destroy_null_callbacks);
	RUN_TEST(test_null_arguments_are_safe_on_walks);
	RUN_TEST(test_layout_walk_order_and_noop);
	RUN_TEST(test_draw_walk_order_and_recording);
	RUN_TEST(test_draw_null_ctx_is_noop);
	RUN_TEST(test_intent_walk_children_first_reverse_then_self);
	RUN_TEST(test_intent_walk_absent_handlers);
	RUN_TEST(test_pointer_walk_deepest_last_first);
	RUN_TEST(test_pointer_walk_single_child);
}
