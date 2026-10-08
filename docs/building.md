# Building Isomata

This document covers how to build, run, and verify the Isomata vertical slice
on its three supported targets:

- **Linux x86_64** (desktop, Meson) — primary, fully verified.
- **ARM Linux** (aarch64 / armhf, Meson cross) — aarch64 builds, links, and
  runs the full app under QEMU; armhf is code-verified (see
  [ARM Linux cross builds](#arm-linux-cross-builds)).
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

### Full app cross-build (aarch64, verified)

The full app links and runs for aarch64. SDL3 and SDL3_ttf are not packaged
for the target, so build them into a sysroot first:

```sh
cross/build-sdl3-aarch64.sh          # SDL3 3.4.14 + SDL3_ttf 3.2.2 (vendored
                                     # FreeType) into .tmp_files/arm-sdl/sysroot
PKG_CONFIG_LIBDIR="$PWD/.tmp_files/arm-sdl/sysroot/lib/pkgconfig" \
    meson setup build-aarch64 --cross-file cross/aarch64.ini
ninja -C build-aarch64
```

`build-aarch64/isomata` is then an aarch64 ELF. Run the headless smoke under
QEMU (the host has no aarch64 glibc at `/lib`, so `-L` points QEMU at the
toolchain sysroot's loader and libc):

```sh
qemu-aarch64-static -L /usr/aarch64-linux-gnu \
    -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E ISO_SMOKE_MS=300 \
    build-aarch64/isomata
# isomata smoke: font measure "Isomata" = 100x32 (height 32, rc 0)
# isomata smoke: safe area 0,0 1280x720
# ERROR: gpu_backend: ... No supported SDL_GPU backend found!   (tolerated)
# audio: ready (8 voices, device 21, 44100 Hz 2 ch)
# exit 0
```

The sysroot SDL3 is configured `-DSDL_UNIX_CONSOLE_BUILD=ON` with
X11/Wayland/KMSDRM and every audio backend off and `-DSDL_VULKAN=OFF`; the
dummy video/audio drivers are built in, so the smoke path runs and the GPU
backend is absent by design (the smoke tolerates it). SDL3_ttf is built with
its vendored FreeType, without harfbuzz/plutosvg.

### What *is* verified for ARM

- **The pure engine test suite cross-compiles and runs on aarch64 and armhf**
  under QEMU (208 tests, 0 failures). Built static and run with
  `qemu-aarch64-static` / `qemu-arm`.
- **Every SDL-tier translation unit compiles clean for aarch64 and armhf**
  (`-Wall -Wextra -Werror`).
- **The full aarch64 app links and runs** — `build-aarch64/isomata` under
  `qemu-aarch64-static` with the dummy drivers, exit 0 (above).
- armhf is code-verified only: a full 32-bit app link needs the same
  SDL3/SDL3_ttf sysroot for `arm-linux-gnueabihf` (mirror
  `cross/build-sdl3-aarch64.sh` with an armhf toolchain file).

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
- **ARM full app build**: aarch64 is verified end-to-end
  (`cross/build-sdl3-aarch64.sh` builds the SDL3/SDL3_ttf sysroot; the app
  links and runs under QEMU — see [ARM Linux cross builds](#arm-linux-cross-builds)).
  armhf still needs the equivalent armhf sysroot.
