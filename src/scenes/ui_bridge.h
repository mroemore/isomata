#ifndef ISOMATA_SCENES_UI_BRIDGE_H
#define ISOMATA_SCENES_UI_BRIDGE_H

/*
 * Command -> UiIntent bridge at the scene/UI boundary. Pure: it includes
 * only input.h and element.h (both pure), so it lives in isomata_pure with
 * a 1:1 twin test and never pulls SDL into the headless suite.
 *
 * Why a bridge at all: ui/ speaks UI-semantic intents (UiIntent) and input/
 * speaks device-independent commands (Command); neither knows the other.
 * This module is the ONE place that maps between them (Task 5 ruling (b)),
 * so a scene can feed a frame's input to its UI tree with a single call.
 *
 * Contract:
 * - Commands map: NAV_UP/DOWN/LEFT/RIGHT -> UI_NAV_*; SELECT -> UI_ACTIVATE;
 *   BACK -> UI_CANCEL. Every other command (rotate, zoom) is ignored — those
 *   belong to the scene's own control handling, not the UI.
 * - Commands are routed in frame order through uiHandleIntent; the first
 *   consumed intent ends the dispatch (ui's own first-consumer-wins rule),
 *   and the call returns true.
 * - A frame tap is then routed through uiHandlePointer(root, tapX, tapY).
 * - Returns true when the UI consumed any of the frame's input, false when
 *   nothing was consumed (or for a NULL frame; a NULL root consumes
 *   nothing).
 */
#include "input/input.h"
#include "ui/element.h"

#include <stdbool.h>

bool uiBridgeDispatch(Element *root, const InputFrame *frame);

/* True when the frame carries CMD_BACK. The UI never consumes UI_CANCEL, so
 * a scene uses this ONLY as the fallback after uiBridgeDispatch reported the
 * frame was not consumed (a [CMD_SELECT, CMD_BACK] frame is consumed by the
 * SELECT activation and must not be acted on again). NULL frame is false. */
bool uiBridgeFrameHasBack(const InputFrame *frame);

#endif /* ISOMATA_SCENES_UI_BRIDGE_H */
