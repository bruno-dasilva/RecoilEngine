// By Emil Ernerfeldt 2014-2016
// LICENSE:
//   This software is dual-licensed to the public domain and under the following
//   license: you are granted a perpetual, irrevocable license to copy, modify,
//   publish, and distribute this file as you see fit.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <utility>

#ifdef SPRING_HASH_INSTRUMENTATION
#include <algorithm>
#include <cxxabi.h>
#include <source_location>
#include <typeinfo>
#include "HashContainerStats.h"
#include "HashContainerRegistry.h"
#include <chrono>
#ifndef HASH_INSTR_GETTIME_DEFINED
#define HASH_INSTR_GETTIME_DEFINED
// bypass spring timing system entirely — safe during static initialization
inline int64_t hashInstrGetNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}
#endif
#define HASHMAP_INSTR(...) __VA_ARGS__
#else
#define HASHMAP_INSTR(...)
#endif

#define DCHECK_EQ_F(a, b)
#define DCHECK_LT_F(a, b)
#define DCHECK_NE_F(a, b)

namespace emilib {

// like std::equal_to but no need to #include <functional>
template<typename T>
struct HashMapEqualTo
{
	constexpr bool operator()(const T &lhs, const T &rhs) const
	{
		return lhs == rhs;
	}
};

// A cache-friendly hash table with open addressing, linear probing and power-of-two capacity
template <typename KeyT, typename ValueT, typename HashT = std::hash<KeyT>, typename CompT = HashMapEqualTo<KeyT>>
class HashMap
{
public:
	using MyType = HashMap<KeyT, ValueT, HashT, CompT>;

	using PairT = std::pair<KeyT, ValueT>;
public:
	using key_type        = KeyT;
	using mapped_type     = ValueT;
	using size_type       = size_t;
	using value_type      = PairT;
	using reference       = PairT&;
	using const_reference = const PairT&;

	class iterator
	{
	public:
		using iterator_category = std::forward_iterator_tag;
		using difference_type   = size_t;
		using distance_type     = size_t;
		using value_type        = std::pair<KeyT, ValueT>;
		using pointer           = value_type*;
		using reference         = value_type&;

		iterator() { }

		iterator(MyType* hash_map, size_t bucket) : _map(hash_map), _bucket(bucket)
		{
		}

		iterator& operator++()
		{
			this->goto_next_element();
			return *this;
		}

		iterator operator++(int)
		{
			size_t old_index = _bucket;
			this->goto_next_element();
			return iterator(_map, old_index);
		}

		reference operator*() const
		{
			return _map->_pairs[_bucket];
		}

		pointer operator->() const
		{
			return _map->_pairs + _bucket;
		}

		bool operator==(const iterator& rhs) const
		{
			DCHECK_EQ_F(_map, rhs._map);
			return this->_bucket == rhs._bucket;
		}

		bool operator!=(const iterator& rhs) const
		{
			DCHECK_EQ_F(_map, rhs._map);
			return this->_bucket != rhs._bucket;
		}

	private:
		void goto_next_element()
		{
			DCHECK_LT_F(_bucket, _map->_num_buckets);
			do {
				_bucket++;
			} while (_bucket < _map->_num_buckets && _map->_states[_bucket] != State::FILLED);
		}

	//private:
	//	friend class MyType;
	public:
		MyType* _map;
		size_t  _bucket;
	};

	class const_iterator
	{
	public:
		using iterator_category = std::forward_iterator_tag;
		using difference_type   = size_t;
		using distance_type     = size_t;
		using value_type        = const std::pair<KeyT, ValueT>;
		using pointer           = value_type*;
		using reference         = value_type&;

		const_iterator() { }

		const_iterator(iterator proto) : _map(proto._map), _bucket(proto._bucket)
		{
		}

		const_iterator(const MyType* hash_map, size_t bucket) : _map(hash_map), _bucket(bucket)
		{
		}

		const_iterator& operator++()
		{
			this->goto_next_element();
			return *this;
		}

		const_iterator operator++(int)
		{
			size_t old_index = _bucket;
			this->goto_next_element();
			return const_iterator(_map, old_index);
		}

		reference operator*() const
		{
			return _map->_pairs[_bucket];
		}

		pointer operator->() const
		{
			return _map->_pairs + _bucket;
		}

		bool operator==(const const_iterator& rhs) const
		{
			DCHECK_EQ_F(_map, rhs._map);
			return this->_bucket == rhs._bucket;
		}

		bool operator!=(const const_iterator& rhs) const
		{
			DCHECK_EQ_F(_map, rhs._map);
			return this->_bucket != rhs._bucket;
		}

