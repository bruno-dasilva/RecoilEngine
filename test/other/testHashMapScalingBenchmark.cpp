/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Scaling benchmarks: sweeps N from 100 to 20000 using non-linear (exponential)
 * sampling — denser at low N, sparser at high N — with clusters of extra points
 * around each power of 2 to capture rehash boundaries.
 *
 * Run: ./test_HashMapScalingBenchmark -r compact-bench [catch2 options]
 * e.g. ./test_HashMapScalingBenchmark "Map<int,int>" -r compact-bench
 *      BENCH_RESERVE=0 ./test_HashMapScalingBenchmark -r compact-bench
 *
 * Scenarios covered (from HASHMAP_FINDINGS.md §2.3):
 *   Map<int,int>              — int-key scalar baseline
 *   Map<ptr,int>              — uintptr_t naïve strided baseline (control)
 *   Map<ptr_obj,int>          — ObjT* realistic pool (dominant shape: 84%)
 *   Map<ptr_obj,SmallStructV> — ObjT* → struct (7% + largest single shape)
 *   Map<int,SmallStructV>     — int  → struct (5%), with Churn for tombstone sweep
 *   Map<string,string>        — string baseline (sanity only, N=10000)
 */

#include "HashMapBenchmarkCommon.hpp"

// Fewer ops per sample than the fixed-N bench — the sweep has many N values
// and we need the total runtime to stay reasonable.
constexpr int BENCH_OPS = 100000;

// =============================================================================
// Container type aliases
// =============================================================================

using StdInt    = std::unordered_map<int, int, spring::synced_hash<int>>;
using SpringInt = spring::unordered_map<int, int>;
using UnsyncInt = spring::unsynced_map<int, int, spring::synced_hash<int>>;

// Naïve strided pointer baseline (128-byte aligned, same as before — control sample)
using StdPtr    = std::unordered_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;
using SpringPtr = spring::unordered_map<uintptr_t, int>;
using UnsyncPtr = spring::unsynced_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;

// Realistic-layout pointer → scalar (covers Map<ptr*, scalar> 84% category)
using StdPtrObj    = std::unordered_map<const ObjT*, int, spring::synced_hash<const ObjT*>>;
using SpringPtrObj = spring::unordered_map<const ObjT*, int>;
using UnsyncPtrObj = spring::unsynced_map<const ObjT*, int, spring::synced_hash<const ObjT*>>;

// Realistic-layout pointer → struct (covers Map<ptr*, struct> 7% category)
using StdPtrStr    = std::unordered_map<const ObjT*, SmallStructV, spring::synced_hash<const ObjT*>>;
using SpringPtrStr = spring::unordered_map<const ObjT*, SmallStructV>;
using UnsyncPtrStr = spring::unsynced_map<const ObjT*, SmallStructV, spring::synced_hash<const ObjT*>>;

// int → struct (covers Map<int, struct> 5% category, CobEngine tombstone pathology)
using StdIntStr    = std::unordered_map<int, SmallStructV, spring::synced_hash<int>>;
using SpringIntStr = spring::unordered_map<int, SmallStructV>;
using UnsyncIntStr = spring::unsynced_map<int, SmallStructV, spring::synced_hash<int>>;

using StdStrStr    = std::unordered_map<std::string, std::string, spring::synced_hash<std::string>>;
using SpringStrStr = spring::unordered_map<std::string, std::string>;
using UnsyncStrStr = spring::unsynced_map<std::string, std::string, spring::synced_hash<std::string>>;

// =============================================================================
// Helper macros — N-sweep over scalingSamples()
// =============================================================================

#define BENCH_SWEEP_MIXED(MapType, Kind) \
	for (int N : scalingSamples()) { \
		BENCHMARK_ADVANCED(std::string{"N=" + padN(N)})(Catch::Benchmark::Chronometer meter) { \
			benchmarkMapMixed<MapType>(meter, N, BENCH_OPS, Kind); \
		}; \
	}

#define BENCH_SWEEP_ITER(MapType) \
	for (int N : scalingSamples()) { \
		BENCHMARK_ADVANCED(std::string{"N=" + padN(N)})(Catch::Benchmark::Chronometer meter) { \
			benchmarkMapIterate<MapType>(meter, N, BENCH_OPS); \
		}; \
	}

// =============================================================================
// Scaling benchmarks
// Hierarchy: Container type → Workload → Implementation → N sweep
// This gives clean CSV columns: Container, Workload, Impl, Benchmark
// =============================================================================

