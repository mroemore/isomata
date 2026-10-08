# Tests for the isomata engine. Called from the top-level meson.build.
# Smoke test: the app must initialize SDL3 (dummy driver), run its loop, and
# exit cleanly inside ISO_SMOKE_MS milliseconds. Native-only until a device
# runner exists for cross builds.
if not meson.is_cross_build()
	test('app smoke', isomata_exe,
		env : [
			'SDL_VIDEODRIVER=dummy',
			'ISO_SMOKE_MS=300',
		],
		timeout : 60,
	)
endif
