#ifndef ISOMATA_SCENES_LEVEL_CONTROLS_H
#define ISOMATA_SCENES_LEVEL_CONTROLS_H

/*
 * The level's four on-screen controls: construction, layout and icon
 * wiring. Headless — ui/element.h + ui/element_button.h only, no SDL. It
 * lives in its own pure module (rather than inside level_scene.c, which is
 * the SDL tier) so the layout rule and the icon assignment are pinned by
 * the headless suite.
 *
 * Layout (virtual px, inside the caller's safe area [sx,sy,sw,sh]):
 *   - bottom-left cluster : [ROT L][ROT R], inset LEVEL_BUTTON_MARGIN from
 *     the left/bottom edges, LEVEL_BUTTON_GAP between.
 *   - bottom-right cluster: [DEBUG][RESET], RESET hugging the safe-area
 *     right edge and DEBUG exactly one gap to its left.
 *   - narrow-width fallback: when the right cluster's left edge would cross
 *     into ROT R (debugX < rotRight + gap) BOTH right buttons move up to a
 *     second row as a unit — right-aligned, same gap, one button row above
 *     the bottom cluster — so nothing overlaps and every button stays
 *     inside the safe area. At desktop widths the arm is not taken and
 *     RESET's rect is byte-identical to the pre-debug layout.
 *
 * Icons (all icon-only; no text): ROT L = UI_ICON_ROTATE_CCW,
 * ROT R = UI_ICON_ROTATE_CW, RESET = UI_ICON_RESTORE, DEBUG = UI_ICON_BULB
 * when the light-debug flag is on else UI_ICON_BULB_OFF.
 */

#include "ui/element.h"
#include "ui/element_button.h"

#include <stdbool.h>

#define LEVEL_BUTTON_W 96
#define LEVEL_BUTTON_H 48	/* >= 44 virtual px, touch-friendly */
#define LEVEL_BUTTON_MARGIN 16	/* inset from the safe-area edges */
#define LEVEL_BUTTON_GAP 8	/* between paired buttons */

typedef struct LevelControls {
	Element *rotL;		/* rotate counter-clockwise */
	Element *rotR;		/* rotate clockwise */
	Element *debug;		/* toggle the light-debug view (== F key) */
	Element *reset;		/* restore the camera */
} LevelControls;

/* Create the four icon-only buttons and append them to root in draw order
 * ROT L, ROT R, DEBUG, RESET. The fixed icons are set here and DEBUG starts
 * at UI_ICON_BULB_OFF (call levelControlsSetDebugIcon with the real flag).
 * Returns false on allocation failure, destroying whatever it created;
 * callers must not use the struct after a false return. */
bool levelControlsCreate(LevelControls *controls, Element *root,
			 const TextStyle *style, UiActionFn onRotL,
			 UiActionFn onRotR, UiActionFn onDebug,
			 UiActionFn onReset, void *ctx);

/* Assign the four rects inside the safe area. Pure geometry: deterministic
 * for a given [sx,sy,sw,sh]. NULL controls is a no-op. */
void levelControlsLayout(const LevelControls *controls, int sx, int sy, int sw,
			 int sh);

/* Point the DEBUG button's icon at the flag: BULB on, BULB_OFF off.
 * NULL-safe. */
void levelControlsSetDebugIcon(const LevelControls *controls, bool lightDebug);

/* Flip *lightDebug and keep the DEBUG icon in step. The single path shared
 * by the CMD_TOGGLE_LIGHT_DEBUG handler (the F key) and the DEBUG button's
 * callback. NULL flag is a no-op (the icon is not touched). */
void levelControlsToggleDebug(const LevelControls *controls, bool *lightDebug);

#endif /* ISOMATA_SCENES_LEVEL_CONTROLS_H */