	private:
		void goto_next_element()
		{
			DCHECK_LT_F(_bucket, _map->_num_buckets);
			do {
				_bucket++;
			} while (_bucket < _map->_num_buckets && _map->_states[_bucket] != State::FILLED);
		}

	//private:
	//	friend class MyType;
	public:
		const MyType* _map;
		size_t        _bucket;
	};

	// ------------------------------------------------------------------------

#ifdef SPRING_HASH_INSTRUMENTATION
private:
	static const char* typeNameStr()
	{
		static const char* name = [] {
			int status = 0;
			char* demangled = abi::__cxa_demangle(typeid(MyType).name(), nullptr, nullptr, &status);
			return (status == 0 && demangled) ? demangled : typeid(MyType).name();
		}();
		return name;
	}

	void initStats(std::source_location loc)
	{
		_stats.sourceFile = loc.file_name();
		_stats.sourceLine = loc.line();
		_stats.typeName = typeNameStr();
		HashContainerRegistry::GetInstance().Register(&_stats);
	}

public:
	HashMap(std::source_location loc = std::source_location::current())
	{
		initStats(loc);
	}

	HashMap(size_t num_elems, std::source_location loc = std::source_location::current())
	{
		initStats(loc);
		reserve(num_elems);
	}

	HashMap(const std::initializer_list< std::pair<KeyT, ValueT> >& l, std::source_location loc = std::source_location::current())
	{
		initStats(loc);
		reserve(l.size());
		for (const auto& pair: l) {
			emplace(pair.first, pair.second);
		}
	}

	HashMap(const HashMap& other)
	{
		_stats.sourceFile = other._stats.sourceFile;
		_stats.sourceLine = other._stats.sourceLine;
		_stats.typeName = typeNameStr();
		HashContainerRegistry::GetInstance().Register(&_stats);
		reserve(other.size());
		insert(other.cbegin(), other.cend());
	}

	HashMap(HashMap&& other)
	{
		_stats.sourceFile = other._stats.sourceFile;
		_stats.sourceLine = other._stats.sourceLine;
		_stats.typeName = typeNameStr();
		HashContainerRegistry::GetInstance().Register(&_stats);
		*this = std::move(other);
	}
#else
	HashMap() = default;
	HashMap(size_t num_elems) { reserve(num_elems); }
	HashMap(const std::initializer_list< std::pair<KeyT, ValueT> >& l)
	{
		reserve(l.size());
		for (const auto& pair: l) {
			emplace(pair.first, pair.second);
		}
	}

	HashMap(const HashMap& other)
	{
		reserve(other.size());
		insert(other.cbegin(), other.cend());
	}

	HashMap(HashMap&& other)
	{
		*this = std::move(other);
	}
#endif

	HashMap& operator=(const HashMap& other)
	{
		clear();
		reserve(other.size()); // this should be the internal length not the size()... right?
		insert(other.cbegin(), other.cend());
		return *this;
	}

	void operator=(HashMap&& other) noexcept
	{
		this->swap(other);
	}

	~HashMap()
	{
		for (size_t bucket=0; bucket<_num_buckets; ++bucket) {
			if (_states[bucket] == State::FILLED) {
				_pairs[bucket].~PairT();
			}
		}

		::operator delete(_states);
		::operator delete(_pairs);

		HASHMAP_INSTR(HashContainerRegistry::GetInstance().Unregister(&_stats));
	}

	void swap(HashMap& other)
	{
		std::swap(_hasher,           other._hasher);
		std::swap(_comp,             other._comp);
		std::swap(_states,           other._states);
		std::swap(_pairs,            other._pairs);
		std::swap(_num_buckets,      other._num_buckets);
		std::swap(_num_filled,       other._num_filled);
		std::swap(_max_probe_length, other._max_probe_length);
		std::swap(_mask,             other._mask);
		// NOTE: _stats are NOT swapped — each container keeps its own
		// identity (source location, registry pointer) across swaps.
		HASHMAP_INSTR(
			updateLiveStats();
			other.updateLiveStats();
		);
	}

	// -------------------------------------------------------------

	iterator begin()
	{
		size_t bucket = 0;
		while (bucket<_num_buckets && _states[bucket] != State::FILLED) {
			++bucket;
		}
		HASHMAP_INSTR(if (bucket < _num_buckets) _stats.iterations++);
		return iterator(this, bucket);
	}

