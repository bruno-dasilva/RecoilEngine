# Profiling in RecoilEngine

RecoilEngine has three profiling approaches, each suited to different use cases.

## Built-in TimeProfiler

The engine's own profiling system, always available. Uses `SCOPED_TIMER` macros to measure CPU time across the game loop. Data is displayed via an in-game overlay (`ProfileDrawer`) toggled with the `/debug` console command.

- No build flags needed (always compiled in)
- Low overhead; can be enabled/disabled at runtime
- Shows per-frame timing, percentages, peaks, and lag detection
- Includes thread pool visualization

See: [TIMEPROFILER.md](TIMEPROFILER.md), [PROFILEDRAWER.md](PROFILEDRAWER.md), [GAME_LOOP_INSTRUMENTATION.md](GAME_LOOP_INSTRUMENTATION.md)

## Tracy Profiler

External profiler integration via [Tracy](https://github.com/wolfpld/tracy). Provides deep, interactive profiling with timeline visualization, memory tracking, and Lua plot support.

- Requires build flag: `-DTRACY_ENABLE=ON`
- `SCOPED_TIMER` macros automatically emit Tracy zones when enabled
- Supports memory allocation tracking and log message routing
- Lua scripts can push data to Tracy plots

See: [TRACY.md](TRACY.md)

## Linux perf

Standard Linux profiling via `perf record` / `perf report`. Useful for low-level CPU analysis (cache misses, branch mispredictions, call graphs).

- Requires a `RELWITHDEBINFO` or `PROFILE` build for symbols
- `PROFILE` build type adds `-pg` linker flags
- Visualization via [Hotspot](https://github.com/KDAB/hotspot) or `perf report`

See: [Profiling Linux Builds with Perf (wiki)](https://github.com/beyond-all-reason/RecoilEngine/wiki/Profiling-Linux-Builds-with-Perf)
