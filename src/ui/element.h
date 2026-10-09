#ifndef ISOMATA_UI_ELEMENT_H
#define ISOMATA_UI_ELEMENT_H

/*
 * Vtable-driven UI elements and the plain tree that arranges, draws and
 * routes input to them. This module is headless: no SDL header is
 * included here or in element.c, and rendering goes through an abstract
 * draw context so tests can record calls instead of pixels.
 *
 * Elements (vtable layout, like scene.h):
 *
 * - An Element is a vtable pointer plus tree bookkeeping (rect, weight,
 *   parent/children links) plus an inline payload of ElementVt::size
 *   bytes, zeroed at creation; uiElementPayload() returns it. Concrete
 *   elements (pane, label, button, menu, overlay, toast) own their
 *   payload structs and their vtables; this file only defines the base.
 * - The "shape" model: an element's geometry is exactly its rect
 *   [x, y, w, h] in virtual (logical UI) pixels. Nothing here knows about
 *   DPI — see ui_scale.h — and how a parent assigns child rects is up to
 *   the parent's layout() callback.
 * - ElementVt callbacks, each optional (an absent entry is a safe
 *   no-op; handleIntent/handlePointer absent = never consumes):
 *     layout       arrange SELF'S CHILDREN within self's rect. Callees
 *                  may assume children's rects are set when their own
 *                  layout() runs (one uiLayout() pass is enough for a
 *                  top-down layout).
 *     draw         draw SELF (background, text, ...). CHILDREN ARE NOT
 *                  DRAWN HERE: the uiDraw walk calls each element's
 *                  draw. Children draw after self, in order.
 *     handleIntent UI-semantic intent (UiIntent), NOT a device command:
 *                  the input layer defines its own Command enum and maps
 *                  Command -> UiIntent at the integration point; ui/
 *                  never depends on input/. Returns consumed?
 *     handlePointer tap at virtual coords (a translated touch/click);
 *                  returns consumed? Handlers hit-test against their own
 *                  rect; the framework does not clip.
 *     destroy      element-private teardown (free owned buffers). The
 *                  subtree walk and the free itself are framework work;
 *                  a destroy callback runs BEFORE children die, but must
 *                  not touch them, free self, or outlive the call.
 * - Ownership: uiCreateElement hands the caller an owned element.
 *   uiAppendChild transfers ownership to the parent for as long as the
 *   child stays linked; uiRemoveChild hands it back to the caller
 *   (unlinked only, subtree intact); uiDestroyElement destroys the whole
 *   subtree. A child that is already owned (child->parent != NULL, which
 *   uiRemoveChild clears) is REJECTED by uiAppendChild: nothing happens,
 *   the caller keeps ownership, no hidden re-parenting. This rules out
 *   corrupt lists and double-destroy sequences, like the scene stack's
 *   aliasing guard rules out double-push.
 * - Bounded state is a per-element choice of the concrete module (fixed
 *   caps inside inline payloads); the base tree itself is unbounded in
 *   depth/degree, like the scene stack is per-stack bounded.
 * - Walk order:
 *     uiDraw    self, then children in order, depth first.
 *     uiLayout  self's layout(), then children top-down.
 *     uiHandleIntent / uiHandlePointer
 *               last subtree first: for each child from LAST to FIRST
 *               its whole subtree, then the child itself, then self; the
 *               first consumer wins and starves everything unvisited.
 *
 * Color format throughout ui/: 0xRRGGBBAA, alpha in the LEAST
 * significant byte (each byte 0..255).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- text seam ---------------------------------------------------------------
 * Measurement is an injected seam so every ui module stays headless-
 * testable: pure tests use a fake measure (fixed size per char), the SDL
 * tier (ui_font.h) supplies a real one built on SDL3_ttf. The callback
 * returns 0 on success and writes the size through its out pointers; a
 * nonzero return means "no measurement" and callers fall back to zero
 * size (as they do when fn is NULL). A TextStyle with measure.fn == NULL
 * measures zero-size — draw code falls back to zero and layouts must not
 * rely on measure results. */

typedef struct UiFont UiFont;

typedef struct TextMeasure {
	int (*fn)(void *ctx, const char *text, int *w, int *h);	/* 0 = ok */
	void *ctx;
} TextMeasure;

typedef struct TextStyle {
	UiFont *font;		/* render-time font; NULL in pure tests */
	TextMeasure measure;	/* measurement seam (fn NULL = no measure) */
	int pixelSize;		/* design pixel size; fallback when unmeasured */
} TextStyle;

/* --- icons (the image draw seam) ----------------------------------------------
 * A small fixed vocabulary of UI icons. The pure layer only NAMES them; the
 * SDL tier (ui_gpu.c) maps each to a texture it loaded from assets/icons/.
 * UI_ICON_COUNT is not a drawable icon: it is the "no icon" sentinel used to
 * clear a button back to text mode, and the array bound. */

typedef enum {
	UI_ICON_ROTATE_CCW = 0,
	UI_ICON_ROTATE_CW,
	UI_ICON_RESTORE,
	UI_ICON_BULB,
	UI_ICON_BULB_OFF,
	UI_ICON_COUNT,
} UiIcon;

/* --- abstract draw context ------------------------------------------------------
 * The SDL_gpu implementation arrives with the render task; headless tests
 * implement a recording ctx and assert on the recorded calls. */

typedef struct UiDrawCtxVt UiDrawCtxVt;
typedef struct UiDrawCtx UiDrawCtx;