	const_iterator cbegin() const { return (begin()); }
	const_iterator begin() const
	{
		size_t bucket = 0;
		while (bucket<_num_buckets && _states[bucket] != State::FILLED) {
			++bucket;
		}
		HASHMAP_INSTR(if (bucket < _num_buckets) _stats.iterations++);
		return const_iterator(this, bucket);
	}

	iterator end()
	{ return iterator(this, _num_buckets); }

	const_iterator cend() const { return (end()); }
	const_iterator end() const
	{ return const_iterator(this, _num_buckets); }

	size_t size() const
	{
		return _num_filled;
	}

	bool empty() const
	{
		return _num_filled==0;
	}

	// ------------------------------------------------------------

	iterator find(const KeyT& key)
	{
		auto bucket = this->find_filled_bucket(key);
		if (bucket == (size_t)-1) {
			return this->end();
		}
		return iterator(this, bucket);
	}

	const_iterator find(const KeyT& key) const
	{
		auto bucket = this->find_filled_bucket(key);
		if (bucket == (size_t)-1)
		{
			return this->end();
		}
		return const_iterator(this, bucket);
	}

	bool contains(const KeyT& k) const
	{
		return find_filled_bucket(k) != (size_t)-1;
	}

	size_t count(const KeyT& k) const
	{
		return find_filled_bucket(k) != (size_t)-1 ? 1 : 0;
	}

	ValueT* try_get(const KeyT& k)
	{
		auto bucket = find_filled_bucket(k);
		if (bucket != (size_t)-1) {
			return &_pairs[bucket].second;
		} else {
			return nullptr;
		}
	}

	const ValueT* try_get(const KeyT& k) const
	{
		auto bucket = find_filled_bucket(k);
		if (bucket != (size_t)-1) {
			return &_pairs[bucket].second;
		} else {
			return nullptr;
		}
	}

	const ValueT get_or_return_default(const KeyT& k) const
	{
		const ValueT* ret = try_get(k);
		if (ret) {
			return *ret;
		} else {
			return ValueT();
		}
	}

	// -----------------------------------------------------

	std::pair<iterator, bool> emplace(const KeyT& key, const ValueT& value)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		check_expand_need();

		auto bucket = find_or_allocate(key);

		if (_states[bucket] == State::FILLED) {
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return { iterator(this, bucket), false };
		} else {
			HASHMAP_INSTR(if (_states[bucket] == State::ACTIVE) _stats.numTombstones--);
			_states[bucket] = State::FILLED;
			new(_pairs + bucket) PairT(key, value);
			_num_filled++;
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return { iterator(this, bucket), true };
		}
	}
	std::pair<iterator, bool> emplace(const KeyT& key, ValueT&& value)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		check_expand_need();

		auto bucket = find_or_allocate(key);

