/* isomata/src/input/input_sdl.h — the SDL glue for the pure input core.
 *
 * The only input header that includes SDL: input_sdl.c is app-target-only
 * (engine_sources), never isomata_pure. It translates SDL_Event into the
 * core calls in input.h; all gesture/mapping logic stays in the pure core.
 */
#ifndef ISOMATA_INPUT_SDL_H
#define ISOMATA_INPUT_SDL_H

#include "input/input.h"

#include <SDL3/SDL.h>

/* Feed one SDL event into the frame's input accumulators (between
 * inputBeginFrame and inputEndFrame). NULL input/event is a no-op. */
void inputHandleSdlEvent(Input *input, const SDL_Event *event);

#endif /* ISOMATA_INPUT_SDL_H */
