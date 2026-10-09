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

# Mutation target: meson/mutate.sh discovers tests/test_mutate_*.c and runs
# <builddir>/tests/<name>. Built here as a normal pure test too, so the same
# executable guards parseVoxmapText on every build. Links the dependency-free
# input-mutation support file (tests/support/mutate.*).
test_mutate_parsevoxmap_exe = executable('test_mutate_parsevoxmap',
	'test_mutate_parsevoxmap.c',
	'support/mutate.c',
	include_directories : ctol_inc,
	dependencies : ctol_deps,
	link_with : ctol_lib,
)

test('mutation parsevoxmap', test_mutate_parsevoxmap_exe,
	protocol : 'exitcode',
	timeout : 120,
)

# Fault-injection target (CTOL rung 4): the --wrap recipe and
# -DISOMATA_HARNESS_FAULTINJECT are exactly what tests/support/faultinject.h
# documents. Every allocation-failure arm of the linked pure modules is swept
# and checked for a clean failure with no leak.
test_faultinject_exe = executable('test_faultinject',
	'test_faultinject.c',
	'support/faultinject.c',
	include_directories : ctol_inc,
	dependencies : ctol_deps,
	link_with : ctol_lib,
	c_args : ['-DISOMATA_HARNESS_FAULTINJECT'],
	link_args : [
		'-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc',
		'-Wl,--wrap=free', '-Wl,--wrap=fopen', '-Wl,--wrap=fread',
		'-Wl,--wrap=fclose', '-Wl,--wrap=fopen64',
	],
)

test('fault injection', test_faultinject_exe,
	protocol : 'exitcode',
	timeout : 120,
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

	# T13b equivalence pin (SDL tier): decode the committed PNG slice
	# directory with the real SDL glue and assert the assembled Voxmap is
	# identical to the ASCII demo. Native-only (decodes real PNGs); its own
	# main(). Links the shared isomata_glue (map_loader.c) so its coverage is
	# captured (a root-level target keeps the object names flat).
	test_pngmap_exe = executable('test_pngmap',
		'test_pngmap.c',
		'unity.c',
		include_directories : ctol_inc,
		dependencies : engine_deps,
		link_with : [ctol_lib, isomata_glue],
	)
	test('pngmap equivalence', test_pngmap_exe,
		env : [
			'SDL_VIDEODRIVER=dummy',
			'SDL_AUDIODRIVER=dummy',
		],
		protocol : 'exitcode',
		timeout : 60,
	)
endif
