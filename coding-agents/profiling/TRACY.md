# Tracy Profiler Integration

RecoilEngine integrates with [Tracy](https://github.com/wolfpld/tracy), an external real-time profiler. When enabled, the engine's `SCOPED_TIMER` macros automatically emit Tracy zones, and additional subsystems route memory allocations and log messages to Tracy.

## Build Flags

Set via CMake (in `rts/lib/CMakeLists.txt`):

| Flag | Default | Description |
|---|---|---|
| `TRACY_ENABLE` | OFF | Master switch. Enables all Tracy integration. |
| `TRACY_ON_DEMAND` | ON | Only captures data when a Tracy profiler client is connected. Prevents OOM from unbounded queue growth. Slightly higher overhead than always-on. |
| `TRACY_PROFILE_MEMORY` | OFF | Overrides global `operator new`/`delete` to track allocations. Expensive. Only compiled when mimalloc is not used. |
| `RECOIL_DETAILED_TRACY_ZONING` | FALSE | Enables `RECOIL_DETAILED_TRACY_ZONE` macro expansion to `ZoneScoped` throughout the codebase. For testing/debugging only. |

Example build command:
```bash
docker-build-v2/build.sh linux -DTRACY_ENABLE=ON -DTRACY_ON_DEMAND=ON
```

When `TRACY_ENABLE` is ON, the build also enables `RMLUI_TRACY_PROFILING` for RmlUI integration.

## How SCOPED_TIMER Emits Tracy Zones

From `rts/System/TimeProfiler.h`:

```cpp
#define SCOPED_TIMER(name)       ZoneScopedNC(name, tracy::Color::Goldenrod); ...
#define SCOPED_ONCE_TIMER(name)  ZoneScopedNC(name, tracy::Color::Purple); ...
```

- `ZoneScopedNC` creates a named Tracy zone with a color
- Standard timers use Goldenrod; once-timers use Purple
- `SCOPED_SPECIAL_TIMER` does NOT emit Tracy zones (intentional -- these are high-frequency timers where Tracy overhead is unwanted)

When `TRACY_ENABLE` is OFF, `ZoneScopedNC` expands to nothing (handled by Tracy's own headers).

## TracyDefs.h

**File:** `rts/System/Misc/TracyDefs.h`

```cpp
#include <tracy/Tracy.hpp>

#ifdef RECOIL_DETAILED_TRACY_ZONING
    #define RECOIL_DETAILED_TRACY_ZONE ZoneScoped
#else
    #define RECOIL_DETAILED_TRACY_ZONE do {} while(0)
#endif
```

`RECOIL_DETAILED_TRACY_ZONE` is placed throughout the rendering and UI code (e.g., every function in `ProfileDrawer.cpp`). It adds fine-grained Tracy zones that are too noisy for normal use but helpful for deep debugging.

## Memory Tracking

**File:** `rts/System/TraceMemory.cpp`

Compiled only when `TRACY_PROFILE_MEMORY` is ON and `USE_MIMALLOC` is OFF (see `rts/System/CMakeLists.txt`).

Overrides global `operator new` and `operator delete`:
```cpp
void* operator new(std::size_t count) {
    auto ptr = recoil::malloc(count);
    TracyAlloc(ptr, count);
    return ptr;
}

void operator delete(void* ptr) noexcept {
    TracyFree(ptr);
    recoil::free(ptr);
}
```

This feeds every heap allocation into Tracy's memory profiling view.

## Log Message Routing

**File:** `rts/System/Log/TracySink.cpp`

When `TRACY_ENABLE` is defined, automatically registers a log backend sink before `main()`:

```cpp
static void log_sink_record_tracy(int level, const char* section, const char* record) {
    auto buf = fmt::format("[{}:{}][{}] {}",
        log_util_levelToString(log_util_getNearestLevel(level)),
        level, section, record);
    TracyMessageS(buf.c_str(), buf.size(), 30);
}
```

All engine log messages (debug, info, warning, error) appear in Tracy's message view with a callstack depth of 30.

## Lua Tracy Extras

**File:** `rts/Lua/LuaTracyExtra.cpp`

Lua scripts can push data to Tracy plots:

### tracy.LuaTracyPlotConfig(plotName, formatType, stepwise, fill, color)
Configures plot appearance. Called once per plot.
- `formatType`: `"Number"` (default), `"Percentage"`, or `"Memory"`
- `stepwise`: boolean, default `true`
- `fill`: boolean, default `false`
- `color`: uint32 BGR, default `0xFFFFFF` (white)

### tracy.LuaTracyPlot(plotName, plotValue)
Updates a plot with a numeric value. Call each frame or as data changes.

Lua plot names are stored in a static `std::set` to provide the stable string pointers that Tracy requires. These are never cleaned up (assumes a small number of plots).

## Lua Zone Annotations

From the [wiki](https://github.com/beyond-all-reason/RecoilEngine/wiki/Tracy-Profiling), Lua code can also annotate Tracy zones:

```lua
tracy.ZoneBeginN("MyZoneName")
-- code to profile
tracy.ZoneEnd()
```

And send messages:
```lua
tracy.Message("debug info here")
```

## Setup Instructions

1. Download the Tracy profiler GUI (v0.9.1+) from [Tracy releases](https://github.com/wolfpld/tracy/releases)
2. Build the engine with `-DTRACY_ENABLE=ON` (and optionally `-DTRACY_ON_DEMAND=ON`)
3. Launch the engine
4. Open Tracy profiler and connect to `localhost`

For detailed setup, see:
- [Tracy Profiling (wiki)](https://github.com/beyond-all-reason/RecoilEngine/wiki/Tracy-Profiling)
- `doc/site/content/development/profiling-with-tracy.md`
