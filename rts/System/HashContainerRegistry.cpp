/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifdef SPRING_HASH_INSTRUMENTATION

#include "HashContainerRegistry.h"
#include "HashContainerStats.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/StringHash.h"
#include "System/TimeProfiler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

// Extract short type from demangled name like "emilib::HashMap<K, V, H, C>" -> "map<K, V>"
// or "emilib::HashSet<K, H, C>" -> "set<K>"
static void FormatShortTypeName(char* buf, size_t bufSize, const char* fullName)
{
	if (fullName == nullptr) {
		snprintf(buf, bufSize, "?");
		return;
	}

	const char* kind = "?";
	const char* params = nullptr;
	int numKeepParams = 0; // how many template params to keep

	if (const char* p = strstr(fullName, "HashMap<")) {
		kind = strstr(fullName, "synced_hash") ? "synced map" : "unsynced map";
		params = p + 8; // skip "HashMap<"
		numKeepParams = 2; // K, V
	} else if (const char* p2 = strstr(fullName, "HashSet<")) {
		kind = strstr(fullName, "synced_hash") ? "synced set" : "unsynced set";
		params = p2 + 8; // skip "HashSet<"
		numKeepParams = 1; // K
	}

	if (params == nullptr) {
		snprintf(buf, bufSize, "%.30s", fullName);
		return;
	}

	// scan params, counting commas at depth 0 to find the boundary
	int depth = 0;
	int commasSeen = 0;
	const char* end = params;
	for (; *end; ++end) {
		if (*end == '<') depth++;
		else if (*end == '>') {
			if (depth == 0) break;
			depth--;
		} else if (*end == ',' && depth == 0) {
			commasSeen++;
			if (commasSeen >= numKeepParams) break;
		}
	}

	// build "map<K, V>" or "set<K>" in a large work buffer first
	char work[512];
	int paramLen = static_cast<int>(end - params);
	snprintf(work, sizeof(work), "%s<%.*s>", kind, paramLen, params);

	// shorten verbose stdlib types before copying to (possibly small) output buf
	static const struct { const char* from; const char* to; } replacements[] = {
		{"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
		{"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>",  "std::string"},
		{"std::__cxx11::basic_string<char>", "std::string"},
	};
	for (const auto& r : replacements) {
		char tmp[512];
		char* pos;
		while ((pos = strstr(work, r.from)) != nullptr) {
			size_t beforeLen = pos - work;
			size_t fromLen = strlen(r.from);
			snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)beforeLen, work, r.to, pos + fromLen);
			snprintf(work, sizeof(work), "%s", tmp);
		}
	}

	snprintf(buf, bufSize, "%s", work);
}

HashContainerRegistry& HashContainerRegistry::GetInstance()
{
	static HashContainerRegistry instance;
	return instance;
}

void HashContainerRegistry::Register(HashContainerStats* stats)
{
	std::lock_guard<std::mutex> lock(registryMutex);
	containers.push_back(stats);
}

void HashContainerRegistry::Unregister(HashContainerStats* stats)
{
	std::lock_guard<std::mutex> lock(registryMutex);
	auto it = std::find(containers.begin(), containers.end(), stats);
	if (it != containers.end()) {
		// swap-and-pop for O(1) removal
		*it = containers.back();
		containers.pop_back();
	}
}

