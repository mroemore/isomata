# Isomata Vertical Slice — Design Specification

**Status:** approved 2026-10-08
**Scope:** SDL3/C11 isometric engine vertical slice

## 1. Purpose

Build a cross-platform C engine rendering true 3D voxel-style tilemaps through an orthographic isometric camera. The first slice demonstrates:

- Scene transitions and persistent overlays.
- Resolution/DPI-independent UI.
- Keyboard, mouse, and touch input.
- Queued pub/sub messaging.
- Billboard entities with deterministic occlusion ordering.
- Basic audio.
- Linux x86_64, ARM Linux, and Android builds.

## 2. Technical decisions

- SDL3 with SDL_gpu.
- Vulkan/SPIR-V rendering path for the initial targets.
- C11.
- Spectrax naming: camelCase functions, PascalCase types, tabs, `X_COUNT` sentinels.
- Dotzy architecture: subsystem directories, vtables, platform twins, invariant-focused headers.
- Orthographic camera with true-isometric default pitch.
- Painter's sorting for the initial transparent world renderer.
- Meson desktop builds; SDL3 Android Gradle/CMake scaffold.
- Virtual-pixel UI with conversion only at rendering and pointer-input seams.

## 3. Runtime architecture

```text
SDL event polling
    ↓
Input command translation
    ↓
Active scene update
    ↓
Queued event dispatch
    ↓
Active scene rendering
    ↓
UI overlay rendering
    ↓
GPU presentation
```

Scene mutations are deferred to safe update boundaries. Event callbacks cannot directly invalidate the currently executing scene.

## 4. Core modules

```text
src/
├── app.c/.h
├── main.c
├── events.c/.h
├── scene.c/.h
├── input/
├── render/
├── scenes/
├── ui/
├── audio/
├── platform/
└── util/
```

### Scene interface

Scenes use a vtable containing:

- `init`
- `update`
- `draw`
- `unload`
- `size`
- `name`

A scene stack supports:

- `pushScene`: retain the underlying scene.
- `popScene`: unload and destroy the top scene.
- `replaceScene`: unload the current scene before loading another.

The top scene updates. Overlay scenes may render over a retained underlying scene while preventing its updates.

### Event system

Events are queued and dispatched once per frame.

Each event contains:

- Topic.
- Event type.
- Copied payload bytes.
- Subscriber callback and context.

Initial topics include engine, gameplay, UI, audio, and achievement events.

### Rendering

The renderer contains:

- SDL_gpu device and swapchain management.
- Orthographic camera.
- Voxel face generation.
- Billboard generation.
- Deterministic back-to-front draw list.
- Textured quad pipeline.
- SPIR-V shaders with source and regeneration tooling retained.

The slice uses a single merged draw list for terrain and transparent sprites. Depth-buffer optimization is deferred.

### Voxel map

The demo map is loaded from an ASCII heightmap. Each cell stores at least:

- X/Y coordinate.
- Height.
- Tile/face material identifier.

The demo includes a trench, stepped elevations, and a tall obstruction to visibly exercise occlusion.

### UI

The UI is a tree of vtable-backed elements and layout panes. Initial elements:

- Label.
- Button.
- Vertical menu.
- Overlay panel.
- Achievement toast.

Layout uses weights, padding, gaps, aspect-adaptive panes, and safe-area insets. All UI coordinates remain virtual until the renderer/input seams.

### Input

Commands include:

- Navigation.
- Select.
- Back.
- Rotate clockwise/counter-clockwise.
- Zoom.
- Pan.

Mappings support keyboard, mouse, touch, and gamepad-compatible expansion. Touch tap, drag, and pinch become commands or pointer gestures. Android back maps to Back.

### Audio

A small SDL3 audio mixer supports:

- Preloaded WAV assets.
- Multiple voices.
- Master gain.
- Menu, rotation, and achievement sounds.

## 5. Vertical slice flow

### Start menu

Three selectable items:

1. Start — replace menu with level scene.
2. Settings — push placeholder overlay; Back pops it.
3. Quit — terminate cleanly.

### Level

Displays:

- Isometric voxel heightmap.
- Multiple height levels.
- Occluding terrain.
- Billboard sprite entities.
- 90° camera rotation with interpolation.
- Zoom.
- Pan.
- Pause overlay.

### Achievement

After four quarter-turns, the event bus routes a gameplay event to an achievement subscriber. The subscriber publishes a UI toast and audio event for the in-memory **Orienteer** achievement.

### Pause overlay

The level remains loaded and visible, but stops updating beneath the overlay. Resume and Quit to Menu are available.

## 6. Build targets

- Linux x86_64.
- ARM Linux with Vulkan-capable hardware.
- Android `arm64-v8a` and `x86_64`.
- Android minimum API: 26.
- SDL3 and SDL3_ttf Android dependencies supplied through official AAR/prefab integration.
- Shared engine sources compiled through desktop Meson and Android CMake.

## 7. Verification

Pure modules receive headless tests:

- Event queue and subscription lifetime.
- Scene stack behavior.
- Camera projection and rotation.
- Voxel map loading/querying.
- Draw-list ordering.
- UI scale conversions.
- Layout behavior.

The completed slice must be manually exercised on Linux and Android. CTOL coverage and sanitizer gates are included in the implementation plan.

## 8. Explicit non-goals

Combat, pathfinding, AI, persistent saves, functional settings, lighting/shadows, skeletal animation, particles, hot reload, editor tooling, networking, streaming, OpenGL fallback, and non-requested platforms.
