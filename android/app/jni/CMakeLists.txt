cmake_minimum_required(VERSION 3.22.1)
project(isomata LANGUAGES C)

# SDL3 prefab dependency is only resolvable once the AARs are in app/libs
# (see app/build.gradle). SDL3 on Android ships no separate main-glue target:
# SDLActivity (inside the AAR) loads libmain.so and drives SDL_EnterAppMain
# callbacks itself, so linking SDL3::SDL3 is the full official recipe.
find_package(SDL3 REQUIRED CONFIG)

# Exactly one .so, named "main" (-> libmain.so): SDL3's Java loader looks
# for it by that name.
add_library(main SHARED
	../../../src/main.c
	../../../src/app.c
	../../../src/platform/platform_android.c)

# Android twin: platform_android.c (NOT platform.c) so Android conditionals
# never reach the rest of the tree.
target_link_libraries(main PRIVATE SDL3::SDL3)