struct UiDrawCtxVt {
	void (*fillRect)(UiDrawCtx *ctx, int x, int y, int w, int h,
			 uint32_t rgba);
	void (*drawText)(UiDrawCtx *ctx, int x, int y, const char *text,
			 const struct TextStyle *style, uint32_t rgba);
	/* Draw `icon` into the virtual rect (x,y,w,h), tinted by `tint`
	 * (0xRRGGBBAA). The caller sizes the rect; the SDL tier maps the
	 * icon to a nearest-sampled texture and scales it to fill. OPTIONAL:
	 * a ctx that carries no icons may leave this NULL (an icon-bearing
	 * button then skips the image and still fills its rect). */
	void (*drawImage)(UiDrawCtx *ctx, int x, int y, int w, int h,
			  UiIcon icon, uint32_t tint);
};

struct UiDrawCtx {
	const UiDrawCtxVt *vt;
};

/* --- intents ------------------------------------------------------------------ */

typedef enum {
	UI_NAV_UP = 0,
	UI_NAV_DOWN,
	UI_NAV_LEFT,
	UI_NAV_RIGHT,
	UI_ACTIVATE,
	UI_CANCEL,
	UI_INTENT_COUNT,
} UiIntent;

/* --- palette ------------------------------------------------------------------ */

#define UI_COLOR_TEXT		0xFFFFFFFFu	/* text, opaque white */
#define UI_COLOR_BACKGROUND	0x303030E6u	/* pane fill + menu rows */
#define UI_COLOR_BUTTON		0x505050E6u	/* button fill */
#define UI_COLOR_SELECTION	0x4090FFE6u	/* menu selected row fill */
#define UI_COLOR_OVERLAY	0x00000099u	/* dim backdrop fill */

/* Fixed text capacity shared by every text-bearing element (label, button,
 * menu item, toast). Stored inline, NUL-terminated; longer input is
 * truncated on a byte boundary and never overruns. */
#define UI_TEXT_MAX		128

/* --- element base ---------------------------------------------------------------- */

typedef struct ElementVt ElementVt;
typedef struct Element Element;

struct ElementVt {
	void (*layout)(Element *self);		/* arrange CHILDREN; may be NULL */
	void (*draw)(Element *self, UiDrawCtx *ctx);
	bool (*handleIntent)(Element *self, UiIntent intent);	/* consumed? */
	bool (*handlePointer)(Element *self, int x, int y);	/* tap; consumed? */
	void (*destroy)(Element *self);		/* private teardown only */
	size_t size;		/* inline payload bytes; 0 = stateless */
	const char *name;	/* diagnostics; may be NULL */
};

struct Element {
	const ElementVt *vt;
	int x;
	int y;
	int w;
	int h;
	int weight;		/* layout weight, default 0 (see layout.h) */
	struct Element *parent;
	struct Element *firstChild;
	struct Element *lastChild;
	struct Element *nextSibling;
	char _inline[];		/* ElementVt::size bytes of instance state */
};

/* Element lifecycle. uiCreateElement allocates sizeof(Element) +
 * ElementVt::size and zeroes it; populating the payload is up to the
 * concrete element's creator. vt == NULL returns NULL. */
Element *uiCreateElement(const ElementVt *vt);

/* The inline tail (never NULL for a non-NULL element; for a zero-size
 * vtable it is valid address arithmetic that must never be
 * dereferenced, like scenePayload). */
void *uiElementPayload(const Element *element);

/* Bounded, guaranteed-NUL-terminated string copy into caller storage:
 * copies at most cap-1 bytes of src and always terminates when cap > 0.
 * src == NULL is stored as empty. cap == 0 or dst == NULL writes
 * nothing. The shared primitive behind every text-bearing element's
 * UI_TEXT_MAX buffer. */
void uiCopyText(char *dst, size_t cap, const char *src);

/* Recursive destroy: destroy() then the subtree, then free. NULL-safe. */
void uiDestroyElement(Element *element);

/* --- tree ---------------------------------------------------------------------
 * See the ownership note above for who owns what after each call.
 * All endpoints tolerate NULL (no-ops). */

/* Unlinks child (must be unowned) and appends it as parent's last child. */
void uiAppendChild(Element *parent, Element *child);

/* Unlinks child from parent if it actually is a child; ownership returns
 * to the caller, subtree intact. A child that is not a child of parent
 * leaves the tree untouched. */
void uiRemoveChild(Element *parent, Element *child);

Element *uiParent(const Element *element);
Element *uiFirstChild(const Element *element);
Element *uiNextSibling(const Element *element);

/* --- geometry -----------------------------------------------------------------
 * Rect in virtual pixels; NULL out pointers are skipped. */
void uiSetRect(Element *element, int x, int y, int w, int h);
void uiGetRect(const Element *element, int *x, int *y, int *w, int *h);

/* Layout weight; default 0 = fixed-size child (see layout.h). Negative
 * weights are treated as 0 by the pane layout but stored as given. */
void uiSetWeight(Element *element, int weight);
int uiWeight(const Element *element);

/* --- frame walks --------------------------------------------------------------
 * uiLayout: root's layout() first, then each child's subtree top-down
 * (a parent assigns child rects; children then arrange their own).
 * uiDraw: root's draw() first, then children in order. NULL-safe
 * (NULL root or NULL ctx is a no-op). */
void uiLayout(Element *root);
void uiDraw(Element *root, UiDrawCtx *ctx);

/* Input walks: children first in REVERSE order, then self's own
 * handler; the first consumer wins. NULL root returns false. */
bool uiHandleIntent(Element *root, UiIntent intent);
bool uiHandlePointer(Element *root, int x, int y);

#endif /* ISOMATA_UI_ELEMENT_H */
