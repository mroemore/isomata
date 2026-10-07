# Isomata Vertical Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a playable SDL3/C11 isometric voxel-engine vertical slice for Linux and Android.

**Architecture:** SDL_gpu with an orthographic 3D camera, modular vtable-backed subsystems, queued pub/sub events, retained scene overlays, and virtual-pixel UI scaling.

**Tech Stack:** C11, SDL3, SDL_gpu, SDL3_ttf, Meson, Android Gradle/CMake, Vulkan/SPIR-V.

**Spec:** `docs/superpowers/specs/2026-10-08-isomata-vertical-slice-design.md`

## Global Constraints

- Use SDL_gpu rather than OpenGL or a software rasterizer.
- Use Spectrax naming and dotzy structural conventions.
- Target Linux x86_64, ARM Linux, and Android.
- Initial ARM Linux target requires Vulkan-capable hardware.
- Android targets `arm64-v8a` and `x86_64`, minimum API 26.
- UI uses virtual coordinates with conversion only at draw/input seams.
- Scene changes are deferred to safe update boundaries.
- Initial transparent rendering uses deterministic painter sorting.
- Functional settings, combat, persistence, lighting, and editor tooling are out of scope.

## Review Focus

1. DPI conversion errors are high risk because every UI interaction crosses physical/virtual coordinates. Pinned by Task 5's `test_ui_scale` (1x, 2x, 3x, invalid, fractional densities).
2. Scene destruction during event dispatch is high risk because callbacks can trigger transitions. Pinned by Task 3's unsubscribe-during-callback test and Task 4's deferred-mutation tests.
3. Draw-order ties are high risk because terrain and transparent sprites share one painter-sorted list. Pinned by Task 8's stable-ordering and deterministic-tie tests.
4. Android asset and back-button behavior are high risk because platform event and packaging paths differ. Covered by Task 6's back mapping and Task 11's on-device verification (manual gate).
5. Shader availability is high risk because SDL_gpu requires target-compatible compiled shaders. Pinned by Task 7's checked-in SPIR-V fallback and clear-diagnostic requirement.

---

### Task 1: Repository and Build Scaffold

**Files:**

- Create: `meson.build`
- Create: `meson_options.txt`
- Create: `src/main.c`
- Create: `src/app.c`, `src/app.h`
- Create: `src/platform/platform.c`, `src/platform/platform_android.c`, `src/platform/platform.h`
- Create: `tests/meson.build`
- Create: `cross/aarch64.ini`
- Create: `cross/armhf.ini`
- Create: `assets/`
- Create: `android/`

**Interfaces:**

- Produces `App`, `appCreate()`, `appRun()`, `appDestroy()`.
- Produces platform functions `platformDisplayDensity()`, `platformDisplaySize()`, `platformAssetPath()`.
- Produces Linux and Android build entry points.

- [ ] Create the Meson project with C11, warnings, SDL3, SDL3_ttf, math, and test targets.
- [ ] Add a minimal SDL window and event loop with clean shutdown.
- [ ] Add platform twin files and keep Android conditionals inside `platform/`.
- [ ] Add Linux native and ARM cross files.
- [ ] Add the SDL3 Android Gradle/CMake scaffold with `libmain.so`.
- [ ] Verify Linux build and launch.
- [ ] Verify Android project configuration reaches native compilation.
- [ ] Commit `build: scaffold isomata desktop and android targets`.

**Acceptance:** The repository is currently empty, so this task establishes all project structure. Linux must launch and Android must compile before feature work proceeds.

---

### Task 2: CTOL and Pure-Test Foundation

**Files:**

- Create: `meson/coverage-min.txt`
- Create: `tests/unity.c`, `tests/unity.h`
- Create: `tests/test_main.c`
- Create: `.clang-format`
- Create: `scripts/`

**Interfaces:**

- Produces the headless test executable and CTOL meson gates that all later pure-module tests plug into.

