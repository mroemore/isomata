/*
 * Element base tree: vtable dispatch, rect/weight state, and the plain
 * child list. See element.h for the invariant block (ownership rules,
 * walk orders, optional callbacks).
 *
 * The tree is an intrusive singly-linked child list: firstChild walks
 * nextSibling; lastChild is cached for O(1) append (and for reverse
 * iteration, which descends via nextSibling anyway). All mutations are
 * guarded against NULLs and against re-parenting an already-owned
 * element, so no sequence of these calls can corrupt the list.
 */

#include "ui/element.h"

#include <stdlib.h>

/* --- lifecycle ---------------------------------------------------------------- */

Element *uiCreateElement(const ElementVt *vt)
{
	Element *element;

	if (vt == NULL)
		return NULL;
	/* One allocation: the head plus the inline payload, zeroed
	 * (defaults: rect 0, weight 0, no children, NULL links). */
	element = calloc(1, sizeof(*element) + vt->size);
	if (element == NULL)
		return NULL;
	element->vt = vt;
	return element;
}

void *uiElementPayload(const Element *element)
{
	if (element == NULL)
		return NULL;
	/* Zero-size payload: valid address arithmetic, never dereferenced
	 * (same rule as scenePayload). */
	return (void *)element->_inline;
}

void uiCopyText(char *dst, size_t cap, const char *src)
{
	size_t i = 0;

	if (dst == NULL || cap == 0)
		return;
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	while (i + 1 < cap && src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

/* Depth-first, self (destroy()) before children, then free — the process
 * traversal of uiHandlePointer keys on; recursion depth is bounded by
 * the tree the caller built. */
static void destroyElementRecursive(Element *element)
{
	Element *child;

	if (element == NULL)
		return;
	/* The child walk does not touch the parent afterwards, so an a
	 * destroy() that clears parent pointers stays memory-safe. */
	if (element->vt->destroy != NULL)
		element->vt->destroy(element);

	child = element->firstChild;
	while (child != NULL) {
		Element *next = child->nextSibling;

		destroyElementRecursive(child);
		child = next;
	}
	free(element);
}

void uiDestroyElement(Element *element)
{
	if (element == NULL)
		return;
	destroyElementRecursive(element);
}

/* --- tree mutation ---------------------------------------------------------------- */

void uiAppendChild(Element *parent, Element *child)
{
	if (parent == NULL || child == NULL)
		return;
	if (child == parent)
		return;		/* self-append would corrupt the list */
	if (child->parent != NULL)
		return;		/* already owned: caller keeps it */

	child->parent = parent;
	child->nextSibling = NULL;
	if (parent->firstChild == NULL) {
		parent->firstChild = child;
		parent->lastChild = child;
		return;
	}
	parent->lastChild->nextSibling = child;
	parent->lastChild = child;
}

/* Linear scan for the child's predecessor; the list is short and this is
 * the only unlink path, so no doubly-linked overhead is paid. */
void uiRemoveChild(Element *parent, Element *child)
{
	Element **link;
	Element *prev = NULL;

	if (parent == NULL || child == NULL)
		return;

	/* Unlink via the link to child (firstChild or some sibling's
	 * nextSibling); no-op if child is not actually a child. When the
	 * removed child was the tail, lastChild falls back to its
	 * predecessor (NULL only when the list is now empty) — leaving the
	 * cached tail dangling would crash the next append. */
	link = &parent->firstChild;
	while (*link != NULL) {
		if (*link == child) {
			if (parent->lastChild == child)
				parent->lastChild = prev;
			*link = child->nextSibling;
			child->parent = NULL;
			child->nextSibling = NULL;
			return;
		}
		prev = *link;
		link = &(*link)->nextSibling;
	}
}

Element *uiParent(const Element *element)
{
	if (element == NULL)
		return NULL;
	return element->parent;
}

Element *uiFirstChild(const Element *element)
{
	if (element == NULL)
		return NULL;
	return element->firstChild;
}

Element *uiNextSibling(const Element *element)
{
	if (element == NULL)
		return NULL;
	return element->nextSibling;
}

/* --- geometry ------------------------------------------------------------------------ */

void uiSetRect(Element *element, int x, int y, int w, int h)
{
	if (element == NULL)
		return;
	element->x = x;
	element->y = y;
	element->w = w;
	element->h = h;
}

void uiGetRect(const Element *element, int *x, int *y, int *w, int *h)
{
	if (element == NULL)
		return;
	if (x != NULL)
		*x = element->x;
	if (y != NULL)
		*y = element->y;
	if (w != NULL)
		*w = element->w;
	if (h != NULL)
		*h = element->h;
}

void uiSetWeight(Element *element, int weight)
{
	if (element == NULL)
		return;
	element->weight = weight;
}

int uiWeight(const Element *element)
{
	if (element == NULL)
		return 0;
	return element->weight;
}

/* --- frame walks -------------------------------------------------------------------------- */

void uiLayout(Element *root)
{
	Element *child;

	if (root == NULL)
		return;
	/* Self arranges children; the walk then runs top-down so nested
	 * panes see their fresh rects. */
	if (root->vt->layout != NULL)
		root->vt->layout(root);
	for (child = root->firstChild; child != NULL; child = child->nextSibling)
		uiLayout(child);
}

void uiDraw(Element *root, UiDrawCtx *ctx)
{
	Element *child;

	if (root == NULL || ctx == NULL)
		return;
	if (root->vt->draw != NULL)
		root->vt->draw(root, ctx);
	for (child = root->firstChild; child != NULL; child = child->nextSibling)
		uiDraw(child, ctx);
}

/* --- input walks -------------------------------------------------------------------------

 * Both walks share one order: an element's CHILDREN run first, each
 * whole subtree (deepest/last first), then the element itself. The
 * first consumer wins and starves everything unvisited. The two phase
 * helpers below implement exactly that: inputSiblingPass iterates a
 * sibling chain from the tail, inputElementPass runs the children pass
 * then self's own handler (an absent handler never consumes).
 */

/* Later siblings first, then this child's subtree+self. */
static bool inputElementPass(Element *element, bool intent, int x, int y,
			     UiIntent intentArg);

static bool inputSiblingPass(Element *child, bool intent, int x, int y,
			     UiIntent intentArg)
{
	if (child == NULL)
		return false;
	if (inputSiblingPass(child->nextSibling, intent, x, y, intentArg))
		return true;
	return inputElementPass(child, intent, x, y, intentArg);
}

/* The children pass (reverse order), then self's own handler. */
static bool inputElementPass(Element *element, bool intent, int x, int y,
			     UiIntent intentArg)
{
	if (element == NULL)
		return false;

	if (inputSiblingPass(element->firstChild, intent, x, y, intentArg))
		return true;

	if (intent)
		return element->vt->handleIntent != NULL &&
		       element->vt->handleIntent(element, intentArg);
	return element->vt->handlePointer != NULL &&
	       element->vt->handlePointer(element, x, y);
}

bool uiHandleIntent(Element *root, UiIntent intent)
{
	bool consumed;

	if (root == NULL)
		return false;
	consumed = inputSiblingPass(root->firstChild, true, 0, 0, intent);
	if (consumed)
		return true;
	return root->vt->handleIntent != NULL &&
	       root->vt->handleIntent(root, intent);
}

bool uiHandlePointer(Element *root, int x, int y)
{
	bool consumed;

	if (root == NULL)
		return false;
	consumed = inputSiblingPass(root->firstChild, false, x, y, UI_NAV_UP);
	if (consumed)
		return true;
	return root->vt->handlePointer != NULL &&
	       root->vt->handlePointer(root, x, y);
}

