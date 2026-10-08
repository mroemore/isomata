# Building Isomata

This document covers how to build, run, and verify the Isomata vertical slice
on its three supported targets:

- **Linux x86_64** (desktop, Meson) — primary, fully verified.
- **ARM Linux** (aarch64 / armhf, Meson cross) — code verified; full app link
  needs a target SDL3 sysroot (see [ARM Linux cross builds](#arm-linux-cross-builds)).
- **Android** (`arm64-v8a` + `x86_64`, Gradle + CMake) — builds, installs, and
  runs the slice on an emulator (see [Android](#android)).

Conventions: C11, `warning_level=2` + `werror` on the Meson builds, tabs,
Spectrax naming, no `__ANDROID__` outside `src/platform/` (single entry-point
exception in `src/main.c`).

---

## Linux x86_64 (desktop, Meson)

### Prerequisites

- Void Linux: `xbps-install SDL3-devel SDL3_ttf-devel` (SDL3 >= 3.4.0,
  SDL3_ttf >= 3.2.0). Other distros: install the equivalent SDL3 / SDL3_ttf
  development packages (`-dev` / `-devel`).
- If no system `sdl3-ttf` pkg-config entry exists, Meson transparently falls
  back to `subprojects/sdl3-ttf.wrap` (SDL_ttf 3.2.2, cmake-method wrap,
  FreeType vendored) — a build-time download, not vendored source.
- Runtime GPU: the renderer uses SDL_gpu's **Vulkan** backend. There is no
  software backend; a box with no Vulkan driver runs only the smoke path
  (GPU backend unavailable, tolerated).

### Build and run

```sh
meson setup build
ninja -C build
build/isomata            # opens a window; close it or Ctrl-C to quit
```

Install (assets land next to the binary via `install_subdir('assets')`):

```sh
meson install -C build --destdir /tmp/isomata-install
```

### Headless smoke (no display / no GPU)

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ISO_SMOKE_MS=300 build/isomata
```

`ISO_SMOKE_MS` bounds the loop; the run probes the bundled font and reports
the safe-area inset, then exits 0 even when no GPU backend is available.

### Tests and gates

```sh
meson test -C build            # pure suite + app smoke
./meson/check.sh               # fast tier: coverage + per-module gate
./meson/check.sh --full        # + asan/valgrind matrix + fuzz + mutation
```

Release build:

```sh
meson setup build-release --buildtype=release
meson test -C build-release
```

---

## ARM Linux cross builds

The cross files are `cross/aarch64.ini` and `cross/armhf.ini` (Void's
`aarch64-linux-gnu-*` and `arm-linux-gnueabihf-*` toolchains). Toolchains,
`qemu-aarch64`(+static), and `qemu-arm` are present on this box.

### Full app cross-build: currently blocked on target SDL3

```sh
meson setup build-aarch64 --cross-file cross/aarch64.ini
# ERROR: Dependency lookup for sdl3 ... Pkg-config for machine host machine
#        not found. Giving up.
```

Two prerequisites are missing, and neither is installable here (no sudo, no
aarch64 packages):

1. A target-side `pkg-config` wrapper. The cross file names
   `aarch64-linux-gnu-pkg-config`, which Void's cross package does not ship.
2. An **aarch64 SDL3** (and SDL3_ttf) development sysroot — headers, `.pc`
   file, and target libraries. The host `/usr/include/SDL3` headers are
   architecture-independent, but there is no aarch64 `libSDL3.so` to link.

To finish a full ARM app build you would cross-build SDL3 (and SDL3_ttf) for
the target, or install target sysroot packages, then point the cross file's
`pkg-config`/`[properties]` at them. SDL3 can be configured with X11/Wayland/
Vulkan disabled for a headless (`SDL_VIDEODRIVER=dummy`) target build.

### What *is* verified for ARM (honest minimum)

- **The pure engine test suite cross-compiles and runs on ARM.** Built static
  for aarch64 and armhf and executed under QEMU:

  ```sh
  aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -I src -I tests \
      -c <each pure + test source> ...
  aarch64-linux-gnu-gcc -static <objects> -lm -o test_pure-aarch64
  qemu-aarch64-static ./test_pure-aarch64        # 204 Tests 0 Failures 0 Ignored -> OK
  ```

  armhf is identical with `arm-linux-gnueabihf-gcc` / `qemu-arm`.

- **Every SDL-tier translation unit compiles clean for aarch64 and armhf**
  (`-Wall -Wextra -Werror`), using the host SDL3 headers and the SDL3_ttf wrap
  headers via a scratch include dir:

  ```sh
  # SDL3 headers are arch-independent; expose them without shadowing the
  # target libc (a cross compiler does not search the host /usr/include):
  mkdir -p .tmp_files/arm-cross/inc
  ln -sfn /usr/include/SDL3 .tmp_files/arm-cross/inc/SDL3
  ln -sfn "$PWD/subprojects/SDL3_ttf-3.2.2/include/SDL3_ttf" \
          .tmp_files/arm-cross/inc/SDL3_ttf

  aarch64-linux-gnu-gcc -std=c11 -Wall -Wextra -Werror -I src \
      -I .tmp_files/arm-cross/inc -c src/app.c -o app.o
  # ... 12 engine objects per target, all clean
  ```

A helper script that reproduces both checks lives at
`.tmp_files/arm-cross.sh` (scratch, not committed).

---

## Android

### Prerequisites

- Android SDK with platform 34, build-tools, `platform-tools` (adb), the
  emulator, and an NDK (NDK **29.0.14206865** tested; `ndkVersion` is pinned in
  `android/app/build.gradle`).
- JDK 17+ (JDK **21** used here, at `~/.local/opt/jdk-21`).
- `android/local.properties` (gitignored) with `sdk.dir=<sdk path>`.
- The prefab AARs (below) copied into `android/app/libs/` (gitignored).

### AAR acquisition

The AARs are binary dependencies and are **never committed** (`android/app/libs/`
is in `.gitignore`). Fetch them from the official libsdl-org releases:

```sh
mkdir -p .tmp_files && cd .tmp_files
# SDL3 (match the pinned desktop SDL3 line, 3.4.x):
curl -sL -o SDL3-devel-android.zip \
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.14/SDL3-devel-3.4.14-android.zip
unzip -o SDL3-devel-android.zip 'SDL3-*.aar'
# SDL3_ttf (latest 3.2.x; ABI-compatible with SDL3 3.4):
curl -sL -o SDL3_ttf-devel-android.zip \
  https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-devel-3.2.2-android.zip
unzip -o SDL3_ttf-devel-android.zip 'SDL3_ttf-*.aar'
cp SDL3-*.aar SDL3_ttf-*.aar ../android/app/libs/
```

`android/app/build.gradle` pulls whatever is in `libs/` via
`implementation fileTree(dir: 'libs', include: ['*.aar'])`; `buildFeatures {
prefab true }` exposes the prefab modules (`SDL3::SDL3`, `SDL3_ttf::SDL3_ttf`)
to `android/app/jni/CMakeLists.txt`. No file names need editing.

### Build

```sh
cd android
JAVA_HOME=/home/krang/.local/opt/jdk-21 ./gradlew :app:assembleDebug
# BUILD SUCCESSFUL
# -> app/build/outputs/apk/debug/app-debug.apk
```

`android/app/jni/CMakeLists.txt` compiles the **same** `src/` tree the Meson
build compiles — the pure modules plus the SDL-tier engine modules, with
`platform_android.c` in place of `platform.c` — into a single `libmain.so`
(SDL3's loader looks it up by that name).

### Verify the APK contents

```sh
unzip -l android/app/build/outputs/apk/debug/app-debug.apk | grep -E 'lib/|assets/'
```

Expected (both ABIs, all assets):

```
lib/arm64-v8a/libmain.so      lib/x86_64/libmain.so
lib/arm64-v8a/libSDL3.so      lib/x86_64/libSDL3.so
lib/arm64-v8a/libSDL3_ttf.so  lib/x86_64/libSDL3_ttf.so
lib/arm64-v8a/libc++_shared.so lib/x86_64/libc++_shared.so
assets/audio/*.wav  assets/fonts/KiwiSoda.ttf  assets/maps/demo.txt
assets/shaders/world.{vert,frag}.spv  assets/textures/placeholder.png
```

### Emulator / device run

```sh
export PATH="$PATH:~/android-sdk/platform-tools"
~/android-sdk/emulator/emulator -list-avds        # e.g. pwnky_test (x86_64)
~/android-sdk/emulator/emulator -avd pwnky_test \
    -no-window -no-audio -no-boot-anim -no-snapshot \
    -gpu swiftshader_indirect -port 5554 &
adb wait-for-device
adb shell getprop sys.boot_completed              # -> 1
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -c
adb shell am start -n com.isomata.app/org.libsdl.app.SDLActivity
adb logcat -d | grep -iE 'SDL/APP|isomata'        # see below
adb exec-out screencap -p > menu.png
```

Expected log lines on a working run (emulator x86_64, SwiftShader Vulkan):

```
SDL/APP : gpu_backend: ready (driver vulkan)
SDL/APP : isomata: window 1080x1854 pixels (aspect 0.583)
SDL/APP : audio: ready (8 voices, device 21, 44100 Hz 2 ch)
```

Then interact:

```sh
adb shell input tap <x> <y>       # touch tap -> menu selection (plays the blip)
adb shell input keyevent 4        # Android Back -> UI_CANCEL / scene pop
```

### Runtime verification status (honest)

Verified on the `pwnky_test` AVD (android-34, x86_64, SwiftShader Vulkan):

- App launches, `SDLActivity` runs, `libmain.so` loads, `SDL_main` starts.
- Vulkan GPU backend initializes (`driver vulkan`) and audio opens.
- The **start menu renders** (title, Start/Settings/Quit, hint) — screenshot
  taken.
- **Touch** works: tapping *Settings* pushes the settings overlay; tapping a
  row plays the menu blip.
- **Android Back** works: it pops the settings overlay and, from the level,
  opens the pause overlay.
- The slice navigates menu → settings → level → pause without crashing.
- The **level renders the voxel terrain** (the demo heightmap: plateau,
  trench, stepped elevations) plus the billboard sprites — screenshot taken.

### Asset loading (Android)

Every asset load goes through SDL's asset-aware I/O, which resolves APK assets
by relative name (plain C stdio `fopen` cannot see them):

| Asset | Reader | Android-safe |
|-------|--------|--------------|
| shaders (`world.*.spv`) | `SDL_LoadFile` (`gpu_backend.c`) | yes |
| texture (`placeholder.png`) | `SDL_LoadPNG` (`gpu_backend.c`) | yes |
| font (`KiwiSoda.ttf`) | `TTF_OpenFont` → `SDL_IOFromFile` (`ui_font.c`/`ui_gpu.c`) | yes |
| audio (`*.wav`) | `SDL_LoadWAV` (`audio.c`) | yes |
| map (`demo.txt`) | `SDL_LoadFile` + `parseVoxmapText` (`level_scene.c`) | yes |

`voxmap.c` keeps its pure `loadVoxmap(path)` (stdio) for desktop/tests, but the
SDL tier loads the map with `SDL_LoadFile` and parses it with the pure,
length-bounded `parseVoxmapText()` — so no Android path uses `fopen`.

---

## Known limitations

- **Vulkan required.** SDL_gpu on Android/Linux uses Vulkan; there is no
  software fallback. Without a Vulkan driver the app logs
  `gpu_backend: ... No supported SDL_GPU backend found` and (outside smoke
  mode) exits. The emulator's `swiftshader_indirect` GPU provides Vulkan and is
  what the runtime check above used.
- **Asset I/O convention.** Asset readers must use SDL I/O
  (`SDL_LoadFile`/`SDL_LoadPNG`/`SDL_LoadWAV`/`TTF_OpenFont`), not C stdio
  `fopen`, because APK assets are not filesystem files. The pure modules keep
  stdio readers for desktop/tests (`voxmap.c`'s `loadVoxmap`); the SDL tier
  feeds them from `SDL_LoadFile` (map) — see the Asset loading table above.
- **AARs are untracked.** `android/app/libs/*.aar` is gitignored; a fresh
  checkout must fetch them (see above). The APK build fails with the exact
  missing-dependency message until they are present.
- **ASan/valgrind.** GCC on this box ships no `libasan`; sanitizer builds use
  clang (`/usr/lib/llvm/21/bin/clang`). The `sdl3-ttf` wrap is built with
  `b_sanitize=none` (see `meson.build`) so the vendored cmake subproject links;
  first-party code is instrumented. Valgrind remains the leak/UAF rung. All of
  `./meson/check.sh --full` is green.
- **ARM full app build** needs a target SDL3 sysroot (above).
