/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <algorithm>
#include <cstdint>

struct HashContainerStats {
	const char* sourceFile = nullptr;
	int sourceLine = 0;
	const char* typeName = nullptr;

	// operation counts
	uint64_t findHits = 0;
	uint64_t findMisses = 0;
	uint64_t inserts = 0;
	uint64_t erases = 0;
	uint64_t rehashes = 0;
	uint64_t iterations = 0;

	// aggregate timing (nanoseconds)
	int64_t findHitNs = 0;
	int64_t findMissNs = 0;
	int64_t insertNs = 0;
	int64_t eraseNs = 0;
	int64_t rehashNs = 0;

	// probe length histograms: [1], [2], [3], [4-7], [8-15], [16+]
	static constexpr int NUM_PROBE_BUCKETS = 6;
	uint64_t probeFindHit[NUM_PROBE_BUCKETS] = {};
	uint64_t probeFindMiss[NUM_PROBE_BUCKETS] = {};
	uint64_t probeInsert[NUM_PROBE_BUCKETS] = {};

	int peakMaxProbeLength = 0;

	// live container state (set by container, read by registry during reporting)
	size_t numFilled = 0;
	size_t numBuckets = 0;
	size_t numTombstones = 0;

	static constexpr int ProbeHistBucket(int probeLen)
	{
		if (probeLen <= 0) return 0;
		if (probeLen == 1) return 1;
		if (probeLen == 2) return 2;
		if (probeLen <= 6) return 3;
		if (probeLen <= 14) return 4;
		return 5;
	}

	void Reset()
	{
		findHits = 0;
		findMisses = 0;
		inserts = 0;
		erases = 0;
		rehashes = 0;
		iterations = 0;

		findHitNs = 0;
		findMissNs = 0;
		insertNs = 0;
		eraseNs = 0;
		rehashNs = 0;

		for (int i = 0; i < NUM_PROBE_BUCKETS; ++i) {
			probeFindHit[i] = 0;
			probeFindMiss[i] = 0;
			probeInsert[i] = 0;
		}

		peakMaxProbeLength = 0;
	}
};