- [ ] Add CTOL scaffolding and Meson test gates.
- [ ] Add a headless test executable independent of SDL_gpu.
- [ ] Add formatting and sanitizer configurations.
- [ ] Verify the empty test suite through Meson and CTOL.
- [ ] Commit `test: establish headless and coverage gates`.

**Acceptance:** Pure-module tests must run without creating a GPU device or window.

---

### Task 3: Event Bus

**Files:**

- Create: `src/events.c`, `src/events.h`
- Create: `tests/test_events.c`

**Interfaces:**

```c
typedef struct EventBus EventBus;
typedef struct Event Event;

typedef void (*EventCallback)(void *ctx, const Event *event);

EventBus *createEventBus(size_t capacity);
void destroyEventBus(EventBus *bus);
bool subscribeEvent(EventBus *bus, uint32_t topic,
                    EventCallback callback, void *ctx);
bool unsubscribeEvent(EventBus *bus, uint32_t topic,
                      EventCallback callback, void *ctx);
bool publishEvent(EventBus *bus, uint32_t topic, uint32_t type,
                  const void *payload, size_t payloadSize);
void dispatchEvents(EventBus *bus);
```

- [ ] Write tests for copied payload ownership.
- [ ] Write tests proving publish is deferred until dispatch.
- [ ] Write tests for multiple subscribers.
- [ ] Write tests for unsubscribe during callback.
- [ ] Implement bounded queue storage and subscriber lists.
- [ ] Verify event tests and sanitizer runs.
- [ ] Commit `feat: add queued event bus`.

**Acceptance:** Events are queued, copied, dispatched once per frame, and safe against callback-triggered subscription changes.

---

### Task 4: Scene Vtable and Scene Stack

**Files:**

- Create: `src/scene.c`, `src/scene.h`
- Create: `tests/test_scene.c`

**Interfaces:**

```c
typedef struct SceneVt SceneVt;
typedef struct Scene Scene;
typedef struct SceneStack SceneStack;

struct SceneVt {
	bool (*init)(void *self, App *app);
	void (*update)(void *self, App *app, float dt);
	void (*draw)(void *self, App *app);
	void (*unload)(void *self, App *app);
	size_t size;
	const char *name;
};

Scene *createScene(const SceneVt *vt);
void destroyScene(Scene *scene);

bool initScene(Scene *scene, App *app);
void updateScene(Scene *scene, App *app, float dt);
void drawScene(Scene *scene, App *app);
void unloadScene(Scene *scene, App *app);
```

- [ ] Test initialization and unload ordering.
- [ ] Test push, pop, and replace semantics.
- [ ] Test that retained underlying scenes remain loaded.
- [ ] Test that only the active scene updates.
- [ ] Implement deferred scene-stack mutations.
- [ ] Verify headless scene tests.
- [ ] Commit `feat: add vtable scene stack`.

**Acceptance:** Replace transitions unload; pushed overlays retain underlying scenes without updating them.

---

### Task 5: UI Tree and DPI Scaling

**Files:**

- Create: `src/ui/ui_scale.c`, `src/ui/ui_scale.h`
- Create: `src/ui/element.c`, `src/ui/element.h`
- Create: `src/ui/layout.c`, `src/ui/layout.h`
- Create: `src/ui/element_label.c/.h`
- Create: `src/ui/element_button.c/.h`
- Create: `src/ui/element_menu.c/.h`
- Create: `src/ui/element_overlay.c/.h`
- Create: `src/ui/toast.c/.h`
- Create: `tests/test_ui_scale.c`
- Create: `tests/test_layout.c`

**Interfaces:**

```c
float uiScaleFromDensity(float density);
int uiScalePhysicalToVirtual(int px, float scale);
int uiScaleVirtualToPhysical(int px, float scale);
void uiScaleRect(int *x, int *y, int *w, int *h, float scale);
```

- [ ] Write scale tests for 1x, 2x, 3x, invalid, and fractional densities.
- [ ] Write layout tests for wide, portrait, and safe-area bounds.
- [ ] Implement element vtables with draw, input, and destroy callbacks.
- [ ] Implement weighted vertical layout.
- [ ] Implement menu selection and activation.
- [ ] Implement toast lifetime and fade state.
- [ ] Integrate SDL3_ttf text rendering.
- [ ] Verify UI tests and a minimal rendered menu.
- [ ] Commit `feat: add scalable tree-based ui`.

