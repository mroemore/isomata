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
 *   consumed intent ends the COMMAND dispatch (ui's own first-consumer-wins
 *   rule).
 * - A frame tap is routed through uiHandlePointer(root, tapX, tapY)
 *   UNCONDITIONALLY, after the command loop: a tap is a separate gesture, so
 *   a command consumed earlier in the frame does not swallow it.
 * - Returns true when the UI consumed any of the frame's input (a command or
 *   the tap), false when nothing was consumed (or for a NULL frame; a NULL
 *   root consumes nothing).
 */
#include "input/input.h"
#include "ui/element.h"

#include <stdbool.h>

bool uiBridgeDispatch(Element *root, const InputFrame *frame);

/* Pointer-only dispatch: route ONLY the frame's tap through
 * uiHandlePointer(root, tapX, tapY); every command is ignored. Returns
 * whether the tap was consumed (false for a NULL frame, no tap, or a NULL
 * root).
 *
 * Why a scene uses this instead of uiBridgeDispatch: the level's on-screen
 * buttons are touch/click controls with NO focus model, and the keyboard
 * already owns rotate/reset (Q/E/R). Routing UI_ACTIVATE there would fire an
 * arbitrary sibling (ui walks children in reverse order, first consumer
 * wins), so Enter/Space would activate the last button. A scene whose UI is
 * pointer-driven calls this and keeps its own command handling. */
bool uiBridgeDispatchPointer(Element *root, const InputFrame *frame);

/* True when the frame carries CMD_BACK. The UI never consumes UI_CANCEL, so
 * a scene uses this ONLY as the fallback after uiBridgeDispatch reported the
 * frame was not consumed (a [CMD_SELECT, CMD_BACK] frame is consumed by the
 * SELECT activation and must not be acted on again). NULL frame is false. */
bool uiBridgeFrameHasBack(const InputFrame *frame);

#endif /* ISOMATA_SCENES_UI_BRIDGE_H */
