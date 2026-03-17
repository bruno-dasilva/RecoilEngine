# TimeProfiler - Built-in Profiling System

The built-in profiler measures CPU time for instrumented code sections using RAII timer objects created by macros. Data is collected into a singleton `CTimeProfiler`, updated ~2Hz, and optionally rendered on screen by `ProfileDrawer`.

**Source files:** `rts/System/TimeProfiler.h`, `rts/System/TimeProfiler.cpp`

## Macros

All timer names must be compile-time string literals. Names are hashed via `hashString()` for storage.

| Macro | Tracy zone? | Description |
|---|---|---|
| `SCOPED_TIMER(name)` | Yes (Goldenrod) | Standard timer. Registers name, creates `ScopedTimer`. |
| `SCOPED_TIMER_NOREG(name)` | Yes (Goldenrod) | Same but skips `TimerNameRegistrar` (name must be pre-registered). |
| `SCOPED_SPECIAL_TIMER(name)` | No | Records time even when profiler is disabled. Used for critical timers like "Sim" and "Draw". |
| `SCOPED_SPECIAL_TIMER_NOREG(name)` | No | Special timer without registration. |
| `SCOPED_MT_TIMER(name)` | No | For thread pool workers. Records per-thread start/end times. |
| `SCOPED_ONCE_TIMER(name)` | Yes (Purple) | Logs elapsed time to infolog on destruction. One-shot measurement. |

## Timer Classes

### BasicTimer (base class)
Stores `nameHash` and `startTime`. `GetDuration()` returns elapsed time since construction.

### ScopedTimer
Used by `SCOPED_TIMER` and `SCOPED_SPECIAL_TIMER`. Maintains per-hash reference counters to handle nested timers with the same name -- only the outermost instance reports time via `CTimeProfiler::AddTime()` on destruction.

Constructor parameters:
- `nameHash`: hash of the timer name
- `autoShowGraph`: if true, enable graph display in ProfileDrawer (default: false)
- `specialTimer`: if true, records time even when profiler is disabled (default: false)

### ScopedMtTimer
Used by `SCOPED_MT_TIMER`. Accepts `bool _autoShowGraph = false` (the macro uses the default). Always calls `AddTime()` with `threadTimer=true`, which records start/end times into per-thread deques for thread barcode visualization.

### ScopedOnceTimer
Standalone timer (not derived from `BasicTimer`). On destruction, logs a formatted message to infolog using the format `"[%s][%s] %ims"` (function, name, milliseconds). The constructor accepts an optional `frmt` parameter to override this default format. Useful for measuring one-time operations like loading.

### TimerNameRegistrar
RAII helper that calls `CTimeProfiler::RegisterTimer(name)` on construction. Used by `SCOPED_TIMER` to ensure the hash-to-name mapping exists before any timing data is recorded. Detects hash collisions with an assertion.

## CTimeProfiler Singleton

Accessed via `CTimeProfiler::GetInstance()` (static local singleton).

### TimeRecord

Each profiled section has a `TimeRecord`:

```cpp
struct TimeRecord {
    static constexpr unsigned numFrames = 128;

    spring_time total;    // cumulative time since start
    spring_time current;  // time accumulated in current 500ms window
    std::array<spring_time, numFrames> frames;  // circular buffer of per-frame times

    float3 stats;  // .x = max latency (ms), .y = time percentage, .z = peak percentage
    float3 color;  // random RGB for visualization

    bool newPeak, newLagPeak, showGraph;
};
```

### Data Storage

- `profiles`: `unordered_map<unsigned, TimeRecord>` -- main storage keyed by name hash
- `sortedProfiles`: `vector<pair<string, TimeRecord>>` -- sorted copy for display
- `threadProfiles`: `vector<deque<pair<spring_time, spring_time>>>` -- per-thread timing history

### Update Cycle

`Update()` is called ~2Hz via a timed job in the game loop (`Game.cpp`):

1. Advances `currentPosition` in the 128-frame circular buffer
2. Clears the new frame slot for all profiles
3. Every 500ms: calculates `stats.y` (percentage = current_ms / 500ms), detects new peaks
4. Every 6 seconds: decays max latency (`stats.x *= 0.5f`)
5. Re-sorts profiles if needed (new timers added, or non-alphabetical sort active)
6. Cleans up thread profile entries older than 0.5s

### AddTime Flow

```
ScopedTimer destructor (when refCount hits 0)
  -> CTimeProfiler::AddTime(nameHash, startTime, deltaTime, showGraph, specialTimer, threadTimer)
     -> acquires profileMutex (if enabled)
     -> AddTimeRaw():
        - profiles[nameHash].total += deltaTime
        - profiles[nameHash].current += deltaTime
        - profiles[nameHash].frames[currentPosition] += deltaTime
        - updates max latency (stats.x)
        - if new profile: assigns random color, flags resort
        - if threadTimer: records to threadProfiles[ThreadPool::GetThreadNum()]
     -> also records own overhead as "Misc::Profiler::AddTime"
```

When the profiler is disabled (`enabled = false`), `AddTime()` is a no-op except for special timers.

### Sorting Types

Controlled via `SetSortingType()`:

| Enum | Value | Sorts by |
|---|---|---|
| `ST_ALPHABETICAL` | 0 | Timer name (ascending) |
| `ST_TOTALTIME` | 1 | Total accumulated time (descending) |
| `ST_CURRENTTIME` | 2 | Current percentage (descending) |
| `ST_MAXTIME` | 3 | Peak percentage (descending) |
| `ST_LAG` | 4 | Max latency in ms (descending) |

### Thread Safety

- `profileMutex` (spring::mutex) protects `profiles` and `sortedProfiles`
- `hashToNameMutex` protects the hash-to-name mapping
- `std::atomic<bool> enabled` for lock-free enable/disable checks
- When disabled, special timers bypass the lock (they're single-threaded)

### Key Constants

| Constant | Value | Location |
|---|---|---|
| `MAX_THREAD_HIST_TIME` | 0.5 seconds | `TimeProfiler.h:33` |
| `TimeRecord::numFrames` | 128 | `TimeProfiler.h:116` |
| Update window | 500ms | `TimeProfiler.cpp:249` |
| Lag decay interval | 6 seconds | `TimeProfiler.cpp:270` |

### Initialization

Constructor pre-registers these timers:
- `"Misc::Profiler::AddTime"` (self-profiling)
- `"Lua::Callins::Synced"`, `"Lua::Callins::Unsynced"`
- `"Lua::CollectGarbage::Synced"`, `"Lua::CollectGarbage::Unsynced"`

Then calls `ResetState()` which clears all data structures, initializes thread profiles to `ThreadPool::GetMaxThreads()` deques (conditional on `#ifdef THREADPOOL`), and sets `enabled = false`.

### PrintProfilingInfo

Logs a formatted table to infolog showing timer name, total time (ms), and last-500ms percentage for all sorted profiles. Called automatically in `HEADLESS` builds at game end, or manually via `/debuginfo profiling`.
