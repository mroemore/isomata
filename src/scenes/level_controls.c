/*
 * Level controls: pure construction + layout of the four on-screen buttons.
 * See level_controls.h for the layout rule and the icon assignment.
 */

#include "scenes/level_controls.h"

bool levelControlsCreate(LevelControls *controls, Element *root,
			 const TextStyle *style, UiActionFn onRotL,
			 UiActionFn onRotR, UiActionFn onDebug,
			 UiActionFn onReset, void *ctx)
{
	Element *rotL;
	Element *rotR;
	Element *debug;
	Element *reset;

	if (controls == NULL || root == NULL)
		return false;

	/* All-or-nothing: create every button before any is appended, so a
	 * partial failure frees exactly what it created (root is untouched). */
	rotL = uiCreateButton(NULL, style, onRotL, ctx);
	rotR = uiCreateButton(NULL, style, onRotR, ctx);
	debug = uiCreateButton(NULL, style, onDebug, ctx);
	reset = uiCreateButton(NULL, style, onReset, ctx);
	if (rotL == NULL || rotR == NULL || debug == NULL || reset == NULL) {
		uiDestroyElement(rotL);
		uiDestroyElement(rotR);
		uiDestroyElement(debug);
		uiDestroyElement(reset);
		return false;
	}

	uiButtonSetIcon(rotL, UI_ICON_ROTATE_CCW);
	uiButtonSetIcon(rotR, UI_ICON_ROTATE_CW);
	uiButtonSetIcon(debug, UI_ICON_BULB_OFF);
	uiButtonSetIcon(reset, UI_ICON_RESTORE);

	uiAppendChild(root, rotL);
	uiAppendChild(root, rotR);
	uiAppendChild(root, debug);
	uiAppendChild(root, reset);

	controls->rotL = rotL;
	controls->rotR = rotR;
	controls->debug = debug;
	controls->reset = reset;
	return true;
}

void levelControlsLayout(const LevelControls *controls, int sx, int sy, int sw,
			 int sh)
{
	int rowY;
	int rotRight;
	int resetX;
	int debugX;

	if (controls == NULL)
		return;

	rowY = sy + sh - LEVEL_BUTTON_MARGIN - LEVEL_BUTTON_H;
	uiSetRect(controls->rotL, sx + LEVEL_BUTTON_MARGIN, rowY, LEVEL_BUTTON_W,
		  LEVEL_BUTTON_H);
	uiSetRect(controls->rotR,
		  sx + LEVEL_BUTTON_MARGIN + LEVEL_BUTTON_W + LEVEL_BUTTON_GAP,
		  rowY, LEVEL_BUTTON_W, LEVEL_BUTTON_H);

	/* Right cluster: RESET hugs the right edge, DEBUG one gap to its
	 * left. rotRight is the right edge of the bottom-left cluster. */
	resetX = sx + sw - LEVEL_BUTTON_MARGIN - LEVEL_BUTTON_W;
	debugX = resetX - LEVEL_BUTTON_GAP - LEVEL_BUTTON_W;
	rotRight = sx + LEVEL_BUTTON_MARGIN + LEVEL_BUTTON_W + LEVEL_BUTTON_GAP +
		   LEVEL_BUTTON_W;

	if (debugX < rotRight + LEVEL_BUTTON_GAP) {
		/* Narrow: the whole right pair rides a second row above. */
		int secondRowY = rowY - LEVEL_BUTTON_H - LEVEL_BUTTON_GAP;

		uiSetRect(controls->debug, debugX, secondRowY, LEVEL_BUTTON_W,
			  LEVEL_BUTTON_H);
		uiSetRect(controls->reset, resetX, secondRowY, LEVEL_BUTTON_W,
			  LEVEL_BUTTON_H);
	} else {
		uiSetRect(controls->debug, debugX, rowY, LEVEL_BUTTON_W,
			  LEVEL_BUTTON_H);
		uiSetRect(controls->reset, resetX, rowY, LEVEL_BUTTON_W,
			  LEVEL_BUTTON_H);
	}
}

void levelControlsSetDebugIcon(const LevelControls *controls, bool lightDebug)
{
	if (controls == NULL)
		return;
	uiButtonSetIcon(controls->debug,
			lightDebug ? UI_ICON_BULB : UI_ICON_BULB_OFF);
}

void levelControlsToggleDebug(const LevelControls *controls, bool *lightDebug)
{
	if (lightDebug == NULL)
		return;
	*lightDebug = !*lightDebug;
	levelControlsSetDebugIcon(controls, *lightDebug);
}
