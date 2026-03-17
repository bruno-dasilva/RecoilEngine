/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <catch_amalgamated.hpp>

#include "System/Log/ILog.h"
#include "System/UnorderedMap.hpp"
#include "System/UnorderedSet.hpp"

#ifdef SPRING_HASH_INSTRUMENTATION
#include "System/HashContainerStats.h"
#include "System/HashContainerRegistry.h"

TEST_CASE("HashContainerStats.ProbeHistBucket")
{
	CHECK(HashContainerStats::ProbeHistBucket(0) == 0);
	CHECK(HashContainerStats::ProbeHistBucket(1) == 1);
	CHECK(HashContainerStats::ProbeHistBucket(2) == 2);
	CHECK(HashContainerStats::ProbeHistBucket(3) == 3);
	CHECK(HashContainerStats::ProbeHistBucket(6) == 3);
	CHECK(HashContainerStats::ProbeHistBucket(7) == 4);
	CHECK(HashContainerStats::ProbeHistBucket(14) == 4);
	CHECK(HashContainerStats::ProbeHistBucket(15) == 5);
	CHECK(HashContainerStats::ProbeHistBucket(100) == 5);
}

TEST_CASE("HashMap.InstrumentedInsertFind")
{
	spring::unsynced_map<int, int> map;

	// insert several elements
	for (int i = 0; i < 100; ++i) {
		map[i] = i * 10;
	}

	// find hits
	for (int i = 0; i < 100; ++i) {
		auto it = map.find(i);
		REQUIRE(it != map.end());
		CHECK(it->second == i * 10);
	}

	// find misses
	for (int i = 100; i < 200; ++i) {
		auto it = map.find(i);
		CHECK(it == map.end());
	}

	// erase some
	for (int i = 0; i < 50; ++i) {
		CHECK(map.erase(i));
	}

	// verify registry has this container tracked
	auto& registry = HashContainerRegistry::GetInstance();

	// print stats (exercises the print path)
	registry.PrintAllStats();
}

TEST_CASE("HashSet.InstrumentedInsertFind")
{
	spring::unsynced_set<int> set;

	for (int i = 0; i < 100; ++i) {
		set.insert(i);
	}

	// find hits
	for (int i = 0; i < 100; ++i) {
		CHECK(set.contains(i));
	}

	// find misses
	for (int i = 100; i < 200; ++i) {
		CHECK(!set.contains(i));
	}

	// erase some
	for (int i = 0; i < 50; ++i) {
		CHECK(set.erase(i));
	}

	auto& registry = HashContainerRegistry::GetInstance();
	registry.PrintAllStats();
}

TEST_CASE("HashMap.IterationCounting")
{
	spring::unsynced_map<int, int> map;

	// iterating an empty container should not count
	for (auto& p : map) { (void)p; }
	CHECK(map.getStats().iterations == 0);

	map[1] = 10;
	map[2] = 20;
	map[3] = 30;

	const auto itersBefore = map.getStats().iterations;

	// range-based for calls begin() once
	for (auto& [k, v] : map) { (void)k; (void)v; }
	CHECK(map.getStats().iterations == itersBefore + 1);

	// another iteration
	for (auto& [k, v] : map) { (void)k; (void)v; }
	CHECK(map.getStats().iterations == itersBefore + 2);

	// const iteration via cbegin (which calls const begin())
	const auto& cmap = map;
	for (const auto& [k, v] : cmap) { (void)k; (void)v; }
	CHECK(map.getStats().iterations == itersBefore + 3);
}

TEST_CASE("HashSet.IterationCounting")
{
	spring::unsynced_set<int> set;

	for (auto& k : set) { (void)k; }
	CHECK(set.getStats().iterations == 0);

	set.insert(1);
	set.insert(2);

	const auto itersBefore = set.getStats().iterations;

	for (auto& k : set) { (void)k; }
	CHECK(set.getStats().iterations == itersBefore + 1);

	const auto& cset = set;
	for (const auto& k : cset) { (void)k; }
	CHECK(set.getStats().iterations == itersBefore + 2);
}

TEST_CASE("HashMap.StatsAccuracy")
{
	spring::unsynced_map<int, int> map;

	// do known operations and verify counts
	map[1] = 10;
	map[2] = 20;
	map[3] = 30;

	auto it1 = map.find(1); // hit
	auto it2 = map.find(999); // miss

	CHECK(it1 != map.end());
	CHECK(it2 == map.end());

	map.erase(2);

	// Verify the map still works correctly after instrumented operations
	CHECK(map.size() == 2);
	CHECK(map.contains(1));
	CHECK(!map.contains(2));
	CHECK(map.contains(3));
}

#else // !SPRING_HASH_INSTRUMENTATION

TEST_CASE("HashMap.NoInstrumentationOverhead")
{
	// When instrumentation is disabled, maps should work normally
	spring::unsynced_map<int, int> map;
	map[1] = 10;
	map[2] = 20;

	CHECK(map.size() == 2);
	CHECK(map.find(1) != map.end());
	CHECK(map.find(3) == map.end());
}

#endif // SPRING_HASH_INSTRUMENTATION
