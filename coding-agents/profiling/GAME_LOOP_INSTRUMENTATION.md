# Game Loop Instrumentation

This document maps where profiling timers are placed throughout the engine's main loop and subsystems.

## Profiler Update Job

In `rts/Game/Game.cpp` (~line 311), the profiler update is registered as a timed job:

```cpp
j.f = []() -> bool {
    CTimeProfiler::GetInstance().Update();
    return true;
};
j.name = "Profiler::Update";
```

This runs at approximately 2Hz (every 500ms), advancing the circular buffer and recalculating statistics.

## Timer Hierarchy

The engine's main loop creates a hierarchy of profiled sections. Special timers (which record even when the profiler is disabled) are marked with `*`.

```
Update                          (SCOPED_TIMER)
├── Update::EventHandler
├── Update::WorldDrawer
│   ├── Update::WorldDrawer::{Sky,Water}
│   └── ...

Draw *                          (SCOPED_SPECIAL_TIMER)
├── Draw::DrawGenesis
├── Draw::World::CreateShadows
├── Draw::World::UpdateReflTex
├── Draw::World::UpdateSpecTex
├── Draw::World::UpdateSkyTex
├── Draw::World::UpdateShadingTex
├── Draw::Screen
│   ├── Draw::Screen::InputReceivers
│   └── Draw::Screen::DrawScreen
└── Misc::SwapBuffers

Sim *                           (SCOPED_SPECIAL_TIMER)
├── Sim::GameFrame
├── Sim::Script
└── (unit/feature/projectile/path updates)
```

## Key Instrumentation Points

### Game.cpp

| Line (~) | Timer | Type | Description |
|---|---|---|---|
| 1209 | `"Update"` | SCOPED_TIMER | Top-level unsynced update |
| 1404 | `"Update::EventHandler"` | SCOPED_TIMER | Event handler updates |
| 1438 | `"Draw"` | SCOPED_SPECIAL_TIMER | Top-level draw (always recorded) |
| 1448 | `"Draw::DrawGenesis"` | SCOPED_TIMER | Pre-draw event |
| 1512 | `"Draw::Screen"` | SCOPED_TIMER | Screen rendering |
| 1563 | `"Draw::Screen::InputReceivers"` | SCOPED_TIMER | UI input receivers |
| 1568 | `"Draw::Screen::DrawScreen"` | SCOPED_TIMER | Main screen draw |
| 1744 | `"Sim"` | SCOPED_SPECIAL_TIMER | Top-level simulation (always recorded) |
| 1752 | `"Sim::GameFrame"` | SCOPED_TIMER | Game frame event |
| 1779 | `"Sim::Script"` | SCOPED_TIMER | Unit script engine tick |

### WorldDrawer.cpp

| Line (~) | Timer | Description |
|---|---|---|
| 208 | `"Update::WorldDrawer"` | World drawer update |
| 229 | `"Update::WorldDrawer::{Sky,Water}"` | Sky/water updates |
| 249 | `"Draw::World::CreateShadows"` | Shadow map generation |
| 258 | `"Draw::World::UpdateReflTex"` | Reflection texture |
| 270 | `"Draw::World::UpdateSpecTex"` | Specular texture |
| 274 | `"Draw::World::UpdateSkyTex"` | Sky texture |
| 278 | `"Draw::World::UpdateShadingTex"` | Shading texture |

### GlobalRendering.cpp

| Line (~) | Timer | Description |
|---|---|---|
| 675 | `"Misc::SwapBuffers"` | OpenGL buffer swap |

## GPU Profiling

GPU frame time is measured using OpenGL timer queries in `CGlobalRendering`:

```cpp
// At frame start (Game.cpp ~line 1440):
globalRendering->SetGLTimeStamp(CGlobalRendering::FRAME_REF_TIME_QUERY_IDX);

// At frame end:
globalRendering->SetGLTimeStamp(CGlobalRendering::FRAME_END_TIME_QUERY_IDX);
```

The delta between these timestamps gives GPU frame time, displayed in `ProfileDrawer` as "GL=X.Xms":

```cpp
globalRendering->CalcGLDeltaTime(
    CGlobalRendering::FRAME_REF_TIME_QUERY_IDX,
    CGlobalRendering::FRAME_END_TIME_QUERY_IDX
) * 0.001f * 0.001f  // nanoseconds -> milliseconds
```

## ThreadPool Profiling

Thread pool workers use `SCOPED_MT_TIMER` to record execution times:

- Data stored in `CTimeProfiler::threadProfiles[threadNum]` as `deque<pair<spring_time, spring_time>>`
- Each entry is a (start, end) pair
- History limited to 0.5 seconds (`MAX_THREAD_HIST_TIME`)
- Visualized as horizontal bars in `ProfileDrawer::DrawThreadBarcode()`
- Disabled in `UNITSYNC` builds (from `rts/System/Threading/ThreadPool.h`):
  ```cpp
  #ifdef UNITSYNC
      #undef SCOPED_MT_TIMER
      #define SCOPED_MT_TIMER(x)
  #endif
  ```

## Frame Timing Events

The game loop emits `DbgTimingInfo` events consumed by `ProfileDrawer`:

| Event | When emitted | Description |
|---|---|---|
| `TIMING_VIDEO` | After draw phase | Draw frame timing |
| `TIMING_SIM` | After sim phase | Simulation frame timing |
| `TIMING_GC` | After Lua GC | Garbage collection timing |
| `TIMING_SWAP` | After buffer swap | OpenGL swap timing |
| `TIMING_UNSYNCED` | After unsynced update | Unsynced update timing |

These are defined in `rts/System/EventClient.h`.

## Headless Builds

In `HEADLESS` builds, `CTimeProfiler::PrintProfilingInfo()` is called at game end (~Game.cpp line 1859), printing a summary table of all timer data to the log.

## Adding New Timers

To instrument a new code section:

```cpp
#include "System/TimeProfiler.h"

void MyFunction() {
    SCOPED_TIMER("MySystem::MyFunction");
    // ... code to profile ...
}
```

For thread pool work:
```cpp
void MyThreadedWork() {
    SCOPED_MT_TIMER("MySystem::ThreadWork");
    // ... code running in thread pool ...
}
```

For one-shot measurements (e.g., loading):
```cpp
void LoadSomething() {
    SCOPED_ONCE_TIMER("LoadSomething");
    // ... loading code, time printed to log on completion ...
}
```

Timer names use `::` as a hierarchy separator by convention (e.g., `"Draw::World::CreateShadows"`). Use `SCOPED_SPECIAL_TIMER` only for timers that must record even when the profiler is disabled (currently just "Sim" and "Draw").
