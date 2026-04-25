/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <functional>
#include <unordered_map>
#include <utility>

#include "SpringHash.h"

#ifdef SPRING_HASH_INSTRUMENTATION
#include <chrono>
#include <cxxabi.h>
#include <source_location>
#include <typeinfo>
#include "HashContainerRegistry.h"
#include "HashContainerStats.h"
#ifndef HASH_INSTR_GETTIME_DEFINED
#define HASH_INSTR_GETTIME_DEFINED
inline int64_t hashInstrGetNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}
#endif
#define CHAINED_HASHMAP_INSTR(...) __VA_ARGS__
#else
#define CHAINED_HASHMAP_INSTR(...)
#endif

namespace spring {

// Instrumented wrapper over std::unordered_map (chained bucket layout).
// Mirrors the emilib::HashMap instrumentation surface so containers show up
// in the same [HashContainerStats] report and get consistent timing numbers
// for apples-to-apples comparison against spring::unordered_map.
//
// NOTE: probe histograms are left empty — chained maps don't have a probe
// distance the way open-addressing does. The `load` and `rehashes` columns
// are the relevant structural signal here.
template <typename KeyT, typename ValueT, typename HashT = spring::synced_hash<KeyT>, typename EqT = std::equal_to<KeyT>>
class chained_hashmap {
public:
	using inner_type      = std::unordered_map<KeyT, ValueT, HashT, EqT>;
	using key_type        = KeyT;
	using mapped_type     = ValueT;
	using value_type      = typename inner_type::value_type;
	using size_type       = typename inner_type::size_type;
	using iterator        = typename inner_type::iterator;
	using const_iterator  = typename inner_type::const_iterator;
	using hasher          = HashT;
	using key_equal       = EqT;

#ifdef SPRING_HASH_INSTRUMENTATION
private:
	static const char* typeNameStr()
	{
		static const char* name = [] {
			int status = 0;
			char* demangled = abi::__cxa_demangle(typeid(chained_hashmap).name(), nullptr, nullptr, &status);
			return (status == 0 && demangled) ? demangled : typeid(chained_hashmap).name();
		}();
		return name;
	}

	void initStats(std::source_location loc)
	{
		_stats.sourceFile = loc.file_name();
		_stats.sourceLine = loc.line();
		_stats.typeName = typeNameStr();
		// Rough per-bucket cost: bucket-array pointer + (node: next-ptr + value + cached hash).
		// Only approximate; filled-node cost dominates for load factors near 1.0.
		_stats.bucketByteSize = sizeof(void*) + sizeof(value_type) + sizeof(void*) + sizeof(std::size_t);
		HashContainerRegistry::GetInstance().Register(&_stats);
		updateLiveStats();
	}

	void updateLiveStats() const
	{
		_stats.numFilled = _map.size();
		_stats.numBuckets = _map.bucket_count();
		_stats.numTombstones = 0;
	}
public:
	chained_hashmap(std::source_location loc = std::source_location::current())
	{
		initStats(loc);
	}

	explicit chained_hashmap(size_type n, std::source_location loc = std::source_location::current())
		: _map(n)
	{
		initStats(loc);
	}

	chained_hashmap(const chained_hashmap& other)
		: _map(other._map)
	{
		_stats.sourceFile = other._stats.sourceFile;
		_stats.sourceLine = other._stats.sourceLine;
		_stats.typeName = typeNameStr();
		_stats.bucketByteSize = other._stats.bucketByteSize;
		HashContainerRegistry::GetInstance().Register(&_stats);
		updateLiveStats();
	}

	chained_hashmap(chained_hashmap&& other) noexcept
		: _map(std::move(other._map))
	{
		_stats.sourceFile = other._stats.sourceFile;
		_stats.sourceLine = other._stats.sourceLine;
		_stats.typeName = typeNameStr();
		_stats.bucketByteSize = other._stats.bucketByteSize;
		HashContainerRegistry::GetInstance().Register(&_stats);
		updateLiveStats();
	}

	~chained_hashmap()
	{
		HashContainerRegistry::GetInstance().Unregister(&_stats);
	}
#else
	chained_hashmap() = default;
	explicit chained_hashmap(size_type n) : _map(n) {}
	chained_hashmap(const chained_hashmap&) = default;
	chained_hashmap(chained_hashmap&&) noexcept = default;
	~chained_hashmap() = default;
#endif

	chained_hashmap& operator=(const chained_hashmap& other)
	{
		if (this != &other) {
			_map = other._map;
			CHAINED_HASHMAP_INSTR(updateLiveStats());
		}
		return *this;
	}

	chained_hashmap& operator=(chained_hashmap&& other) noexcept
	{
		_map = std::move(other._map);
		CHAINED_HASHMAP_INSTR(updateLiveStats());
		return *this;
	}

	// ---- capacity ----
	size_type size()  const { return _map.size();  }
	bool      empty() const { return _map.empty(); }