		if (_states[bucket] == State::FILLED) {
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return { iterator(this, bucket), false };
		}
		else {
			HASHMAP_INSTR(if (_states[bucket] == State::ACTIVE) _stats.numTombstones--);
			_states[bucket] = State::FILLED;
			new(_pairs + bucket) PairT(key, std::move(value));
			_num_filled++;
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return { iterator(this, bucket), true };
		}
	}

	std::pair<iterator, bool> insert(const std::pair<KeyT, ValueT>& p)
	{
		return emplace(p.first, p.second);
	}

	void insert(const_iterator begin, const_iterator end)
	{
		for (; begin != end; ++begin) {
			emplace(begin->first, begin->second);
		}
	}

	void insert_unique(KeyT&& key, ValueT&& value)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		assert(!contains(key));
		check_expand_need();
		auto bucket = find_empty_bucket(key);
		HASHMAP_INSTR(if (_states[bucket] == State::ACTIVE) _stats.numTombstones--);
		_states[bucket] = State::FILLED;
		new(_pairs + bucket) PairT(std::move(key), std::move(value));
		_num_filled++;
		HASHMAP_INSTR(
			_stats.inserts++;
			_stats.insertNs += hashInstrGetNs() - t0;
			updateLiveStats();
		);
	}

	void insert_unique(std::pair<KeyT, ValueT>&& p)
	{
		insert_unique(std::move(p.first), std::move(p.second));
	}

	ValueT set_get(const KeyT& key, const ValueT& new_value)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		check_expand_need();

		auto bucket = find_or_allocate(key);

		if (_states[bucket] == State::FILLED) {
			ValueT old_value = _pairs[bucket].second;
			_pairs[bucket] = new_value.second;
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return old_value;
		} else {
			HASHMAP_INSTR(if (_states[bucket] == State::ACTIVE) _stats.numTombstones--);
			_states[bucket] = State::FILLED;
			new(_pairs + bucket) PairT(key, new_value);
			_num_filled++;
			HASHMAP_INSTR(
				_stats.inserts++;
				_stats.insertNs += hashInstrGetNs() - t0;
				updateLiveStats();
			);
			return ValueT();
		}
	}

	ValueT& operator[](const KeyT& key)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		check_expand_need();

		auto bucket = find_or_allocate(key);

		if (_states[bucket] != State::FILLED) {
			HASHMAP_INSTR(if (_states[bucket] == State::ACTIVE) _stats.numTombstones--);
			_states[bucket] = State::FILLED;
			new(_pairs + bucket) PairT(key, ValueT());
			_num_filled++;
		}

		HASHMAP_INSTR(
			_stats.inserts++;
			_stats.insertNs += hashInstrGetNs() - t0;
			updateLiveStats();
		);
		return _pairs[bucket].second;
	}

	// -------------------------------------------------------

	bool erase(const KeyT& key)
	{
		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto bucket = find_filled_bucket(key);
		if (bucket != (size_t)-1) {
			_states[bucket] = State::ACTIVE;
			_pairs[bucket].~PairT();
			_num_filled -= 1;
			HASHMAP_INSTR(
				_stats.erases++;
				_stats.eraseNs += hashInstrGetNs() - t0;
				_stats.numTombstones++;
				updateLiveStats();
			);
			return true;
		} else {
			HASHMAP_INSTR(
				_stats.erases++;
				_stats.eraseNs += hashInstrGetNs() - t0;
			);
			return false;
		}
	}

	iterator erase(iterator it)
	{
		DCHECK_EQ_F(it._map, this);
		DCHECK_LT_F(it._bucket, _num_buckets);
		_states[it._bucket] = State::ACTIVE;
		_pairs[it._bucket].~PairT();
		_num_filled -= 1;
		HASHMAP_INSTR(
			_stats.erases++;
			_stats.numTombstones++;
			updateLiveStats();
		);
		return ++it;
	}

	void clear()
	{
		for (size_t bucket=0; bucket<_num_buckets; ++bucket) {
			if (_states[bucket] == State::FILLED) {
				_states[bucket] = State::INACTIVE;
				_pairs[bucket].~PairT();
			}
		}
		_num_filled = 0;
		_max_probe_length = -1;
		HASHMAP_INSTR(
			_stats.numTombstones = 0;
			updateLiveStats();
		);
	}

	void reserve(size_t num_elems)
	{
		size_t required_buckets = num_elems + num_elems/2 + 1; // load factor of 0.66
		if (required_buckets <= _num_buckets) {
			return;
		}

		HASHMAP_INSTR(auto t0 = hashInstrGetNs());

		size_t num_buckets = 4;
		while (num_buckets < required_buckets) { num_buckets *= 2; }

		auto new_states = reinterpret_cast<State*>(::operator new(num_buckets * sizeof(State)));
		auto new_pairs  = reinterpret_cast<PairT*>(::operator new(num_buckets * sizeof(PairT)));

		//auto old_num_filled  = _num_filled;
		auto old_num_buckets = _num_buckets;
		auto old_states      = _states;
		auto old_pairs       = _pairs;

		_num_filled  = 0;
		_num_buckets = num_buckets;
		_mask        = _num_buckets - 1;
		_states      = new_states;
		_pairs       = new_pairs;

		std::fill_n(_states, num_buckets, State::INACTIVE);

		_max_probe_length = -1;

		for (size_t src_bucket=0; src_bucket<old_num_buckets; src_bucket++) {
			if (old_states[src_bucket] == State::FILLED) {
				auto& src_pair = old_pairs[src_bucket];

				auto dst_bucket = find_empty_bucket(src_pair.first);
				DCHECK_NE_F(dst_bucket, (size_t)-1);
				DCHECK_NE_F(_states[dst_bucket], State::FILLED);
				_states[dst_bucket] = State::FILLED;
				new(_pairs + dst_bucket) PairT(std::move(src_pair));
				_num_filled += 1;

				src_pair.~PairT();
			}
		}

		//DCHECK_EQ_F(old_num_filled, _num_filled);

		::operator delete(old_states);
		::operator delete(old_pairs);

		HASHMAP_INSTR(
			_stats.rehashes++;
			_stats.rehashNs += hashInstrGetNs() - t0;
			_stats.peakMaxProbeLength = std::max(_stats.peakMaxProbeLength, _max_probe_length);
			_stats.numTombstones = 0;
			updateLiveStats();
		);
	}

