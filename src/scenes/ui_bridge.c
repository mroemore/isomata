/*
 * Command -> UiIntent bridge (see ui_bridge.h for the contract). The only
 * Command-to-UiIntent mapping in the engine; input/ and ui/ stay mutually
 * unaware.
 */

#include "scenes/ui_bridge.h"

/* Map a device command to a UI intent. Returns false for commands the UI
 * does not speak (rotate/zoom/unknown). */
static bool commandToIntent(Command command, UiIntent *out)
{
	switch (command) {
	case CMD_NAV_UP:
		*out = UI_NAV_UP;
		return true;
	case CMD_NAV_DOWN:
		*out = UI_NAV_DOWN;
		return true;
	case CMD_NAV_LEFT:
		*out = UI_NAV_LEFT;
		return true;
	case CMD_NAV_RIGHT:
		*out = UI_NAV_RIGHT;
		return true;
	case CMD_SELECT:
		*out = UI_ACTIVATE;
		return true;
	case CMD_BACK:
		*out = UI_CANCEL;
		return true;
	default:
		return false;
	}
}

bool uiBridgeDispatch(Element *root, const InputFrame *frame)
{
	int i;

	if (frame == NULL)
		return false;

	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++) {
		UiIntent intent;

		if (commandToIntent(frame->commands[i], &intent) &&
		    uiHandleIntent(root, intent))
			return true;
	}

	if (frame->tap && uiHandlePointer(root, frame->tapX, frame->tapY))
		return true;
	return false;
}

bool uiBridgeFrameHasBack(const InputFrame *frame)
{
	int i;

	if (frame == NULL)
		return false;
	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++)
		if (frame->commands[i] == CMD_BACK)
			return true;
	return false;
}
