# Key Decisions: Hash Container Instrumentation

## 1. Stats as container member (not external map)

**Decision:** `HashContainerStats` is a `mutable` member of `HashMap`/`HashSet`, not stored in a separate lookup table.

**Why:** Zero indirection on the hot path — counter increments like `_stats.findHits++` are a direct member access. An external `std::unordered_map<void*, Stats>` would add a hash lookup on every instrumented operation.

**Tradeoff:** Each instrumented container is 272 bytes larger (328 vs 56 bytes). This only applies when `SPRING_HASH_INSTRUMENTATION` is defined.

## 2. `std::source_location` default parameter (not macros)

**Decision:** Constructors take `std::source_location loc = std::source_location::current()` to capture the caller's file/line automatically.

**Why:** No macro wrapping needed at usage sites — the existing `spring::unordered_map<K,V>` type alias works unchanged. C++23 guarantees `source_location` is available. The default parameter captures the *caller's* location, which is exactly what we want.

**Alternative considered:** Macro wrapping `spring::unordered_map` to inject `__FILE__`/`__LINE__`. Rejected because it would require changing the type alias from `using` to a macro, breaking any code that does `spring::unordered_map` in template contexts.

## 3. `spring_gettime()` for timing (not `rdtsc`)

**Decision:** Use the engine's existing `spring_gettime()` / `spring_time` for all timing measurements.

**Why:** Matches the TimeProfiler convention, avoids rdtsc-to-nanosecond conversion complexity, and `spring_gettime()` uses `clock_gettime` via vDSO on Linux (~20-30ns overhead per call). Since this is a profiling build, that overhead is acceptable.

**Tradeoff:** ~20-30ns overhead per timed operation. For a release/profiling hybrid build this could be significant on very hot find() calls. If this proves too costly, a follow-up could sample every Nth operation instead.

## 4. Stats NOT swapped during `swap()`

**Decision:** When two containers are swapped (including via move-assignment), their `_stats` are NOT exchanged. Only `updateLiveStats()` is called on both.

**Why:** `swap()` is used by move-assignment, and `clear_unordered_map()` does `cont = C()` which creates a temporary, move-assigns, then destroys the temporary. If stats were swapped, the original container's source location and accumulated counters would end up in the temporary and be destroyed. By keeping stats in place, each container maintains its identity across swaps.

**Tradeoff:** After a swap, the operation counters (findHits, etc.) stay with their original container even though the data moved. This means counters reflect "operations on this container object" not "operations on this data". For profiling purposes this is the right semantic — we care about which code path is hot, not which data set.

## 5. Tombstone tracking: incremental counters (not O(n) scan)

**Decision:** `numTombstones` is incremented on `erase()` and reset to 0 on `reserve()`/`clear()`. No per-operation scan of the state array.

**Why:** Scanning all buckets for ACTIVE state on every erase/insert would be O(n) — unacceptable overhead even for profiling.

**Tradeoff:** The count slightly over-reports between rehashes because when `find_or_allocate` reuses a tombstone slot for a new insert, we don't decrement the counter. The count self-corrects on every rehash (reset to 0). For profiling purposes this is accurate enough — it shows the general tombstone pressure trend.

## 6. TimeProfiler integration via periodic harvesting (not per-operation)

**Decision:** `HarvestForTimeProfiler()` is called once per frame in `CTimeProfiler::Update()`. It sums all containers' cumulative timing, computes the delta since last harvest, and feeds synthetic timers (`HashMap::find`, `HashMap::insert`, etc.) into the profiler.

**Why:** Calling `CTimeProfiler::AddTime()` on every individual find/insert would be far too expensive — it acquires a mutex. By harvesting aggregate deltas once per frame, we get the same profiler integration (% of frame time, per-frame graphs) with negligible overhead.

**Tradeoff:** Per-frame granularity only. You can't see which individual find() call within a frame was slowest — only total time per operation type per frame.

## 7. Probe histogram buckets: [1, 2, 3, 4-7, 8-15, 16+]

**Decision:** 6 fixed-size histogram buckets with exponentially growing ranges.

**Why:** Probe lengths of 1-3 are the "fast" cases where linear probing works well. 4-7 indicates moderate clustering. 8-15 is concerning. 16+ is pathological. This distribution gives visibility into the tail without requiring dynamic allocation.

**Tradeoff:** Coarse granularity in the upper ranges. If needed, the bucket boundaries could be adjusted, but 6 buckets keeps the per-container overhead to 144 bytes (3 histograms × 6 × 8 bytes).

## 8. Timers registered as "special" timers

**Decision:** `HarvestForTimeProfiler` passes `specialTimer = true` when calling `AddTime()`.

**Why:** Special timers are counted even when the profiler is disabled (the profiler can be toggled at runtime). Since hash container instrumentation is compile-time gated, it makes sense for the timers to always report when instrumentation is enabled — otherwise the data would be lost during periods when the profiler is off.

## 9. `HASH_INSTR(...)` / `HASHSET_INSTR(...)` macros

**Decision:** Separate macro names for HashMap and HashSet to avoid redefinition warnings if both headers are included.

**Why:** Both headers are standalone and may be included independently or together. Using the same macro name would produce redefinition warnings. Using separate names (`HASH_INSTR` for map, `HASHSET_INSTR` for set) avoids this.

**Note:** The `DCHECK_*` macros ARE duplicated across both headers (pre-existing issue, not introduced by us). The map macro was later renamed to `HASHMAP_INSTR` for consistency.

## 10. `setStatsSource()` + tagging macros for `std::array`-embedded containers

**Decision:** Added `setStatsSource(file, line)` method and `SPRING_TAG_HASH_SOURCE(container)` / `SPRING_TAG_HASH_ARRAY_SOURCE(arr)` macros. Applied at known sites where containers live inside `std::array`.

**Why:** `std::source_location::current()` as a default constructor parameter captures the *caller's* location. When the caller is `std::array`'s aggregate initialization, the location resolves to the standard library header (`<array>:94`) instead of user code. This affected 16+ high-activity containers (per-thread `blockMaps` in `MoveMath.cpp`).

**Tradeoff:** Requires manual tagging at each `std::array` site. Only 2-3 sites needed tagging; a fallback display (`<std-aggregate>`) handles any untagged containers. The macros are no-ops when instrumentation is disabled.
