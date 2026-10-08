# Tests for the isomata engine. Called from the top-level meson.build.
#
# Two suite shapes live here:
#   - 'pure': the headless CTOL suite (tests/unity.c + tests/test_main.c +
#     later tests/test_*.c). It must never link SDL, never create a GPU
#     device or window, and only exercises modules from ctol_sources.
#   - app-level smoke below: runs the whole SDL app under the dummy driver.

# The objects under test. Later tasks append pure module sources to
# ctol_sources in the top-level meson.build; nothing is built until then.
# Scripts under meson/ (coverage-gate.sh floors, matrix.sh, mutate.sh)
# discover targets by name in this build directory, not by library variable.
ctol_lib = []
if ctol_sources.length() > 0
	ctol_lib = static_library('isomata_pure', ctol_sources,
		include_directories : ctol_inc,
		dependencies : ctol_deps,
	)
endif

# Headless Unity suite: test_main.c handles the empty engine-module case
# (it runs its own sanity tests and exits 0), so an empty ctol_sources is a
# valid state. Later tasks: add 'test_events.c' &c. to the sources list.
test_pure_exe = executable('test_pure',
	'test_main.c',
	'unity.c',
	include_directories : ctol_inc,
	dependencies : ctol_deps,
	link_with : ctol_lib,
)

test('pure', test_pure_exe,
	protocol : 'exitcode',
	timeout : 60,
)

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