private:
	void check_expand_need()
	{
		reserve(_num_filled + 1);
	}

	size_t find_filled_bucket(const KeyT& key) const
	{
		if (empty()) {
			HASHMAP_INSTR(
				_stats.findMisses++;
				_stats.probeFindMiss[0]++;
			);
			return (size_t)-1;
		}

		HASHMAP_INSTR(auto t0 = hashInstrGetNs());
		auto hash_value = _hasher(key);
		for (int offset=0; offset<=_max_probe_length; ++offset) {
			auto bucket = (hash_value + offset) & _mask;
			if (_states[bucket] == State::FILLED && _comp(_pairs[bucket].first, key)) {
				HASHMAP_INSTR(
					_stats.findHits++;
					_stats.findHitNs += hashInstrGetNs() - t0;
					_stats.probeFindHit[HashContainerStats::ProbeHistBucket(offset)]++;
					_stats.peakMaxProbeLength = std::max(_stats.peakMaxProbeLength, _max_probe_length);
				);
				return bucket;
			}
			if (_states[bucket] == State::INACTIVE) {
				HASHMAP_INSTR(
					_stats.findMisses++;
					_stats.findMissNs += hashInstrGetNs() - t0;
					_stats.probeFindMiss[HashContainerStats::ProbeHistBucket(offset)]++;
				);
				return (size_t)-1;
			}
		}
		HASHMAP_INSTR(
			_stats.findMisses++;
			_stats.findMissNs += hashInstrGetNs() - t0;
			_stats.probeFindMiss[HashContainerStats::ProbeHistBucket(_max_probe_length)]++;
		);
		return (size_t)-1;
	}

	size_t find_or_allocate(const KeyT& key)
	{
		auto hash_value = _hasher(key);
		size_t hole = (size_t)-1;
		int offset=0;
		for (; offset<=_max_probe_length; ++offset) {
			auto bucket = (hash_value + offset) & _mask;

			if (_states[bucket] == State::FILLED) {
				if (_comp(_pairs[bucket].first, key)) {
					HASHMAP_INSTR(_stats.probeInsert[HashContainerStats::ProbeHistBucket(offset)]++);
					return bucket;
				}
			} else if (_states[bucket] == State::INACTIVE) {
				HASHMAP_INSTR(_stats.probeInsert[HashContainerStats::ProbeHistBucket(offset)]++);
				return bucket;
			} else {
				// ACTIVE: keep searching
				if (hole == (size_t)-1) {
					hole = bucket;
				}
			}
		}

		DCHECK_EQ_F(offset, _max_probe_length+1);

		if (hole != (size_t)-1) {
			HASHMAP_INSTR(_stats.probeInsert[HashContainerStats::ProbeHistBucket(offset)]++);
			return hole;
		}

		for (; ; ++offset) {
			auto bucket = (hash_value + offset) & _mask;

			if (_states[bucket] != State::FILLED) {
				_max_probe_length = offset;
				HASHMAP_INSTR(_stats.probeInsert[HashContainerStats::ProbeHistBucket(offset)]++);
				return bucket;
			}
		}
	}

	size_t find_empty_bucket(const KeyT& key)
	{
		auto hash_value = _hasher(key);
		for (int offset=0; ; ++offset) {
			auto bucket = (hash_value + offset) & _mask;
			if (_states[bucket] != State::FILLED) {
				if (offset > _max_probe_length) {
					_max_probe_length = offset;
				}
				return bucket;
			}
		}
		return (size_t(-1));
	}

#ifdef SPRING_HASH_INSTRUMENTATION
	void updateLiveStats() const
	{
		_stats.numFilled = _num_filled;
		_stats.numBuckets = _num_buckets;
		_stats.bucketByteSize = sizeof(PairT) + sizeof(State);
	}

public:
	const HashContainerStats& getStats() const { return _stats; }
	void setStatsSource(const char* file, int line) { _stats.sourceFile = file; _stats.sourceLine = line; }
#endif

private:
	enum class State : uint8_t
	{
		INACTIVE, // Never been touched
		ACTIVE,   // Is inside a search-chain, but is empty
		FILLED    // Is set with key/value
	};

	HashT   _hasher;
	CompT   _comp;
	State*  _states           = nullptr;
	PairT*  _pairs            = nullptr;
	size_t  _num_buckets      =  0;
	size_t  _num_filled       =  0;
	int     _max_probe_length = -1; // Our longest bucket-brigade is this long. ONLY when we have zero elements is this ever negative (-1).
	size_t  _mask             =  0;  // _num_buckets minus one

	HASHMAP_INSTR(mutable HashContainerStats _stats;)
};

} // namespace emilib
