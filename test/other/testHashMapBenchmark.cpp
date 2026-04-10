/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Fixed-N benchmarks comparing std::unordered_map/set against spring::unordered_map/set
 * and spring::unsynced_map/set (all emilib open-addressing, linear probing).
 *
 * Run: ./test_HashMapBenchmark [catch2 options]
 * e.g. ./test_HashMapBenchmark "Map<int,int>"
 *      ./test_HashMapBenchmark "[hot][ptr]"
 *      ./test_HashMapBenchmark "[churn]"
 *      ./test_HashMapBenchmark "[findhit]"
 *
 * BENCH_RESERVE=0 ./test_HashMapBenchmark    # measure rehash impact
 *
 * Tags:
 *   [hot]      — representative scenario from HASHMAP_FINDINGS.md top-10 categories
 *   [ptr]      — pointer key
 *   [int]      — integer key
 *   [str]      — string key
 *   [churn]    — rolling-window workload to expose tombstone accumulation
 *   [findhit]  — 100% find-hit workload (matches ~95% find production hot paths)
 */

#include "HashMapBenchmarkCommon.hpp"

// Fixed number of operations per benchmark sample (independent of container size).
// Ensures Catch2's auto-estimated iterations-per-sample is consistent across N values.
constexpr int BENCH_OPS = 1000000;

// =============================================================================
// Helper macros to reduce repetition across N={100, 1000, 10000}.
// Std/Spring/Unsync must be defined as using-aliases in scope at the call site.
// =============================================================================

