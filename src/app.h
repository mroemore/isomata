/* isomata/src/app.h — the application shell.
 *
 * Invariants (dotzy-style header contract):
 *   - App owns the SDL window and drives the frame loop; it is the only
 *     owner of SDL lifecycle init/quit for the process.
 *   - appCreate() must run before appRun()/appDestroy(); no call is
 *     re-entrant and each App is intended one per process.
 *   - appRun() returns when SDL_EVENT_QUIT, a window close event, or the
 *     ISO_SMOKE_MS deadline (smoke-test hook used by tests/) is reached.
 *   - All file-scope state lives behind g_ prefixed names in app.c; the
 *     App type is opaque to callers.
 *   - No Android conditionals here: platform differences go through
 *     src/platform/platform.h only.
 */
#ifndef ISOMATA_APP_H
#define ISOMATA_APP_H

#include <stdbool.h>

typedef struct App App;

/* Initialize SDL3 and create the window. Returns NULL on failure
 * (SDL_GetError() carries the reason; SDL_Quit state is cleaned up). */
App *appCreate(const char *title, int width, int height);

/* Run the event loop until clean shutdown is requested. Returns true when
 * the loop exited cleanly. */
bool appRun(App *app);

/* Destroy the window and release SDL. Call once, then discard the pointer. */
void appDestroy(App *app);

#endif /* ISOMATA_APP_H */
