# Isomata

A greenfield SDL3/C11 isometric voxel-game engine. This is the vertical-slice
scaffold: a Meson-based desktop build, an SDL3 Android app shell, and the
platform seam all later tasks extend.

## Layout

```
src/            engine sources (subsystems are added under src/<system>/ later)
src/platform/   the ONLY place Android-vs-desktop conditionals may live
                (platform.c = desktop twin compiled by Meson;
                 platform_android.c = NDK twin compiled by Gradle/CMake)
tests/          meson test targets (starts with an SDL event-loop smoke test)
cross/          Meson cross files (aarch64.ini, armhf.ini — Linux-on-Linux)
android/        SDL3 Android scaffold (Gradle + CMake, builds libmain.so)
assets/         runtime assets; installed next to the desktop binary
subprojects/    wrap fallbacks (sdl3-ttf) — not vendored sources
```

## Conventions

- C11, tab indentation.
- camelCase functions, PascalCase types, `g_` prefix for file-scope globals,
  `X_COUNT` enum sentinels.
- Headers carry a top comment block stating the contract/invariants.
- No `#ifdef __ANDROID__` outside `src/platform/`, except the single
  entry-point exception in `src/main.c` (SDL3 main glue include).

## Dependencies

- SDL3 >= 3.4.0 (SDL_gpu + pinch-gesture events are assumed by later tasks).
- SDL3_ttf >= 3.2.0 (text rendering; a wrap fallback is wired for systems
  that ship only the runtime library).

### Linux (desktop) — Meson

Void Linux ships `SDL3`/`SDL3-devel` **and** `SDL3_ttf`/`SDL3_ttf-devel`:

```
xbps-install SDL3-devel SDL3_ttf-devel
meson setup build
meson test -C build
build/isomata     # opens a window; close it or Ctrl-C to quit
```

If a system pkg-config entry for `sdl3-ttf` is missing (e.g. only the runtime
lib is installed), Meson transparently falls back to
`subprojects/sdl3-ttf.wrap`, which builds SDL_ttf 3.2.2 (cmake-method wrap,
FreeType vendored) under the hood. Both paths are supported; the wrap is not
vendored source in this repo — it is a build-time download.

Smoke test without a display server:

```
SDL_VIDEODRIVER=dummy ISO_SMOKE_MS=300 build/isomata
```

### ARM cross builds (Linux targets)

The cross files are wired for the Void `aarch64-linux-gnu` /
`arm-linux-gnueabihf` toolchains:

```
meson setup build-aarch64 --cross-file cross/aarch64.ini
meson setup build-armhf  --cross-file cross/armhf.ini
```

SDL3 is not packaged for the targets, so a full app cross-build needs a
target sysroot first. For **aarch64 this is verified end-to-end**:
`cross/build-sdl3-aarch64.sh` builds the SDL3/SDL3_ttf sysroot, the app
links, and the headless smoke runs under `qemu-aarch64-static` (exit 0).
For **armhf**, the code is verified by the headless pure test suite: it
cross-compiles to a static binary and runs under `qemu-arm` (210 tests,
0 failures), and every SDL-tier translation unit compiles clean; a full
armhf app link needs the equivalent sysroot (mirror the aarch64 script).
See [docs/building.md](docs/building.md#arm-linux-cross-builds) for the
exact commands and prerequisites.

### Android

The Android build does NOT use Meson. It is a Gradle + CMake project under
`android/` that compiles the exact same `src/` tree via
`android/app/jni/CMakeLists.txt` into `libmain.so` (SDL3's expected entry
library), with the `org.libsdl.app.SDLActivity` launcher and the repo
`assets/` packed into the APK.

Setup:

1. Install the Android SDK (NDK 29 tested) and JDK 17+ (JDK 21 is used here at
   `~/.local/opt/jdk-21`).
2. `android/local.properties` (gitignored) must contain `sdk.dir=<path>`.
3. Download the prefab AARs from the official libsdl-org releases and copy
   them into `android/app/libs/` (gitignored):
   - `SDL3-devel-<ver>-android.zip` → `SDL3-<ver>.aar`
   - `SDL3_ttf-devel-<ver>-android.zip` → `SDL3_ttf-<ver>.aar`
4. Build (the AARs are picked up by the `fileTree(dir: 'libs')` dependency; no
   file names need editing):

```
cd android
JAVA_HOME=/home/krang/.local/opt/jdk-21 ./gradlew :app:assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk (arm64-v8a + x86_64)
```

Constraints: `minSdk 26`, ABIs `arm64-v8a` + `x86_64`, AARs from the official
prefab SDL releases only — never vendored into the repo. Full prerequisites,
emulator steps, and known limitations are in
[docs/building.md](docs/building.md#android).

[sdlrel]: https://github.com/libsdl-org/SDL/releases
[sdlttfrel]: https://github.com/libsdl-org/SDL_ttf/releases