**Acceptance:** UI remains logically stable across resolution, aspect ratio, and density changes.

---

### Task 6: Input Command Layer

**Files:**

- Create: `src/input/input.c`, `src/input/input.h`
- Create: `tests/test_input.c`

**Interfaces:**

```c
typedef enum {
	CMD_NAV_UP,
	CMD_NAV_DOWN,
	CMD_NAV_LEFT,
	CMD_NAV_RIGHT,
	CMD_SELECT,
	CMD_BACK,
	CMD_ROTATE_CW,
	CMD_ROTATE_CCW,
	CMD_ZOOM_IN,
	CMD_ZOOM_OUT,
	CMD_COUNT
} Command;
```

- [ ] Add keyboard mappings including arrows and hjkl.
- [ ] Add mouse click, wheel, and drag handling.
- [ ] Add touch tap, drag, and pinch handling.
- [ ] Convert pointer coordinates at the virtual-coordinate seam.
- [ ] Map Android back to `CMD_BACK`.
- [ ] Test command translation independently of scenes.
- [ ] Commit `feat: add cross-device command input`.

**Acceptance:** Keyboard, mouse, and touch all drive the same command-level behavior.

---

### Task 7: SDL_gpu Backend and Camera

**Files:**

- Create: `src/render/gpu_backend.c`, `src/render/gpu_backend.h`
- Create: `src/render/camera3d.c`, `src/render/camera3d.h`
- Create: `src/render/math3d.c`, `src/render/math3d.h`
- Create: `shaders/world.vert.hlsl`
- Create: `shaders/world.frag.hlsl`
- Create: `scripts/compile-shaders.sh`
- Create: `tests/test_camera.c`

**Interfaces:**

```c
typedef struct Camera3D Camera3D;

void initCamera3D(Camera3D *camera);
void cameraRotateQuarterTurn(Camera3D *camera, int direction);
void updateCamera3D(Camera3D *camera, float dt);
void cameraZoom(Camera3D *camera, float amount);
void cameraPan(Camera3D *camera, float x, float y);
Mat4 cameraProjection(const Camera3D *camera, float aspect);
Mat4 cameraView(const Camera3D *camera);
```

- [ ] Test orthographic projection and true-isometric default pitch.
- [ ] Test 90° yaw normalization and interpolation completion.
- [ ] Implement SDL_gpu device and swapchain ownership.
- [ ] Implement a textured quad pipeline.
- [ ] Add SPIR-V generation and checked-in fallback artifacts.
- [ ] Render a static quad on Linux.
- [ ] Verify shader loading failure produces a clear diagnostic.
- [ ] Commit `feat: add SDL gpu renderer and isometric camera`.

**Acceptance:** Linux renders a valid 3D orthographic scene through SDL_gpu.

---

### Task 8: Voxmap and Draw List

**Files:**

- Create: `src/render/voxmap.c`, `src/render/voxmap.h`
- Create: `src/render/drawlist.c`, `src/render/drawlist.h`
- Create: `src/render/sprites.c`, `src/render/sprites.h`
- Create: `assets/maps/demo.txt`
- Create: `tests/test_voxmap.c`
- Create: `tests/test_drawlist.c`

**Interfaces:**

```c
typedef struct Voxmap Voxmap;
typedef struct DrawList DrawList;
typedef struct SpriteEntity SpriteEntity;

Voxmap *loadVoxmap(const char *path);
void destroyVoxmap(Voxmap *map);
int voxmapHeightAt(const Voxmap *map, int x, int y);

void initDrawList(DrawList *list, size_t capacity);
void clearDrawList(DrawList *list);
bool appendVoxelFace(DrawList *list, ...);
bool appendSprite(DrawList *list, const SpriteEntity *sprite);
void sortDrawList(DrawList *list, const Camera3D *camera);
```