void HashContainerRegistry::PrintAllStats() const
{
	std::lock_guard<std::mutex> lock(registryMutex);

	// sort by bucket count (memory footprint) descending
	std::vector<const HashContainerStats*> sorted(containers.begin(), containers.end());
	std::sort(sorted.begin(), sorted.end(), [](const HashContainerStats* a, const HashContainerStats* b) {
		return a->numBuckets > b->numBuckets;
	});

	size_t numActive = 0;
	size_t numSkipped = 0;

	LOG("[HashContainerStats] %zu instrumented containers", containers.size());
	LOG("%-65s | %-50s | %7s | %7s | %5s | %5s | %12s | %12s | %8s | %6s | %6s | %4s | %4s | probes",
		"type", "source", "size", "buckets", "load", "tomb", "find-hit", "find-miss", "inserts", "erases", "iters", "rh", "maxP");
	LOG("%142s 1|2|3|4-7|8-15|16+  \xe2\x96\x91=1-10%% \xe2\x96\x92=10-30%% \xe2\x96\x93=30-55%% \xe2\x96\x88=55%%+", "");

	for (const auto* stats : sorted) {
		const uint64_t totalOps = stats->findHits + stats->findMisses + stats->inserts + stats->erases;
		if (totalOps < 100) {
			numSkipped++;
			continue;
		}
		numActive++;

		const float loadPct = (stats->numBuckets > 0) ? (100.0f * stats->numFilled / stats->numBuckets) : 0.0f;
		const float tombPct = (stats->numBuckets > 0) ? (100.0f * stats->numTombstones / stats->numBuckets) : 0.0f;

		char srcBuf[80];
		if (stats->sourceFile != nullptr) {
			const char* file = stats->sourceFile;
#ifdef SPRING_SOURCE_DIR
			constexpr const char* srcDir = SPRING_SOURCE_DIR;
			const size_t srcDirLen = strlen(srcDir);
			if (strncmp(file, srcDir, srcDirLen) == 0)
				file += srcDirLen;
#endif
			// detect stdlib paths (e.g. std::array aggregate init)
			if (strstr(file, "/include/c++/") != nullptr || strstr(file, "/include/c/") != nullptr)
				snprintf(srcBuf, sizeof(srcBuf), "<std-aggregate>:%d", stats->sourceLine);
			else
				snprintf(srcBuf, sizeof(srcBuf), "%s:%d", file, stats->sourceLine);
		} else {
			snprintf(srcBuf, sizeof(srcBuf), "<unknown>");
		}

		char typeBuf[80];
		FormatShortTypeName(typeBuf, sizeof(typeBuf), stats->typeName);

		// sparkline probe histogram [1|2|3|4-7|8-15|16+]
		// uses Unicode block chars for intensity: ░ ▒ ▓ █
		char probeBuf[32] = " n/a ";
		if (stats->findHits > 0) {
			// UTF-8 block elements: ░=\xe2\x96\x91 ▒=\xe2\x96\x92 ▓=\xe2\x96\x93 █=\xe2\x96\x88
			static const char* blocks[] = {" ", "\xe2\x96\x91", "\xe2\x96\x92", "\xe2\x96\x93", "\xe2\x96\x88"};
			const double total = static_cast<double>(stats->findHits);
			char* p = probeBuf;
			for (int i = 0; i < 6; ++i) {
				const double pct = 100.0 * stats->probeFindHit[i] / total;
				int level;
				if      (pct < 1.0)  level = 0;  // space
				else if (pct < 10.0) level = 1;  // ░
				else if (pct < 30.0) level = 2;  // ▒
				else if (pct < 55.0) level = 3;  // ▓
				else                 level = 4;  // █
				const char* blk = blocks[level];
				while (*blk) *p++ = *blk++;
			}
			*p = '\0';
		}

		LOG("%-65s | %-50s | %7zu | %7zu | %4.1f%% | %4.1f%% | %12llu | %12llu | %8llu | %6llu | %6llu | %4llu | %4d | %s",
			typeBuf,
			srcBuf,
			stats->numFilled,
			stats->numBuckets,
			loadPct,
			tombPct,
			(unsigned long long)stats->findHits,
			(unsigned long long)stats->findMisses,
			(unsigned long long)stats->inserts,
			(unsigned long long)stats->erases,
			(unsigned long long)stats->iterations,
			(unsigned long long)stats->rehashes,
			stats->peakMaxProbeLength,
			probeBuf
		);
	}

	LOG("[HashContainerStats] %zu active, %zu skipped (<100 ops)", numActive, numSkipped);

	// top 20 containers by total time, with ns/op breakdown
	{
		std::vector<const HashContainerStats*> byTime(containers.begin(), containers.end());
		std::sort(byTime.begin(), byTime.end(), [](const HashContainerStats* a, const HashContainerStats* b) {
			const int64_t timeA = a->findHitNs + a->findMissNs + a->insertNs + a->eraseNs + a->rehashNs;
			const int64_t timeB = b->findHitNs + b->findMissNs + b->insertNs + b->eraseNs + b->rehashNs;
			return timeA > timeB;
		});

		LOG("");
		LOG("[HashContainerStats] Top 20 containers by total time (ns/op):");
		LOG("  %-50s | %-40s | %9s | %10s | %10s | %10s | %10s | %10s",
			"type", "source", "total-ms", "find-hit", "find-miss", "insert", "erase", "rehash");

		const size_t limit = std::min(byTime.size(), size_t{20});
		for (size_t i = 0; i < limit; ++i) {
			const auto* s = byTime[i];
			const int64_t totalNs = s->findHitNs + s->findMissNs + s->insertNs + s->eraseNs + s->rehashNs;
			if (totalNs == 0) break;

			char typeBuf[80];
			FormatShortTypeName(typeBuf, sizeof(typeBuf), s->typeName);

			char srcBuf[80];
			if (s->sourceFile != nullptr) {
				const char* file = s->sourceFile;
#ifdef SPRING_SOURCE_DIR
				constexpr const char* srcDir = SPRING_SOURCE_DIR;
				const size_t srcDirLen = strlen(srcDir);
				if (strncmp(file, srcDir, srcDirLen) == 0)
					file += srcDirLen;
#endif
				if (strstr(file, "/include/c++/") != nullptr || strstr(file, "/include/c/") != nullptr)
					snprintf(srcBuf, sizeof(srcBuf), "<std-aggregate>:%d", s->sourceLine);
				else
					snprintf(srcBuf, sizeof(srcBuf), "%s:%d", file, s->sourceLine);
			} else {
				snprintf(srcBuf, sizeof(srcBuf), "<unknown>");
			}

			char fhBuf[16], fmBuf[16], inBuf[16], erBuf[16], rhBuf[16];
			if (s->findHits   > 0) snprintf(fhBuf, sizeof(fhBuf), "%llu", (unsigned long long)(s->findHitNs  / s->findHits));   else snprintf(fhBuf, sizeof(fhBuf), "-");
			if (s->findMisses > 0) snprintf(fmBuf, sizeof(fmBuf), "%llu", (unsigned long long)(s->findMissNs / s->findMisses)); else snprintf(fmBuf, sizeof(fmBuf), "-");
			if (s->inserts    > 0) snprintf(inBuf, sizeof(inBuf), "%llu", (unsigned long long)(s->insertNs   / s->inserts));    else snprintf(inBuf, sizeof(inBuf), "-");
			if (s->erases     > 0) snprintf(erBuf, sizeof(erBuf), "%llu", (unsigned long long)(s->eraseNs    / s->erases));     else snprintf(erBuf, sizeof(erBuf), "-");
			if (s->rehashes   > 0) snprintf(rhBuf, sizeof(rhBuf), "%llu", (unsigned long long)(s->rehashNs   / s->rehashes));   else snprintf(rhBuf, sizeof(rhBuf), "-");

			LOG("  %-50s | %-40s | %8.2fms | %10s | %10s | %10s | %10s | %10s",
				typeBuf, srcBuf, totalNs / 1e6,
				fhBuf, fmBuf, inBuf, erBuf, rhBuf);
		}
	}

	// aggregated stats grouped by type
	{
		struct TypeAgg {
			size_t count = 0;
			size_t totalFilled = 0;
			size_t totalBuckets = 0;
			size_t totalTombstones = 0;
			uint64_t findHits = 0, findMisses = 0;
			uint64_t inserts = 0, erases = 0, rehashes = 0, iterations = 0;
			int64_t findHitNs = 0, findMissNs = 0;
			int64_t insertNs = 0, eraseNs = 0, rehashNs = 0;
			int peakMaxProbeLength = 0;
		};

		std::map<const char*, TypeAgg> byType;
		for (const auto* stats : containers) {
			auto& agg = byType[stats->typeName];
			agg.count++;
			agg.totalFilled += stats->numFilled;
			agg.totalBuckets += stats->numBuckets;
			agg.totalTombstones += stats->numTombstones;
			agg.findHits += stats->findHits;
			agg.findMisses += stats->findMisses;
			agg.inserts += stats->inserts;
			agg.erases += stats->erases;
			agg.rehashes += stats->rehashes;
			agg.iterations += stats->iterations;
			agg.findHitNs += stats->findHitNs;
			agg.findMissNs += stats->findMissNs;
			agg.insertNs += stats->insertNs;
			agg.eraseNs += stats->eraseNs;
			agg.rehashNs += stats->rehashNs;
			agg.peakMaxProbeLength = std::max(agg.peakMaxProbeLength, stats->peakMaxProbeLength);
		}

		// sort by total ops descending
		std::vector<std::pair<const char*, const TypeAgg*>> sortedTypes;
		for (const auto& [key, agg] : byType) {
			sortedTypes.emplace_back(key, &agg);
		}
		std::sort(sortedTypes.begin(), sortedTypes.end(), [](const auto& a, const auto& b) {
			const uint64_t opsA = a.second->findHits + a.second->findMisses + a.second->inserts + a.second->erases + a.second->iterations;
			const uint64_t opsB = b.second->findHits + b.second->findMisses + b.second->inserts + b.second->erases + b.second->iterations;
			return opsA > opsB;
		});

		LOG("");
		LOG("[HashContainerStats] Aggregated by type (%zu types):", sortedTypes.size());
		LOG("  %-65s | %5s | %8s | %12s | %12s | %10s | %8s | %6s | %9s | %9s",
			"type", "count", "size", "find-hit", "find-miss", "inserts", "erases", "iters", "find-ms", "insert-ms");

		for (const auto& [key, agg] : sortedTypes) {
			const uint64_t totalOps = agg->findHits + agg->findMisses + agg->inserts + agg->erases + agg->iterations;
			if (totalOps < 100) continue;

			char typeBuf[80];
			FormatShortTypeName(typeBuf, sizeof(typeBuf), key);

			LOG("  %-65s | %5zu | %8zu | %12llu | %12llu | %10llu | %8llu | %6llu | %8.1fms | %8.1fms",
				typeBuf,
				agg->count,
				agg->totalFilled,
				(unsigned long long)agg->findHits,
				(unsigned long long)agg->findMisses,
				(unsigned long long)agg->inserts,
				(unsigned long long)agg->erases,
				(unsigned long long)agg->iterations,
				(agg->findHitNs + agg->findMissNs) / 1e6,
				agg->insertNs / 1e6
			);
		}
	}

	// print aggregate timing summary
	int64_t totalFindHitNs = 0, totalFindMissNs = 0;
	int64_t totalInsertNs = 0, totalEraseNs = 0, totalRehashNs = 0;
	uint64_t totalFindHits = 0, totalFindMisses = 0;
	uint64_t totalInserts = 0, totalErases = 0, totalRehashes = 0;
	uint64_t totalIterations = 0;

	for (const auto* stats : containers) {
		totalFindHitNs += stats->findHitNs;
		totalFindMissNs += stats->findMissNs;
		totalInsertNs += stats->insertNs;
		totalEraseNs += stats->eraseNs;
		totalRehashNs += stats->rehashNs;
		totalFindHits += stats->findHits;
		totalFindMisses += stats->findMisses;
		totalInserts += stats->inserts;
		totalErases += stats->erases;
		totalRehashes += stats->rehashes;
		totalIterations += stats->iterations;
	}

	LOG("[HashContainerStats] Aggregate timing:");
	LOG("  find_hit:  %llu ops, %.3f ms total (%.0f ns/op avg)",
		(unsigned long long)totalFindHits, totalFindHitNs / 1e6,
		totalFindHits > 0 ? (double)totalFindHitNs / totalFindHits : 0.0);
	LOG("  find_miss: %llu ops, %.3f ms total (%.0f ns/op avg)",
		(unsigned long long)totalFindMisses, totalFindMissNs / 1e6,
		totalFindMisses > 0 ? (double)totalFindMissNs / totalFindMisses : 0.0);
	LOG("  insert:    %llu ops, %.3f ms total (%.0f ns/op avg)",
		(unsigned long long)totalInserts, totalInsertNs / 1e6,
		totalInserts > 0 ? (double)totalInsertNs / totalInserts : 0.0);
	LOG("  erase:     %llu ops, %.3f ms total (%.0f ns/op avg)",
		(unsigned long long)totalErases, totalEraseNs / 1e6,
		totalErases > 0 ? (double)totalEraseNs / totalErases : 0.0);
	LOG("  rehash:    %llu ops, %.3f ms total (%.0f ns/op avg)",
		(unsigned long long)totalRehashes, totalRehashNs / 1e6,
		totalRehashes > 0 ? (double)totalRehashNs / totalRehashes : 0.0);
	LOG("  iterate:   %llu begin() calls",
		(unsigned long long)totalIterations);
}

