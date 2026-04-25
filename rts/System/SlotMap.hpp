/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <new>
#include <utility>
#include <vector>

#include "System/creg/creg_cond.h"
#include "System/creg/STL_Deque.h"

namespace spring {

// Generational arena (slot map). Stores T by value in a deque of slots;
// each slot carries a generation counter so stale handles can be detected.
//
// Handle layout: 64-bit, low 32 = slot index, high 32 = generation.
// Generation 0 is reserved for "never assigned" so the all-zero handle is a
// natural sentinel — no live entry ever produces it.
//
// Storage uses std::deque rather than std::vector for pointer/reference
// stability: callers may hold a T* (e.g. the currently-ticking COB thread)
// while inserting new entries, and deque's emplace_back never relocates
// existing elements.

template <typename T>
struct slot_map_slot {
	CR_DECLARE_STRUCT(slot_map_slot<T>)

	uint32_t generation = 0;
	bool     occupied   = false;
	T        value{};
};


template <typename T>
class slot_map {
	CR_DECLARE_STRUCT(slot_map<T>)

public:
	using key_type    = uint64_t;
	using mapped_type = T;
	using size_type   = std::size_t;
	using slot_type   = slot_map_slot<T>;

	static constexpr key_type npos = 0;

	static constexpr uint32_t slot_of(key_type h) noexcept { return uint32_t(h & 0xFFFFFFFFu); }
	static constexpr uint32_t gen_of (key_type h) noexcept { return uint32_t(h >> 32); }
	static constexpr key_type pack(uint32_t slot, uint32_t gen) noexcept {
		return (key_type(gen) << 32) | key_type(slot);
	}

	// ---- capacity ----
	size_type size()  const noexcept { return liveCount; }
	bool      empty() const noexcept { return liveCount == 0; }

	// std::deque has no reserve() — its segmented storage grows without
	// relocating existing elements, so pre-reservation isn't needed for
	// the pointer-stability guarantee. Provided as a no-op so the call site
	// in CCobEngine::Init() reads naturally.
	void reserve(size_type /*n*/) {}

	// ---- mutation ----
	template <typename... Args>
	key_type emplace(Args&&... args) {
		const uint32_t idx = allocSlot();
		slot_type& s = slots[idx];
		s.value = T(std::forward<Args>(args)...);
		s.occupied = true;
		++liveCount;
		return pack(idx, s.generation);
	}

	// Two-phase: reserve a handle now, fill the value later via restore().
	// Used when the value is constructed/moved through an external queue
	// (CobEngine's tickAddedThreads) before it lands in the registry.
	key_type reserve_handle() {
		const uint32_t idx = allocSlot();
		// occupied stays false until restore() lands the value.
		return pack(idx, slots[idx].generation);
	}

	void restore(key_type handle, T&& v) {
		const uint32_t idx = slot_of(handle);
		assert(idx < slots.size());
		slot_type& s = slots[idx];
		assert(s.generation == gen_of(handle));
		assert(!s.occupied);
		s.value = std::move(v);
		s.occupied = true;
		++liveCount;
	}

	// Cancel a reserve_handle() that was never restored. The slot returns to
	// the freelist; the generation is left bumped so the abandoned handle
	// stays unique. Call only on a handle that was reserved-but-never-restored.
	void release_handle(key_type handle) {
		const uint32_t idx = slot_of(handle);
		assert(idx < slots.size());
		slot_type& s = slots[idx];
		assert(s.generation == gen_of(handle));
		assert(!s.occupied);
		freeSlots.push_back(idx);
	}

	bool erase(key_type handle) {
		const uint32_t idx = slot_of(handle);
		if (idx >= slots.size())
			return false;
		slot_type& s = slots[idx];
		if (!s.occupied || s.generation != gen_of(handle))
			return false;
		s.occupied = false;
		// Run T's destructor (not move-assignment from a default-constructed
		// temp): for types like CCobThread whose dtor performs cleanup that
		// move-assign doesn't replicate, this is the only way to fire it.
		s.value.~T();
		::new (static_cast<void*>(&s.value)) T{};
		freeSlots.push_back(idx);
		--liveCount;
		return true;
	}

	void clear() {
		slots.clear();
		freeSlots.clear();
		liveCount = 0;
	}

