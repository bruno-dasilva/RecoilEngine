/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/**
 * Shared helpers for testHashMapBenchmark.cpp and testHashMapScalingBenchmark.cpp.
 *
 * Includes:
 *  - Xorshift64 RNG, key/value generators, checksum helpers
 *  - ObjT pointer pool: realistic heap-address layout for pointer-keyed benchmarks
 *  - SmallStructV: representative small-struct value type (~24 B)
 *  - WorkloadKind enum: Mixed90, Mixed50, FindHitOnly, Churn (tombstone-aging)
 *  - prepareMapWorkload, populateMap/Set, benchmark runner templates
 *  - scalingSamples(), padN() for scaling sweep benchmarks
 *
 * BENCH_OPS is intentionally NOT defined here — each TU defines its own value
 * (1,000,000 for fixed-N, 100,000 for scaling) to keep per-sample runtimes consistent.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "System/UnorderedMap.hpp"
#include "System/UnorderedSet.hpp"
#include "System/SpringHash.h"

#include <catch_amalgamated.hpp>

// =============================================================================
// Fast RNG (xorshift64)
//   std::rand() is much slower than PCG/xorshift and otherwise
//   dominates the runtime in these benchmarks.
// =============================================================================

struct Xorshift64 {
	uint64_t state;

	explicit Xorshift64(uint64_t seed = 0xDEADBEEFCAFEBABEull) : state(seed) {}

	uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}

	uint64_t nextBelow(uint64_t range) { return next() % range; }
};

// Set BENCH_RESERVE=0 to disable .reserve() calls and measure rehashing impact.
static const bool USE_RESERVE = []() {
	const char* env = std::getenv("BENCH_RESERVE");
	return !env || std::string(env) != "0";
}();

// =============================================================================
// Key and value types
//
// Key families (matching real engine containers from HASHMAP_FINDINGS.md §2.3):
//
//   int       → entity IDs (units, features, projectiles, COB thread IDs)
//   uint64_t  → QTPFS PathHashType, CFontTexture glyph caches
//   uintptr_t → legacy strided-heap baseline (kept as control sample)
//   const ObjT* → pointer-keyed containers (dominant: 84% of pathfinding hash time)
//                  Uses realistic heap-address layout (mixed strides, holes) to
//                  reproduce the low-bit clustering identified in findings §2.4 #2.
//   std::string → unit/weapon/feature def names (load-time only; <1% runtime)
//
// Value families:
//   int           → scalar values (most common)
//   SmallStructV  → representative small POD struct (~24 B) for the
//                   Map<ptr*, struct> and Map<int, struct> categories
//   std::string   → string-to-string baseline
// =============================================================================

// ------------------------------------------------------------------
// ObjT: empty tag type used as pointer key.
// One type covers all pointer-keyed containers (we don't need four
// separate tags — one representative scenario per category is enough).
// ------------------------------------------------------------------
struct ObjT {};

// ------------------------------------------------------------------
// ObjPool: pool of "fake objects" at addresses representative of real
// heap layout, to reproduce pointer-hash clustering.
//
// Rationale (findings §1.4, §2.4 #2):
//   synced_hash<T*> returns lo32 ^ hi32(address). For heap pointers,
//   hi32 is nearly constant, so the hash is effectively the low 32 bits.
//   With power-of-two bucket counts, only log2(N) bits matter, and
//   16-byte heap alignment wipes out the bottom 4 bits entirely.
//
//   The naïve uintptr_t generator (base + n*128) lands every key on
//   aliasing bucket positions and MASKS the problem rather than
//   reproducing it. This pool uses mixed strides (24/40/56 B) so that
//   some entries have bit-3 set (24B stride is not a multiple of 16),
//   plus random cacheline gaps every 64 entries to simulate heap holes.
// ------------------------------------------------------------------
struct ObjPool {
	char* buf = nullptr;
	std::vector<const ObjT*> ptrs;

	static constexpr int kCapacity = 35000; // covers 3*N for N up to 10000
	static constexpr size_t kStrides[3] = {24, 40, 56};

	ObjPool() {
		// Max offset: kCapacity entries * max stride 56 + max gap (3 * 64) per 64 entries
		const size_t maxGaps = (kCapacity / 64 + 1) * 3 * 64;
		const size_t bufSize = static_cast<size_t>(kCapacity) * 56 + maxGaps + 512;
		buf = static_cast<char*>(std::malloc(bufSize));
		assert(buf != nullptr);
		ptrs.reserve(kCapacity);

		Xorshift64 rng(0xABCDEF0123456789ULL); // fixed seed for reproducibility
		size_t offset = 16; // start 16 B in for alignment variety
		for (int i = 0; i < kCapacity; ++i) {
			// Random 0-3 cache-line gaps every 64 entries simulate heap holes
			if ((i & 63) == 0)
				offset += static_cast<size_t>(rng.nextBelow(4)) * 64;
			ptrs.push_back(reinterpret_cast<const ObjT*>(buf + offset));
			offset += kStrides[i % 3];
		}
	}

	~ObjPool() { std::free(buf); }
	ObjPool(const ObjPool&) = delete;
	ObjPool& operator=(const ObjPool&) = delete;

	const ObjT* ptrAt(int n) const {
		return ptrs[static_cast<size_t>(n) % ptrs.size()];
	}
};

inline ObjPool& getObjPool() {
	static ObjPool pool;
	return pool;
}

// ------------------------------------------------------------------
// SmallStructV: representative small-struct value type.
//
// Sized at ~24 B to model the value shapes in the Map<ptr*, struct>
// and Map<int, struct> categories that together account for 12% of
// pathfinding hash time (ScopedTransformMemAlloc at 16B + padding,
// per-frame int counters, etc.). Trivially copyable so benchmark
// overhead is proportional to hash operations, not value copies.
// ------------------------------------------------------------------
struct SmallStructV {
	size_t a = 0;
	size_t b = 0;
	uint32_t c = 0;
	uint32_t tag = 0;
	// 24 bytes on LP64 (16 + 4 + 4)
};

// =============================================================================
// Faction/unit-type strings for string-keyed benchmarks
// =============================================================================

static constexpr const char* kFactions[] = {
	"arm", "cor", "leg", "rap", "gok", "aven", "gear", "claw"
};
static constexpr const char* kUnitTypes[] = {
	"com", "llt", "hlt", "mex", "fus", "war", "flea", "flash",
	"pit", "lab", "fac", "air", "sea", "sub", "tank", "bot"
};
static constexpr int kNumFactions  = static_cast<int>(sizeof(kFactions) / sizeof(kFactions[0]));
static constexpr int kNumUnitTypes = static_cast<int>(sizeof(kUnitTypes) / sizeof(kUnitTypes[0]));

// =============================================================================
// makeKey<T>(int n) — generate a key of type T from integer index n
// =============================================================================

template<typename T>
T makeKey(int n) {
	if constexpr (std::is_same_v<T, uintptr_t>) {
		// Naïve strided baseline: kept as a control sample to compare against
		// the realistic ObjT* pool. All entries are 128-byte aligned.
		constexpr uintptr_t heapBase = 0x555555550000ULL;
		return heapBase + static_cast<uintptr_t>(n) * 128;
	} else if constexpr (std::is_integral_v<T>) {
		return static_cast<T>(n);
	} else {
		// std::string — RTS unit def names: "armcom", "corllt", "gokflea", ...
		std::string key;
		key += kFactions[n % kNumFactions];
		key += kUnitTypes[(n / kNumFactions) % kNumUnitTypes];
		if (n >= kNumFactions * kNumUnitTypes)
			key += std::to_string(n / (kNumFactions * kNumUnitTypes));
		return key;
	}
}

// Full specialization for const ObjT* — uses the realistic pointer pool.
template<>
inline const ObjT* makeKey<const ObjT*>(int n) {
	return getObjPool().ptrAt(n);
}

// =============================================================================
// makeValue<V>(int n) — generate a mapped value of type V from index n
// Falls back to makeKey<V>(n) for all types except SmallStructV.
// =============================================================================

template<typename V>
V makeValue(int n) {
	if constexpr (std::is_same_v<V, SmallStructV>) {
		SmallStructV v;
		v.a   = static_cast<size_t>(n);
		v.b   = static_cast<size_t>(n) * 3;
		v.c   = static_cast<uint32_t>(n);
		v.tag = static_cast<uint32_t>(n >> 8);
		return v;
	} else {
		return makeKey<V>(n);
	}
}

// =============================================================================
// toChecksum — convert a value to a uint64_t to prevent dead-code elimination
// =============================================================================

template<typename T>
uint64_t toChecksum(const T& v) {
	if constexpr (std::is_integral_v<T>) {
		return static_cast<uint64_t>(v);
	} else if constexpr (std::is_pointer_v<T>) {
		return reinterpret_cast<uintptr_t>(v);
	} else if constexpr (std::is_same_v<T, SmallStructV>) {
		return v.a ^ v.b ^ static_cast<uint64_t>(v.c);
	} else {
		return v.size(); // std::string: length is a cheap proxy
	}
}

// =============================================================================
// WorkloadKind — selects the operation mix and container preparation
//
//   Mixed90      90% find / 5% insert / 5% erase  (existing baseline)
//   Mixed50      50% find / 25% insert / 25% erase (existing baseline)
//   FindHitOnly  100% find on keys in [0,N)         (matches ~95% find hot paths)
//   Churn        Rolling insert+erase to age the map with tombstones, then
//                measure find-hit on the aged container.
//                Targets the tombstone-accumulation pathology observed in
//                CobEngine (84% tombstones, peak probe 1656) and QTPFS PathManager
//                (43-49% tombstones) per HASHMAP_FINDINGS.md §2.4 #3.
// =============================================================================

enum class WorkloadKind { Mixed90, Mixed50, FindHitOnly, Churn };

// =============================================================================
// Operation types
// =============================================================================

enum class OpType : uint8_t { Find, Insert, Erase };

template<typename K, typename V> struct MapOp { OpType type; K key; V value; };
template<typename K>             struct SetOp { OpType type; K key; };

// =============================================================================
// Container population helpers
// =============================================================================

template<typename Map>
Map populateMap(int n) {
	using K = typename Map::key_type;
	using V = typename Map::mapped_type;
	Map map;
	if (USE_RESERVE) map.reserve(n * 2);
	for (int i = 0; i < n; ++i)
		map[makeKey<K>(i)] = makeValue<V>(i);
	return map;
}

template<typename Set>
Set populateSet(int n) {
	using K = typename Set::value_type;
	Set set;
	if (USE_RESERVE) set.reserve(n * 2);
	for (int i = 0; i < n; ++i)
		set.insert(makeKey<K>(i));
	return set;
}

// =============================================================================
// Workload preparation
//
// prepareMapWorkload returns a pre-populated map + a vector of operations.
// The benchmark runner copies the map meter.runs() times and measures the op loop.
//
// For Mixed workloads:
//   Find   — keys from [0, 2N) → ~50% hit rate on initial contents
//   Insert — keys from [N, 3N) → new keys not in the initial map
//   Erase  — keys from [0, N)  → keys that exist in the initial map
//
// For FindHitOnly:
//   Find   — keys from [0, N) → 100% hit rate
//
// For Churn:
//   Phase 1 (pre-warm, not measured): rolling window insert+erase deposits
//            tombstones.  emilib inserts prefer INACTIVE slots over ACTIVE
//            (tombstone) slots when scanning for an empty bucket, so tombstones
//            accumulate rather than being immediately reused.
//   Phase 2 (measured): find-hit ops on the live key window in the aged map.
//            Exercises the probe-length overhead of tombstone-heavy tables.
// =============================================================================

template<typename Map>
std::pair<Map, std::vector<MapOp<typename Map::key_type, typename Map::mapped_type>>>
prepareMapWorkload(int n, int numOps, WorkloadKind kind) {
	using K = typename Map::key_type;
	using V = typename Map::mapped_type;

	// --- Churn: build tombstones, then measure find-hit on aged map ---
	if (kind == WorkloadKind::Churn) {
		// Window = live entry count (constant throughout aging).
		// Reserve minimal headroom so tombstones accumulate faster.
		// reserve(n) → num_buckets ≈ next_pow2(1.5n).
		// With _num_filled = window = n/2, fill fraction ≈ 33% — well below
		// the 0.66 load-factor rehash threshold, so tombstones are not cleared.
		const int window = n / 2;
		Map map;
		map.reserve(n);
		for (int i = 0; i < window; ++i)
			map[makeKey<K>(i)] = makeValue<V>(i);

		// Rolling window: insert key[window+i], erase key[i].
		// After ageOps iterations:
		//   live keys = [ageOps, ageOps+window) (last 'window' inserts)
		//   all keys [0, ageOps) have been erased → tombstones deposited
		const int ageOps = n * 2;
		for (int i = 0; i < ageOps; ++i) {
			map[makeKey<K>(window + i)] = makeValue<V>(window + i);
			map.erase(makeKey<K>(i));
		}

		// Measurement ops: find-hit on current live keys [ageOps, ageOps+window).
		Xorshift64 rng;
		std::vector<MapOp<K, V>> ops;
		ops.reserve(numOps);
		for (int j = 0; j < numOps; ++j) {
			const int idx = ageOps + static_cast<int>(rng.nextBelow(static_cast<uint64_t>(window)));
			ops.push_back({OpType::Find, makeKey<K>(idx), {}});
		}
		return {std::move(map), std::move(ops)};
	}

	// --- Standard workloads ---
	Xorshift64 rng;
	Map map = populateMap<Map>(n);

	std::vector<MapOp<K, V>> ops;
	ops.reserve(numOps);

	if (kind == WorkloadKind::FindHitOnly) {
		// 100% find on [0, N): guaranteed hit, no inserts or erases.
		// Matches the lookup-dominated hot paths (ModelsMemStorage: 83M find-hits,
		// 0 misses per pathfinding run per HASHMAP_FINDINGS.md §2.2 row 1).
		for (int i = 0; i < numOps; ++i) {
			ops.push_back({OpType::Find, makeKey<K>(static_cast<int>(rng.nextBelow(n))), {}});
		}
	} else {
		// Mixed90 or Mixed50
		const int findPct   = (kind == WorkloadKind::Mixed90) ? 90 : 50;
		const int insertPct = (100 - findPct) / 2;
		for (int i = 0; i < numOps; ++i) {
			const int roll = static_cast<int>(rng.nextBelow(100));
			if (roll < findPct) {
				ops.push_back({OpType::Find, makeKey<K>(static_cast<int>(rng.nextBelow(2 * n))), {}});
			} else if (roll < findPct + insertPct) {
				const int k = n + static_cast<int>(rng.nextBelow(2 * n));
				ops.push_back({OpType::Insert, makeKey<K>(k), makeValue<V>(k)});
			} else {
				ops.push_back({OpType::Erase, makeKey<K>(static_cast<int>(rng.nextBelow(n))), {}});
			}
		}
	}

	return {std::move(map), std::move(ops)};
}

template<typename Set>
std::pair<Set, std::vector<SetOp<typename Set::value_type>>>
prepareSetWorkload(int n, int numOps, int findPct) {
	using K = typename Set::value_type;

	Xorshift64 rng;
	Set set = populateSet<Set>(n);

	const int insertPct = (100 - findPct) / 2;

	std::vector<SetOp<K>> ops;
	ops.reserve(numOps);
	for (int i = 0; i < numOps; ++i) {
		const int roll = static_cast<int>(rng.nextBelow(100));
		if (roll < findPct) {
			ops.push_back({OpType::Find, makeKey<K>(static_cast<int>(rng.nextBelow(2 * n)))});
		} else if (roll < findPct + insertPct) {
			ops.push_back({OpType::Insert, makeKey<K>(n + static_cast<int>(rng.nextBelow(2 * n)))});
		} else {
			ops.push_back({OpType::Erase, makeKey<K>(static_cast<int>(rng.nextBelow(n)))});
		}
	}

	return {std::move(set), std::move(ops)};
}

// =============================================================================
// Benchmark runners
//
// numOps is passed explicitly (not read from BENCH_OPS) because each TU
// defines a different BENCH_OPS value (1,000,000 for fixed-N benchmarks,
// 100,000 for scaling benchmarks).
// =============================================================================

template<typename Map>
void benchmarkMapMixed(Catch::Benchmark::Chronometer meter, int n, int numOps, WorkloadKind kind) {
	auto [baseMap, ops] = prepareMapWorkload<Map>(n, numOps, kind);
	std::vector<Map> maps(meter.runs(), baseMap);
	// Do NOT re-reserve for Churn: that would rehash and destroy the tombstones
	// we deliberately accumulated during aging.
	if (USE_RESERVE && kind != WorkloadKind::Churn) {
		for (auto& m : maps) m.reserve(n * 2);
	}
	meter.measure([&](int i) {
		auto& map = maps[i];
		uint64_t checksum = 0;
		for (const auto& op : ops) {
			if (op.type == OpType::Find) {
				auto it = map.find(op.key);
				if (it != map.end())
					checksum += toChecksum(it->second);
			} else if (op.type == OpType::Insert) {
				map[op.key] = op.value;
			} else {
				map.erase(op.key);
			}
		}
		return checksum;
	});
}

template<typename Set>
void benchmarkSetMixed(Catch::Benchmark::Chronometer meter, int n, int numOps, int findPct) {
	auto [baseSet, ops] = prepareSetWorkload<Set>(n, numOps, findPct);
	std::vector<Set> sets(meter.runs(), baseSet);
	if (USE_RESERVE) { for (auto& s : sets) s.reserve(n * 2); }
	meter.measure([&](int i) {
		auto& set = sets[i];
		uint64_t checksum = 0;
		for (const auto& op : ops) {
			if (op.type == OpType::Find) {
				checksum += set.count(op.key);
			} else if (op.type == OpType::Insert) {
				set.insert(op.key);
			} else {
				set.erase(op.key);
			}
		}
		return checksum;
	});
}

template<typename Map>
void benchmarkMapIterate(Catch::Benchmark::Chronometer meter, int n, int numOps) {
	Map map = populateMap<Map>(n);
	meter.measure([&] {
		uint64_t checksum = 0;
		auto it = map.begin();
		for (int j = 0; j < numOps; ++j) {
			if (it == map.end())
				it = map.begin();
			checksum += toChecksum(it->first) + toChecksum(it->second);
			++it;
		}
		return checksum;
	});
}

template<typename Set>
void benchmarkSetIterate(Catch::Benchmark::Chronometer meter, int n, int numOps) {
	Set set = populateSet<Set>(n);
	meter.measure([&] {
		uint64_t checksum = 0;
		auto it = set.begin();
		for (int j = 0; j < numOps; ++j) {
			if (it == set.end())
				it = set.begin();
			checksum += toChecksum(*it);
			++it;
		}
		return checksum;
	});
}

// =============================================================================
// scalingSamples() — non-linear N sweep for the scaling benchmark TU.
//
// Exponential spacing gives denser sampling at low N and sparser at high N.
// Extra points are clustered around each power of 2 (where resizes occur).
// =============================================================================

[[maybe_unused]] static std::vector<int> scalingSamples() {
	std::vector<int> samples;
	for (double x = std::log2(100.0); x <= std::log2(2000.0); x += 0.2)
		samples.push_back(static_cast<int>(std::round(std::pow(2.0, x))));
	for (double x = std::log2(2000.0); x <= std::log2(20000.0); x += 0.05)
		samples.push_back(static_cast<int>(std::round(std::pow(2.0, x))));
	for (int p = 2048; p <= 16384; p *= 2) {
		for (int v : {p * 3 / 4, p * 7 / 8, p - 1, p, p + 1, p * 9 / 8, p * 3 / 2}) {
			if (v >= 100 && v <= 20000)
				samples.push_back(v);
		}
	}
	std::sort(samples.begin(), samples.end());
	samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
	return samples;
}

// Pad N to 5 characters for aligned output (e.g. "  100", "20000")
[[maybe_unused]] static std::string padN(int n) {
	auto s = std::to_string(n);
	return std::string(5 - std::min<size_t>(5, s.size()), ' ') + s;
}