- [ ] Test ASCII heightmap loading and invalid-cell handling.
- [ ] Test bounds queries and varied heights.
- [ ] Test stable depth ordering and deterministic ties.
- [ ] Implement voxel top and visible side-face generation.
- [ ] Implement camera-anchored billboard sprites.
- [ ] Integrate the draw list with the GPU backend.
- [ ] Verify the demo map renders with visible height and occlusion differences.
- [ ] Commit `feat: render voxel map and billboard entities`.

**Acceptance:** The level view demonstrates multiple height levels, occlusion, and correctly draw-ordered billboard sprites.

---

### Task 9: Menu, Settings, Level, and Pause Scenes

**Files:**

- Create: `src/scenes/menu_scene.c/.h`
- Create: `src/scenes/settings_scene.c/.h`
- Create: `src/scenes/level_scene.c/.h`
- Create: `src/scenes/pause_scene.c/.h`
- Modify: `src/app.c`, `src/app.h`

- [ ] Build the three-item start menu.
- [ ] Connect Start to scene replacement.
- [ ] Connect Settings to a retained overlay.
- [ ] Connect Quit to clean application termination.
- [ ] Build level-scene camera controls.
- [ ] Build pause overlay with Resume and Quit to Menu.
- [ ] Verify the full Linux scene flow manually.
- [ ] Commit `feat: integrate vertical slice scenes`.

**Acceptance:** The complete menu-to-level-to-pause flow works on Linux.

---

### Task 10: Achievement and Audio Integration

**Files:**

- Create: `src/audio/audio.c`, `src/audio/audio.h`
- Create: `src/achievement.c`, `src/achievement.h`
- Create: `assets/audio/menu.wav`
- Create: `assets/audio/rotate.wav`
- Create: `assets/audio/achievement.wav`
- Modify: `src/scenes/level_scene.c`
- Modify: `src/ui/toast.c`

- [ ] Test achievement subscription and four-turn threshold.
- [ ] Implement SDL3 audio initialization and voice mixing.
- [ ] Subscribe the achievement system to camera rotation events.
- [ ] Publish toast and audio events when Orienteer unlocks.
- [ ] Render the timed achievement toast.
- [ ] Verify the event chain manually.
- [ ] Commit `feat: add achievement and audio event demo`.

**Acceptance:** Four quarter-turns produce an achievement toast and audible feedback through the event system.

---

### Task 11: Android and ARM Verification

**Files:**

- Modify: `android/app/build.gradle`
- Modify: `android/app/src/main/AndroidManifest.xml`
- Modify: `android/app/jni/CMakeLists.txt`
- Create: `README.md`
- Create: `docs/building.md`

- [ ] Build Android `arm64-v8a`.
- [ ] Build Android `x86_64`.
- [ ] Verify assets load from Android packaging.
- [ ] Verify density scaling and safe-area behavior.
- [ ] Verify touch gestures and Android Back.
- [ ] Build Linux ARM cross targets.
- [ ] Run CTOL, sanitizer, and release checks.
- [ ] Document Linux, ARM Linux, and Android commands.
- [ ] Commit `build: verify linux arm and android targets`.

**Acceptance:** Linux x86_64, ARM Linux, and Android artifacts build and launch the vertical slice.

---

## Plan self-check

- Repository exploration found an empty `isomata` directory, so no existing implementation contracts were omitted.
- Every requested vertical-slice feature has an owning task.
- The task order minimizes cross-module rework: events, scenes, UI, renderer, and platform interfaces are consumed by later scenes.
- Headless tests cover eventing, scenes, camera, voxel data, draw ordering, layout, scaling, and input.
- Android and ARM verification are explicit completion gates.
- No implementation code is included in this plan.

## Maturity

**M2 — implementation-ready draft**
Basis: architecture and product decisions are approved, repository structure is known, and executable subtasks are defined.

**Status:** `approved`
**repo_snapshot_ref:** `isomata-empty-2026-10-08`
**recommended_plan_path:** `docs/superpowers/plans/2026-10-08-isomata-vertical-slice.md`
