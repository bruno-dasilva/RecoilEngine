/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#ifdef SPRING_HASH_INSTRUMENTATION

#include <mutex>
#include <vector>

struct HashContainerStats;

class HashContainerRegistry {
public:
	static HashContainerRegistry& GetInstance();

	void Register(HashContainerStats* stats);
	void Unregister(HashContainerStats* stats);

	void PrintAllStats() const;
	void ResetAllStats();
	void HarvestForTimeProfiler();

private:
	HashContainerRegistry() = default;

	mutable std::mutex registryMutex;
	std::vector<HashContainerStats*> containers;

	// delta tracking for TimeProfiler integration
	int64_t prevFindNs = 0;
	int64_t prevInsertNs = 0;
	int64_t prevEraseNs = 0;
	int64_t prevRehashNs = 0;
};

#endif // SPRING_HASH_INSTRUMENTATION
