#ifndef ISOMATA_UI_TOAST_H
#define ISOMATA_UI_TOAST_H

/*
 * Toast: a transient notification with a hold-then-fade lifetime.
 * Headless (no SDL); see element.h for the tree and the text seam.
 *
 * Invariants:
 * - State machine: HIDDEN -> SHOWING (hold) -> FADING -> HIDDEN.
 *   toastShow moves to SHOWING and zeroes the timer whether or not a
 *   toast was already showing (a second show REPLACES the text and
 *   restarts the hold).
 * - Time constants are pinned: TOAST_HOLD_SECONDS (3.0) in SHOWING, then
 *   TOAST_FADE_SECONDS (0.5) in FADING. The hold->fade crossing carries
 *   the surplus dt into the fade (so a single large dt can pass through
 *   both phases in one update).
 * - toastUpdate ignores dt that is non-positive or non-finite (no rewind,
 *   no NaN); updating a hidden toast is a no-op. Reaching the end of the
 *   fade hides the toast and resets the timer.
 * - toastAlpha: 0 when hidden, 1 while showing, and 1 - elapsed/fade
 *   clamped to [0, 1] while fading. toastVisible is true for SHOWING and
 *   FADING.
 * - Text lives inline (UI_TEXT_MAX, truncated safely; NULL = empty).
 *   draw() is a no-op while hidden or at zero alpha; otherwise it fills
 *   the rect with UI_COLOR_BACKGROUND and draws the text at the rect's
 *   top-left in UI_COLOR_TEXT, both with their alpha byte scaled by
 *   toastAlpha (empty text draws no text call).
 */

#include "ui/element.h"

#define TOAST_HOLD_SECONDS 3.0f
#define TOAST_FADE_SECONDS 0.5f

Element *uiCreateToast(const TextStyle *style);

/* Show/replace the text and restart the hold. NULL toast = no-op. */
void toastShow(Element *toast, const char *text);

/* Advance the state machine by dt seconds. NULL/ignored dt = no-op. */
void toastUpdate(Element *toast, float dt);

/* Current opacity in [0, 1]; 0 for NULL or hidden. */
float toastAlpha(const Element *toast);

/* True while SHOWING or FADING; false for NULL or hidden. */
bool toastVisible(const Element *toast);

#endif /* ISOMATA_UI_TOAST_H */