// Mixed or FindHitOnly or Churn workload across the three implementations
#define BENCH_N_MIXED(Kind) \
	BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 100,   BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 100,   BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=100    unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 100,   BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 1000,  BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 1000,  BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=1000   unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 1000,  BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Std>   (meter, 10000, BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Spring>(meter, 10000, BENCH_OPS, Kind); }; \
	BENCHMARK_ADVANCED("N=10000  unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapMixed<Unsync>(meter, 10000, BENCH_OPS, Kind); };

// Iterate workload across the three implementations
#define BENCH_N_ITER() \
	BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=100    unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Std>   (meter, 10000, BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Spring>(meter, 10000, BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkMapIterate<Unsync>(meter, 10000, BENCH_OPS); };

#define BENCH_N_SET_MIXED(findPct) \
	BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 100,   BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 100,   BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=100    unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 100,   BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 1000,  BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 1000,  BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=1000   unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 1000,  BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Std>   (meter, 10000, BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Spring>(meter, 10000, BENCH_OPS, findPct); }; \
	BENCHMARK_ADVANCED("N=10000  unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetMixed<Unsync>(meter, 10000, BENCH_OPS, findPct); };

#define BENCH_N_SET_ITER() \
	BENCHMARK_ADVANCED("N=100    std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=100    spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=100    unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 100,   BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=1000   unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 1000,  BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  std    ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Std>   (meter, 10000, BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  spring ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Spring>(meter, 10000, BENCH_OPS); }; \
	BENCHMARK_ADVANCED("N=10000  unsync ")(Catch::Benchmark::Chronometer meter) { benchmarkSetIterate<Unsync>(meter, 10000, BENCH_OPS); };

// =============================================================================
// Benchmarks — existing baselines (int, string, uintptr_t, sets)
// =============================================================================

// clang-format off

TEST_CASE("Map<int,int>", "[int]")
{
	using Std    = std::unordered_map<int, int, spring::synced_hash<int>>;
	using Spring = spring::unordered_map<int, int>;
	using Unsync = spring::unsynced_map<int, int, spring::synced_hash<int>>;

	SECTION("90% find / 10% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed90) }
	SECTION("50% find / 50% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed50) }
	SECTION("iterate")               { BENCH_N_ITER() }
}

TEST_CASE("Map<string,string>", "[str]")
{
	using Std    = std::unordered_map<std::string, std::string, spring::synced_hash<std::string>>;
	using Spring = spring::unordered_map<std::string, std::string>;
	using Unsync = spring::unsynced_map<std::string, std::string, spring::synced_hash<std::string>>;

	SECTION("90% find / 10% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed90) }
	SECTION("50% find / 50% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed50) }
	SECTION("iterate")               { BENCH_N_ITER() }
}

// Naive strided-heap baseline (all addresses 128-byte aligned, identical high bits).
// Retained as a control sample: compare against Map<ptr,int> to see the effect
// of realistic pointer layout on probe-chain distribution.
TEST_CASE("Map<uintptr_t,int>", "[int]")
{
	using Std    = std::unordered_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;
	using Spring = spring::unordered_map<uintptr_t, int>;
	using Unsync = spring::unsynced_map<uintptr_t, int, spring::synced_hash<uintptr_t>>;

	SECTION("90% find / 10% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed90) }
	SECTION("50% find / 50% mutate") { BENCH_N_MIXED(WorkloadKind::Mixed50) }
	SECTION("iterate")               { BENCH_N_ITER() }
}

TEST_CASE("Set<int>", "[int]")
{
	using Std    = std::unordered_set<int, spring::synced_hash<int>>;
	using Spring = spring::unordered_set<int>;
	using Unsync = spring::unsynced_set<int, spring::synced_hash<int>>;

	SECTION("90% find / 10% mutate") { BENCH_N_SET_MIXED(90) }
	SECTION("50% find / 50% mutate") { BENCH_N_SET_MIXED(50) }
	SECTION("iterate")               { BENCH_N_SET_ITER() }
}

TEST_CASE("Set<string>", "[str]")
{
	using Std    = std::unordered_set<std::string, spring::synced_hash<std::string>>;
	using Spring = spring::unordered_set<std::string>;
	using Unsync = spring::unsynced_set<std::string, spring::synced_hash<std::string>>;

	SECTION("90% find / 10% mutate") { BENCH_N_SET_MIXED(90) }
	SECTION("50% find / 50% mutate") { BENCH_N_SET_MIXED(50) }
	SECTION("iterate")               { BENCH_N_SET_ITER() }
}

// =============================================================================
// Benchmarks — new representative scenarios (from HASHMAP_FINDINGS.md §2.3)
// =============================================================================

// Map<ptr*, scalar> — 84% of pathfinding hash time (HASHMAP_FINDINGS.md §2.3).
// Dominant container: ModelsMemStorage.h:156 (Map<CWorldObject*, size_t>),
// 83M find-hits / 0 misses per pathfinding run.
//
// Uses the realistic ObjT* pointer pool (mixed 24/40/56 B strides + random gaps)
// to model real heap-address diversity. Compare against Map<uintptr_t,int> above
// to see the effect of pointer layout on probe behavior.
TEST_CASE("Map<ptr,int>", "[hot][ptr]")
{
	using Std    = std::unordered_map<const ObjT*, int, spring::synced_hash<const ObjT*>>;
	using Spring = spring::unordered_map<const ObjT*, int>;
	using Unsync = spring::unsynced_map<const ObjT*, int, spring::synced_hash<const ObjT*>>;

	SECTION("find-hit only", "[findhit]") { BENCH_N_MIXED(WorkloadKind::FindHitOnly) }
	SECTION("90% find / 10% mutate")      { BENCH_N_MIXED(WorkloadKind::Mixed90) }
}

// Map<ptr*, struct> — 7% of pathfinding hash time.
// Dominant containers: ModelDrawerData.h:100 scTransMemAllocMap (CUnit/CFeature* →
// ScopedTransformMemAlloc, insert-bound at ~2.5M inserts/run).
TEST_CASE("Map<ptr,SmallStructV>", "[hot][ptr]")
{
	using Std    = std::unordered_map<const ObjT*, SmallStructV, spring::synced_hash<const ObjT*>>;
	using Spring = spring::unordered_map<const ObjT*, SmallStructV>;
	using Unsync = spring::unsynced_map<const ObjT*, SmallStructV, spring::synced_hash<const ObjT*>>;

	SECTION("find-hit only", "[findhit]") { BENCH_N_MIXED(WorkloadKind::FindHitOnly) }
	SECTION("churn",         "[churn]")   { BENCH_N_MIXED(WorkloadKind::Churn) }
}

// Map<int, struct> — 5% of pathfinding hash time.
// Dominant container: CobEngine.h:25 (int → CCobThread), 84% tombstones
// + peak probe 1656 in pathfinding. The Churn workload reproduces tombstone
// accumulation: emilib inserts prefer INACTIVE over ACTIVE (tombstone) slots
// when scanning for an empty bucket, so tombstones are not reused and pile up.
TEST_CASE("Map<int,SmallStructV>", "[hot][int]")
{
	using Std    = std::unordered_map<int, SmallStructV, spring::synced_hash<int>>;
	using Spring = spring::unordered_map<int, SmallStructV>;
	using Unsync = spring::unsynced_map<int, SmallStructV, spring::synced_hash<int>>;

	SECTION("churn",               "[churn]") { BENCH_N_MIXED(WorkloadKind::Churn) }
	SECTION("50% find / 50% mutate")          { BENCH_N_MIXED(WorkloadKind::Mixed50) }
}

// Map<uint64_t, scalar> — scenario-gated: QTPFS PathManager (PathHashType → int).
// 0.4 ms regular → 405 ms pathfinding (1000× amplification per findings §2.2 row 8).
// 43-49% tombstones in pathfinding.
TEST_CASE("Map<uint64_t,int>", "[hot][int]")
{
	using Std    = std::unordered_map<uint64_t, int, spring::synced_hash<uint64_t>>;
	using Spring = spring::unordered_map<uint64_t, int>;
	using Unsync = spring::unsynced_map<uint64_t, int, spring::synced_hash<uint64_t>>;

	SECTION("find-hit only", "[findhit]") { BENCH_N_MIXED(WorkloadKind::FindHitOnly) }
	SECTION("churn",         "[churn]")   { BENCH_N_MIXED(WorkloadKind::Churn) }
}

// clang-format on

#undef BENCH_N_MIXED
#undef BENCH_N_ITER
#undef BENCH_N_SET_MIXED
#undef BENCH_N_SET_ITER