	// ---- iterators ----
	iterator       begin()        { CHAINED_HASHMAP_INSTR(if (!_map.empty()) _stats.iterations++); return _map.begin(); }
	const_iterator begin()  const { CHAINED_HASHMAP_INSTR(if (!_map.empty()) _stats.iterations++); return _map.begin(); }
	const_iterator cbegin() const { CHAINED_HASHMAP_INSTR(if (!_map.empty()) _stats.iterations++); return _map.cbegin(); }
	iterator       end()          { return _map.end();  }
	const_iterator end()    const { return _map.end();  }
	const_iterator cend()   const { return _map.cend(); }

	// ---- lookup ----
	iterator find(const KeyT& key)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto it = _map.find(key);
		CHAINED_HASHMAP_INSTR(recordFind(it != _map.end(), t0));
		return it;
	}

	const_iterator find(const KeyT& key) const
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto it = _map.find(key);
		CHAINED_HASHMAP_INSTR(recordFind(it != _map.end(), t0));
		return it;
	}

	bool      contains(const KeyT& k) const { return find(k) != end(); }
	size_type count   (const KeyT& k) const { return find(k) != end() ? 1 : 0; }

	// ---- modifiers ----
	ValueT& operator[](const KeyT& key)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevSize = _map.size());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		auto& ref = _map[key];
		CHAINED_HASHMAP_INSTR(
			if (_map.size() != prevSize) {
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				if (_map.bucket_count() != prevBuckets)
					_stats.rehashes++;
				updateLiveStats();
			}
		);
		return ref;
	}

	std::pair<iterator, bool> insert(const value_type& v)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		auto res = _map.insert(v);
		CHAINED_HASHMAP_INSTR(
			if (res.second) {
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				if (_map.bucket_count() != prevBuckets)
					_stats.rehashes++;
				updateLiveStats();
			}
		);
		return res;
	}

	std::pair<iterator, bool> insert(value_type&& v)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		auto res = _map.insert(std::move(v));
		CHAINED_HASHMAP_INSTR(
			if (res.second) {
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				if (_map.bucket_count() != prevBuckets)
					_stats.rehashes++;
				updateLiveStats();
			}
		);
		return res;
	}

	template <typename... Args>
	std::pair<iterator, bool> emplace(Args&&... args)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		auto res = _map.emplace(std::forward<Args>(args)...);
		CHAINED_HASHMAP_INSTR(
			if (res.second) {
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				if (_map.bucket_count() != prevBuckets)
					_stats.rehashes++;
				updateLiveStats();
			}
		);
		return res;
	}

	size_type erase(const KeyT& key)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto n = _map.erase(key);
		CHAINED_HASHMAP_INSTR(
			if (n > 0) {
				_stats.erases++;
				_stats.eraseNs += hashInstrGetNs() - t0;
				updateLiveStats();
			}
		);
		return n;
	}

	iterator erase(const_iterator pos)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto it = _map.erase(pos);
		CHAINED_HASHMAP_INSTR(
			_stats.erases++;
			_stats.eraseNs += hashInstrGetNs() - t0;
			updateLiveStats();
		);
		return it;
	}

	iterator erase(iterator pos)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto it = _map.erase(pos);
		CHAINED_HASHMAP_INSTR(
			_stats.erases++;
			_stats.eraseNs += hashInstrGetNs() - t0;
			updateLiveStats();
		);
		return it;
	}

	void clear()
	{
		_map.clear();
		CHAINED_HASHMAP_INSTR(updateLiveStats());
	}

	void reserve(size_type n)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		_map.reserve(n);
		CHAINED_HASHMAP_INSTR(
			if (_map.bucket_count() != prevBuckets) {
				_stats.rehashes++;
				_stats.rehashNs += hashInstrGetNs() - t0;
			}
			updateLiveStats();
		);
	}

	void rehash(size_type n)
	{
		CHAINED_HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		CHAINED_HASHMAP_INSTR(const size_t prevBuckets = _map.bucket_count());
		_map.rehash(n);
		CHAINED_HASHMAP_INSTR(
			if (_map.bucket_count() != prevBuckets) {
				_stats.rehashes++;
				_stats.rehashNs += hashInstrGetNs() - t0;
			}
			updateLiveStats();
		);
	}

	void swap(chained_hashmap& other) noexcept
	{
		_map.swap(other._map);
		// _stats stay with the container (as emilib does).
		CHAINED_HASHMAP_INSTR(updateLiveStats());
		CHAINED_HASHMAP_INSTR(other.updateLiveStats());
	}

#ifdef SPRING_HASH_INSTRUMENTATION
	void setStatsSource(const char* file, int line) { _stats.sourceFile = file; _stats.sourceLine = line; }
	const HashContainerStats& getStats() const { return _stats; }
#endif

private:
#ifdef SPRING_HASH_INSTRUMENTATION
	void recordFind(bool hit, int64_t t0) const
	{
		if (hit) {
			_stats.findHits++;
			_stats.findHitNs += hashInstrGetNs() - t0;
		} else {
			_stats.findMisses++;
			_stats.findMissNs += hashInstrGetNs() - t0;
		}
	}
#endif

	inner_type _map;
#ifdef SPRING_HASH_INSTRUMENTATION
	mutable HashContainerStats _stats;
#endif
};

} // namespace spring