	// ---- lookup ----
	T* find(key_type handle) noexcept {
		const uint32_t idx = slot_of(handle);
		if (idx >= slots.size())
			return nullptr;
		slot_type& s = slots[idx];
		if (!s.occupied || s.generation != gen_of(handle))
			return nullptr;
		return &s.value;
	}

	const T* find(key_type handle) const noexcept {
		const uint32_t idx = slot_of(handle);
		if (idx >= slots.size())
			return nullptr;
		const slot_type& s = slots[idx];
		if (!s.occupied || s.generation != gen_of(handle))
			return nullptr;
		return &s.value;
	}

	bool contains(key_type handle) const noexcept { return find(handle) != nullptr; }

	// ---- iteration ----
	// Dereferences to a proxy with first/second members so structured bindings
	// `for (auto& [tid, val] : map)` work the same as on std::unordered_map.
	struct value_proxy       { key_type first; T&       second; };
	struct const_value_proxy { key_type first; const T& second; };

	class iterator {
	public:
		iterator(slot_map* m, uint32_t i) : m(m), idx(i) {}
		iterator& operator++() { advance(); return *this; }
		value_proxy operator*() const {
			auto& s = m->slots[idx];
			return { pack(idx, s.generation), s.value };
		}
		bool operator==(const iterator& o) const { return idx == o.idx; }
		bool operator!=(const iterator& o) const { return idx != o.idx; }
	private:
		void advance() {
			++idx;
			while (idx < m->slots.size() && !m->slots[idx].occupied)
				++idx;
		}
		slot_map* m;
		uint32_t idx;
		friend class slot_map;
	};

	class const_iterator {
	public:
		const_iterator(const slot_map* m, uint32_t i) : m(m), idx(i) {}
		const_iterator& operator++() { advance(); return *this; }
		const_value_proxy operator*() const {
			const auto& s = m->slots[idx];
			return { pack(idx, s.generation), s.value };
		}
		bool operator==(const const_iterator& o) const { return idx == o.idx; }
		bool operator!=(const const_iterator& o) const { return idx != o.idx; }
	private:
		void advance() {
			++idx;
			while (idx < m->slots.size() && !m->slots[idx].occupied)
				++idx;
		}
		const slot_map* m;
		uint32_t idx;
		friend class slot_map;
	};

	iterator       begin()        { return iterator(this, firstOccupied()); }
	iterator       end()          { return iterator(this, uint32_t(slots.size())); }
	const_iterator begin()  const { return const_iterator(this, firstOccupied()); }
	const_iterator end()    const { return const_iterator(this, uint32_t(slots.size())); }
	const_iterator cbegin() const { return begin(); }
	const_iterator cend()   const { return end();   }

public:
	// Public for creg traversal. Treat as private for all other purposes.
	std::deque<slot_type> slots;        // pointer-stable across emplace_back
	std::vector<uint32_t> freeSlots;    // free indices into `slots`
	size_type             liveCount = 0;

private:
	uint32_t firstOccupied() const {
		uint32_t i = 0;
		while (i < slots.size() && !slots[i].occupied)
			++i;
		return i;
	}

	uint32_t allocSlot() {
		uint32_t idx;
		if (!freeSlots.empty()) {
			idx = freeSlots.back();
			freeSlots.pop_back();
		} else {
			idx = uint32_t(slots.size());
			slots.emplace_back();
		}
		uint32_t& gen = slots[idx].generation;
		gen = (gen == 0xFFFFFFFFu) ? 1u : (gen + 1u);
		return idx;
	}
};

} // namespace spring


CR_BIND_TEMPLATE_1TYPED(spring::slot_map_slot, T, )
CR_REG_METADATA_TEMPLATE_1TYPED(spring::slot_map_slot, T, (
	CR_MEMBER(generation),
	CR_MEMBER(occupied),
	CR_MEMBER(value)
))

CR_BIND_TEMPLATE_1TYPED(spring::slot_map, T, )
CR_REG_METADATA_TEMPLATE_1TYPED(spring::slot_map, T, (
	CR_MEMBER(slots),
	CR_MEMBER(freeSlots),
	CR_MEMBER(liveCount)
))