// clang-format off

// --- int → int (existing baseline) ---
TEST_CASE("Map<int,int>", "[int]")
{
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdInt,    WorkloadKind::Mixed90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringInt, WorkloadKind::Mixed90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncInt, WorkloadKind::Mixed90) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdInt,    WorkloadKind::Mixed50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringInt, WorkloadKind::Mixed50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncInt, WorkloadKind::Mixed50) }
	}
	SECTION("iterate") {
		SECTION("std")      { BENCH_SWEEP_ITER(StdInt)    }
		SECTION("spring")   { BENCH_SWEEP_ITER(SpringInt) }
		SECTION("unsynced") { BENCH_SWEEP_ITER(UnsyncInt) }
	}
}

// --- uintptr_t → int (naïve strided baseline, kept as control sample) ---
TEST_CASE("Map<ptr,int>")
{
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtr,    WorkloadKind::Mixed90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtr, WorkloadKind::Mixed90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtr, WorkloadKind::Mixed90) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtr,    WorkloadKind::Mixed50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtr, WorkloadKind::Mixed50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtr, WorkloadKind::Mixed50) }
	}
	SECTION("iterate") {
		SECTION("std")      { BENCH_SWEEP_ITER(StdPtr)    }
		SECTION("spring")   { BENCH_SWEEP_ITER(SpringPtr) }
		SECTION("unsynced") { BENCH_SWEEP_ITER(UnsyncPtr) }
	}
}

// --- ObjT* → int (realistic pointer layout, dominant 84% shape) ---
TEST_CASE("Map<ptr_obj,int>", "[hot][ptr]")
{
	SECTION("find-hit only", "[findhit]") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtrObj,    WorkloadKind::FindHitOnly) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtrObj, WorkloadKind::FindHitOnly) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtrObj, WorkloadKind::FindHitOnly) }
	}
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtrObj,    WorkloadKind::Mixed90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtrObj, WorkloadKind::Mixed90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtrObj, WorkloadKind::Mixed90) }
	}
}

// --- ObjT* → SmallStructV (7% shape, dominant single shape from ModelDrawerData.h:100) ---
TEST_CASE("Map<ptr_obj,SmallStructV>", "[hot][ptr]")
{
	SECTION("find-hit only", "[findhit]") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtrStr,    WorkloadKind::FindHitOnly) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtrStr, WorkloadKind::FindHitOnly) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtrStr, WorkloadKind::FindHitOnly) }
	}
	SECTION("90% find / 10% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdPtrStr,    WorkloadKind::Mixed90) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringPtrStr, WorkloadKind::Mixed90) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncPtrStr, WorkloadKind::Mixed90) }
	}
}

// --- int → SmallStructV under Churn (tombstone scaling curve) ---
// The only way to observe probe-length growth due to tombstone accumulation
// across the N-sweep. spring::unordered_map should show super-linear cost
// growth vs std::unordered_map (chained, no tombstones) as N increases.
TEST_CASE("Map<int,SmallStructV>", "[hot][int]")
{
	SECTION("churn", "[churn]") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdIntStr,    WorkloadKind::Churn) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringIntStr, WorkloadKind::Churn) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncIntStr, WorkloadKind::Churn) }
	}
	SECTION("50% find / 50% mutate") {
		SECTION("std")      { BENCH_SWEEP_MIXED(StdIntStr,    WorkloadKind::Mixed50) }
		SECTION("spring")   { BENCH_SWEEP_MIXED(SpringIntStr, WorkloadKind::Mixed50) }
		SECTION("unsynced") { BENCH_SWEEP_MIXED(UnsyncIntStr, WorkloadKind::Mixed50) }
	}
}

// --- string → string (sanity only: <1% runtime cost per HASHMAP_FINDINGS.md §2.3) ---
// Single N=10000 point — enough to verify correctness and catch regressions
// without running the full sweep for a load-time-only workload.
TEST_CASE("Map<string,string>", "[str]")
{
	SECTION("90% find / 10% mutate") {
		BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<StdStrStr>   (meter, 10000, BENCH_OPS, WorkloadKind::Mixed90); };
		BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<SpringStrStr>(meter, 10000, BENCH_OPS, WorkloadKind::Mixed90); };
		BENCHMARK_ADVANCED("N=10000  unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<UnsyncStrStr>(meter, 10000, BENCH_OPS, WorkloadKind::Mixed90); };
	}
}

// clang-format on

#undef BENCH_SWEEP_MIXED
#undef BENCH_SWEEP_ITER
