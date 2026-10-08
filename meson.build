project('isomata', 'c',
	version : '0.1.0',
	meson_version : '>= 1.3.0',
	default_options : [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

# Coverage builds define the CTOL test config: src/verify.h keys off
# ISOMATA_COVERAGE_TEST to compile contract macros away and enable testcase()
# (see TESTING.md), and NDEBUG keeps assert() from adding uncoverable edges.
# Declared before the first build target (meson requirement).
if get_option('b_coverage')
	add_project_arguments('-DISOMATA_COVERAGE_TEST=1', '-DNDEBUG', language: 'c')
endif

# SDL3 floor is 3.4.0: SDL_gpu and pinch-gesture events are required by later tasks.
sdl3_dep = dependency('sdl3', version : '>= 3.4.0')

# SDL3_ttf is not packaged system-wide on this box; falls back to
# subprojects/sdl3-ttf.wrap when the pkg-config lookup fails.
sdl3_ttf_dep = dependency(
	'sdl3-ttf',
	version : '>= 3.2.0',
)

math_dep = cc.find_library('m', required : false)

engine_deps = [
	sdl3_dep,
	sdl3_ttf_dep,
	math_dep,
]

# The desktop twin: platform.c is compiled here; the Android twin
# (platform_android.c) is compiled only by android/app/jni/CMakeLists.txt.
engine_sources = [
	'src/app.c',
	'src/platform/platform.c',
]

isomata_exe = executable('isomata',
	engine_sources + ['src/main.c'],
	dependencies : engine_deps,
	install : true,
)

install_subdir('assets',
	install_dir : '',
)

# --- CTOL pure-test wiring ------------------------------------------------
# Pure engine modules under test. Later tasks (scene, ...) append file entries
# here (ctol_sources = files('src/x.c', ...); the tests/meson.build suite
# pairs each src/<module>.c with a tests/test_<module>.c twin and fails the
# build if the twin is missing). Every file must compile and link without SDL
# (the headless suite must never create a GPU device or window) and must rely
# only on the deps in ctol_deps.
#
# The static library is defined HERE, not in tests/meson.build: meson
# cross-tree source paths into object names with a '.._' prefix
# when the target lives in a subdirectory ('tests/libisomata_pure.a.p/
# .._src_events.c.o'), which geninfo (lcov 1.x) silently drops from coverage
# ('GCOV did not produce any data'). A root-level target flattens to
# 'isomata_pure.p/src_events.c.o' and captures cleanly, exactly like the
# isomata exe target. Scripts under meson/ discover the target merely by
# name, so the move is transparent to coverage-gate.sh and friends.
ctol_sources = files('src/events.c')
ctol_inc = include_directories('src')
ctol_deps = [math_dep]

isomata_pure = static_library('isomata_pure', ctol_sources,
	include_directories : ctol_inc,
	dependencies : ctol_deps,
)

subdir('tests')
