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

subdir('tests')
