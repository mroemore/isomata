# Tests for the isomata engine. Called from the top-level meson.build.
#
# Two suite shapes live here:
#   - 'pure': the headless CTOL suite (tests/unity.c + tests/test_main.c +
#     later tests/test_*.c). It must never link SDL, never create a GPU
#     device or window, and only exercises modules from ctol_sources.
#   - app-level smoke below: runs the whole SDL app under the dummy driver.

# The objects under test: libisomata_pure is defined in the top-level
# meson.build (root-level target: object names stay flat and thus coverage
# capture works — see the wiring note there).
ctol_lib = isomata_pure

# Headless Unity suite: engine-module test files pair 1:1 with ctol_sources
# in the top-level meson.build — every src/<module>.c there pulls its twin
# tests/test_<module>.c from this directory. A missing twin fails the build
# at configure time instead of silently dropping its tests. While
# ctol_sources is empty the suite is test_main.c alone (a valid state).
fs = import('fs')
test_module_src = []
foreach src : ctol_sources
	# Twin name: test_<basename>.c. camera3d.c is the one exception — its
	# tests live in test_camera.c (the task brief's file list), so the
	# basename maps to "camera" here. Every other module pairs 1:1.
	base = fs.name(src).split('.')[0]
	if base == 'camera3d'
		base = 'camera'
	endif
	test_module_src += ['test_' + base + '.c']
endforeach
test_pure_exe = executable('test_pure',
	'test_main.c',
	'unity.c',
	test_module_src,
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
			'SDL_AUDIODRIVER=dummy',
			'ISO_SMOKE_MS=300',
		],
		timeout : 60,
	)
endif
