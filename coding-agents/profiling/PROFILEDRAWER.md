# ProfileDrawer - In-Game Profiling HUD

The `ProfileDrawer` renders a real-time profiling overlay on screen, showing timer data from `CTimeProfiler`, frame timing breakdowns, thread pool utilization, and engine performance metrics.

**Source files:** `rts/Game/UI/ProfileDrawer.h`, `rts/Game/UI/ProfileDrawer.cpp`

## Enabling/Disabling

- **Console command:** `/debug` toggles the overlay on/off
- **Code:** `ProfileDrawer::SetEnabled(bool)` creates or destroys the singleton instance
- When enabled, calls `CTimeProfiler::GetInstance().ResetPeaks()` to reset peak indicators
- The `ProfileDrawer` registers itself as a `CEventClient` to receive draw and timing events

## Drawing Components

`DrawScreen()` renders five components in this order:

### 1. Thread Barcode (`DrawThreadBarcode`)
- Shows thread pool utilization as horizontal colored bars
- One row per active `ThreadPool` thread, covering 0.5 seconds of history
- Data comes from `CTimeProfiler::GetThreadProfiles()` (populated by `SCOPED_MT_TIMER`)
- A red "feeder" line indicates the current time position
- Located at screen coordinates (0.01, 0.30) to (0.30, 0.35)

### 2. Frame Barcode (`DrawFrameBarcode`)
- Horizontal timeline showing 0.5 seconds of frame timing history
- Color-coded by phase:
  - **Red**: Sim frames
  - **Green**: Video (draw) frames
  - **Cyan**: Swap buffer frames
  - **Yellow**: Unsynced update frames
  - **Magenta**: GC (garbage collection) frames
- Red feeder line shows current time; scale bar indicates 30FPS frame length
- Located at (0.01, 0.21) to (~0.55, 0.26)

### 3. Info Text (`DrawInfoText`)
Performance metrics in numbered rows:
1. Draw and Sim frame rates (Hz)
2. Draw and Sim frame tick counters
3. Sim, Update, and Draw frame times (ms), plus GL time from GPU timer queries. Values turn red when exceeding thresholds (sim > 16ms, update > 30ms, draw > 16ms)
4. Current and wanted simulation speed multipliers
5. Synced/Unsynced projectile counts, particle count, saturation
6. Pathfinder type and queued update counts
7. Lua memory allocation: total MB, allocation count, time, state count
8. GPU memory: used / total (MB)
9. SOP (Sim Object Pool) memory: Units, Features, Projectiles, Weapons (allocated/freed KB)

### 4. Profiler Table (`DrawProfiler`)
- Located at screen right side (x: 0.6 to 0.99, y: 0.95 downward)
- Columns: sum-time, cur-%usage, max-%usage, lag (ms), title
- Each timer has a colored selection box (random color per timer)
- Clicking a timer box toggles its graph display
- Graph lines show 128-frame history scaled by GAME_SPEED
- Refreshes sorted profile data every 10 draw frames to reduce lock contention

### 5. Buffer Stats (`DrawBufferStats`)
- Font metrics and render buffer utilization
- Shows element/index counts and submission batches per render buffer

## Frame Timing Events

`ProfileDrawer` receives timing events via the `DbgTimingInfo` callback:

```cpp
enum DbgTimingInfoType {
    TIMING_VIDEO,    // Draw frame
    TIMING_SIM,      // Simulation frame
    TIMING_GC,       // Garbage collection
    TIMING_SWAP,     // Buffer swap
    TIMING_UNSYNCED  // Unsynced update
};
```

These are stored as `deque<pair<spring_time, spring_time>>` (start, end pairs) and used by `DrawFrameBarcode`. The `Update()` method discards entries older than 0.5 seconds.

Timing events are emitted from various points in the game loop (see [GAME_LOOP_INSTRUMENTATION.md](GAME_LOOP_INSTRUMENTATION.md)).

## Console Commands

From `rts/Game/UnsyncedGameCommands.cpp`:

### `/debug [on|off|reset]`
- `on`/`off`: Enable/disable the profiler overlay and `CTimeProfiler`
- `reset`: Clear all profiling data and re-initialize

### `/debug sort <type>`
Sort the profiler table. Types:
- `alphabetical` (default)
- `totaltime` / `total`
- `current` / `cur` / `currenttime`
- `max` / `maxtime`
- `lag`

### `/debuginfo profiling`
Calls `CTimeProfiler::GetInstance().PrintProfilingInfo()` to log a text table of all timer data to infolog.

## Layout Constants

| Constant | Value | Description |
|---|---|---|
| `MIN_X_COOR` | 0.6 | Profiler table left edge |
| `MAX_X_COOR` | 0.99 | Profiler table right edge |
| `MIN_Y_COOR` | 0.95 | Profiler table top edge |
| `LINE_HEIGHT` | 0.013 | Row height per timer |
| `MAX_FRAMES_HIST_TIME` | 0.5s | Frame barcode history window |
