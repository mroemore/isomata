/* isomata/src/main.c — the process entry point.
 *
 * Platform rule: src/portable code must carry no `__ANDROID__` conditionals
 * outside src/platform/. The one sanctioned exception is this file: the
 * SDL3 entry glue (<SDL3/SDL_main.h>) that lets the same main() serve both
 * the desktop (Meson) and Android (libmain.so via SDLActivity) targets.
 * Every other Android-specific detail goes through src/platform/.
 */
#include <SDL3/SDL_main.h>

#include "app.h"

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	App *app = appCreate("Isomata", 1280, 720);
	if (!app) {
		return 1;
	}
	const bool ranCleanly = appRun(app);
	appDestroy(app);
	return ranCleanly ? 0 : 1;
}
