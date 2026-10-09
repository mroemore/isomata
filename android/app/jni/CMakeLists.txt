cmake_minimum_required(VERSION 3.22.1)
project(isomata LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# SDL3 / SDL3_ttf prefab dependencies are only resolvable once the AARs are in
# app/libs (see app/build.gradle). SDL3 on Android ships no separate main-glue
# target: SDLActivity (inside the AAR) loads libmain.so and drives
# SDL_EnterAppMain callbacks itself, so linking SDL3::SDL3 is the full official
# recipe. SDL3_ttf arrives as its own AAR; prefab exposes it as the module
# SDL3_ttf (target SDL3_ttf::SDL3_ttf) and its headers under SDL3_ttf/.
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)

# The engine is compiled from the exact same src/ tree the Meson build uses:
# the pure modules (isomata_pure on desktop) plus the SDL-tier engine modules
# (engine_sources). platform_android.c replaces platform.c (the Android twin);
# every other file is shared verbatim. Keep this list in lockstep with
# meson.build's ctol_sources + engine_sources.
set(ISOMATA_SRC
	# SDL-tier (engine_sources + main.c)
	../../../src/main.c
	../../../src/app.c
	../../../src/platform/platform_android.c
	../../../src/input/input_sdl.c
	../../../src/audio/audio.c
	../../../src/render/gpu_backend.c
	../../../src/ui/ui_font.c
	../../../src/ui/ui_gpu.c
	../../../src/scenes/menu_scene.c
	../../../src/scenes/settings_scene.c
	../../../src/scenes/level_scene.c
	../../../src/scenes/pause_scene.c
	# Pure modules (ctol_sources)
	../../../src/events.c
	../../../src/achievement.c
	../../../src/scene.c
	../../../src/input/input.c
	../../../src/render/math3d.c
	../../../src/render/camera3d.c
	../../../src/render/atlas.c
	../../../src/render/materials.c
	../../../src/render/voxmap.c
	../../../src/render/mapsource.c
	../../../src/render/lightgrid.c
	../../../src/render/drawlist.c
	../../../src/render/sprites.c
	../../../src/render/frame.c
	../../../src/render/grid.c
	../../../src/scenes/ui_bridge.c
	../../../src/ui/ui_scale.c
	../../../src/ui/element.c
	../../../src/ui/layout.c
	../../../src/ui/element_label.c
	../../../src/ui/element_button.c
	../../../src/ui/element_menu.c
	../../../src/ui/element_overlay.c
	../../../src/ui/toast.c
)

# Exactly one .so, named "main" (-> libmain.so): SDL3's Java loader looks
# for it by that name.
add_library(main SHARED ${ISOMATA_SRC})

# Engine headers are included as "app.h", "render/foo.h", ... exactly as the
# Meson build (include_directories('src')).
target_include_directories(main PRIVATE ../../../src)

# Meson warning policy parity (warning_level=2 + werror).
target_compile_options(main PRIVATE -Wall -Wextra -Werror)

target_link_libraries(main PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)