void HashContainerRegistry::ResetAllStats()
{
	std::lock_guard<std::mutex> lock(registryMutex);
	for (auto* stats : containers) {
		stats->Reset();
	}
	prevFindNs = 0;
	prevInsertNs = 0;
	prevEraseNs = 0;
	prevRehashNs = 0;
}

void HashContainerRegistry::HarvestForTimeProfiler()
{
	std::lock_guard<std::mutex> lock(registryMutex);

	int64_t totalFindNs = 0;
	int64_t totalInsertNs = 0;
	int64_t totalEraseNs = 0;
	int64_t totalRehashNs = 0;

	for (const auto* stats : containers) {
		totalFindNs += stats->findHitNs + stats->findMissNs;
		totalInsertNs += stats->insertNs;
		totalEraseNs += stats->eraseNs;
		totalRehashNs += stats->rehashNs;
	}

	auto& profiler = CTimeProfiler::GetInstance();

	auto feedTimer = [&](const char* name, int64_t totalNs, int64_t& prevNs) {
		const int64_t deltaNs = totalNs - prevNs;
		prevNs = totalNs;
		if (deltaNs > 0) {
			const spring_time deltaTime = spring_time::fromNanoSecs(deltaNs);
			profiler.AddTime(hashString(name), spring_gettime() - deltaTime, deltaTime, false, true, false);
		}
	};

	feedTimer("HashMap::find",   totalFindNs,   prevFindNs);
	feedTimer("HashMap::insert", totalInsertNs,  prevInsertNs);
	feedTimer("HashMap::erase",  totalEraseNs,   prevEraseNs);
	feedTimer("HashMap::rehash", totalRehashNs,  prevRehashNs);
}

#endif // SPRING_HASH_INSTRUMENTATION
