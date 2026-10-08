# Release checklist — Isomata

A short, human-in-the-loop gate for cutting a release. The automated tiers
(`./meson/check.sh --full`) prove the code is consistent with what the tests
check; this proves the feature is right.

## 1. Scope

- [ ] What is in this release, in one paragraph.
- [ ] Anything knowingly left out, and why.

## 2. Automated gates

- [ ] `./meson/check.sh --full` is green on the release commit.
- [ ] Sanitizer matrix: the `asan` config runs under clang (GCC here has no
      `libasan`); the `sdl3-ttf` wrap is `b_sanitize=none` so the vendored
      subproject links. Leak/UAF coverage comes from the `valgrind` rung.
- [ ] `meson/coverage-min.txt` floors were raised for any module that closed
      branches this cycle (the ratchet only goes up).
- [ ] No new mutation survivors on gated targets.
- [ ] Every fuzz crasher found this cycle is fixed and kept as a permanent
      `regress*` regression input.

## 3. Regression discipline

- [ ] Every user-reported bug this cycle has a test that fails on the old code
      and passes on the new, named for the symptom.

## 4. Real-app smoke (not automated)

- [ ] Build the shipped binary (`ninja -C build && ./build/isomata` or your
      install step) and exercise the user-facing path by hand on a clean
      checkout. This is the step no automation replaces.

## 4b. Platform artifacts

- [ ] **Android**: fetch the SDL3 + SDL3_ttf AARs into `android/app/libs/`,
      `./gradlew :app:assembleDebug`, and confirm the APK contains both ABIs'
      `libmain.so` + assets (`unzip -l`). Install on an emulator/device and
      walk menu → settings → level → pause; confirm the level shows the voxel
      terrain. Android Back and touch are the two platform-specific inputs to
      check. Asset reads must go through SDL I/O, not stdio `fopen`
      (APK assets are not files) — see `docs/building.md`.
- [ ] **ARM Linux**: the pure suite cross-runs under `qemu-aarch64-static` /
      `qemu-arm` and all sources compile for aarch64/armhf; a full app build
      additionally needs a target SDL3 sysroot (`docs/building.md`).

## 5. Notes and tag

- [ ] Release notes written.
- [ ] Version bumped.
- [ ] Tag cut; artifacts built from the tag.
