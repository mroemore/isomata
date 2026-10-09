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
#
# default_options b_sanitize=none: the wrap is a cmake-method subproject whose
# shared library link does not receive the sanitizer runtime flags Meson adds
# to first-party targets, so an instrumented build of it fails to link
# (undefined __asan/__ubsan refs). Keeping the vendored dependency
# uninstrumented lets the asan matrix build and run our own code under ASan.
sdl3_ttf_dep = dependency(
	'sdl3-ttf',
	version : '>= 3.2.0',
	default_options : ['b_sanitize=none'],
)

math_dep = cc.find_library('m', required : false)

engine_deps = [
	sdl3_dep,
	sdl3_ttf_dep,
	math_dep,
]

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
#
# It is declared BEFORE the app executable so the app can link it: input_sdl.c
# (the SDL glue, app-only) calls into the pure input core, and Task 9's scenes
# consume the other pure modules the same way. The pure lib carries no SDL
# dependency, so linking it into the SDL app is safe.
ctol_sources = files('src/events.c', 'src/achievement.c', 'src/scene.c',
		     'src/input/input.c',
		     'src/render/math3d.c', 'src/render/camera3d.c',
		     'src/render/atlas.c', 'src/render/materials.c',
		     'src/render/voxmap.c', 'src/render/mapsource.c',
		     'src/render/grid.c',
		     'src/render/drawlist.c', 'src/render/lightgrid.c',
		     'src/render/sprites.c', 'src/render/frame.c',
		     'src/scenes/ui_bridge.c',
		     'src/ui/ui_scale.c', 'src/ui/element.c', 'src/ui/layout.c',
		     'src/ui/element_label.c', 'src/ui/element_button.c',
		     'src/ui/element_menu.c', 'src/ui/element_overlay.c',
		     'src/ui/toast.c')
ctol_inc = include_directories('src')
ctol_deps = [math_dep]

isomata_pure = static_library('isomata_pure', ctol_sources,
	include_directories : ctol_inc,
	dependencies : ctol_deps,
)

# The desktop twin: platform.c is compiled here; the Android twin
# (platform_android.c) is compiled only by android/app/jni/CMakeLists.txt.
# input_sdl.c is the SDL glue for the pure input core (the only input file
# that includes SDL); it lives here, never in isomata_pure.
# render/gpu_backend.c is the SDL_gpu tier (the only render file that
# includes SDL); it lives here, never in isomata_pure.
engine_sources = [
	'src/app.c',
	'src/platform/platform.c',
	'src/input/input_sdl.c',
	'src/audio/audio.c',
	'src/render/gpu_backend.c',
	'src/render/map_loader.c',
	'src/ui/ui_font.c',
	'src/ui/ui_gpu.c',
	'src/scenes/menu_scene.c',
	'src/scenes/settings_scene.c',
	'src/scenes/level_scene.c',
	'src/scenes/pause_scene.c',
]

isomata_exe = executable('isomata',
	engine_sources + ['src/main.c'],
	include_directories : include_directories('src'),
	dependencies : engine_deps,
	link_with : isomata_pure,
	install : true,
)

install_subdir('assets',
	install_dir : '',
)

# Stage the font into the build tree so the app-smoke run (which resolves
# assets relative to the executable via SDL_GetBasePath) can load it
# without a full install. The installed tree gets it via install_subdir.
# (configure_file/custom_target reject path segments in `output` here, so
# this is a configure-time copy.)
_stage_font_dir = meson.current_build_dir() / 'assets' / 'fonts'
run_command('mkdir', '-p', _stage_font_dir, check : true)
run_command('cp', files('assets/fonts/KiwiSoda.ttf'),
	    _stage_font_dir / 'KiwiSoda.ttf', check : true)

# Stage the checked-in SPIR-V shaders and the placeholder texture the same
# way (Task 7): the app resolves "shaders/*.spv" and
# "textures/placeholder.png" relative to the executable at runtime.
_stage_shader_dir = meson.current_build_dir() / 'assets' / 'shaders'
_stage_texture_dir = meson.current_build_dir() / 'assets' / 'textures'
_stage_map_dir = meson.current_build_dir() / 'assets' / 'maps'
run_command('mkdir', '-p', _stage_shader_dir, check : true)
run_command('mkdir', '-p', _stage_texture_dir, check : true)
run_command('mkdir', '-p', _stage_map_dir, check : true)
run_command('cp', files('assets/shaders/world.vert.spv'),
	    _stage_shader_dir / 'world.vert.spv', check : true)
run_command('cp', files('assets/shaders/world.frag.spv'),
	    _stage_shader_dir / 'world.frag.spv', check : true)
run_command('cp', files('assets/shaders/grid.frag.spv'),
	    _stage_shader_dir / 'grid.frag.spv', check : true)
# Stage the whole textures tree (materials.txt + every material PNG), so the
# runtime atlas can resolve every referenced file relative to the executable.
run_command('cp', '-r',
	    meson.current_source_dir() / 'assets/textures/.',
	    _stage_texture_dir, check : true)
run_command('cp', files('assets/maps/demo.txt'),
	    _stage_map_dir / 'demo.txt', check : true)
# Stage the PNG slice version of the demo (T13b) so the app can load it and
# the pngmap equivalence test can decode the committed slices.
run_command('cp', '-r',
	    meson.current_source_dir() / 'assets/maps/demo',
	    _stage_map_dir, check : true)

# Stage the committed WAVs (Task 10) so the app smoke / X11 run can load
# "audio/*.wav" relative to the executable.
_stage_audio_dir = meson.current_build_dir() / 'assets' / 'audio'
run_command('mkdir', '-p', _stage_audio_dir, check : true)
run_command('cp', files('assets/audio/menu.wav'),
	    _stage_audio_dir / 'menu.wav', check : true)
run_command('cp', files('assets/audio/rotate.wav'),
	    _stage_audio_dir / 'rotate.wav', check : true)
run_command('cp', files('assets/audio/achievement.wav'),
	    _stage_audio_dir / 'achievement.wav', check : true)

subdir('tests')
