/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef COB_ENGINE_H
#define COB_ENGINE_H

/*
 * Simple VM responsible for "scheduling" and running COB threads.
 * It also manages reading and caching of the actual .cob files.
 */

#include <vector>

#include "CobThread.h"
#include "CobDeferredCallin.h"
#include "System/creg/creg_cond.h"
#include "System/creg/STL_Queue.h"
#include "System/creg/STL_Map.h"
#include "System/Cpp11Compat.hpp"
#include "System/SlotMap.hpp"

class CCobThread;
class CCobInstance;
class CCobFile;
class CCobFileHandler;

class CCobEngine
{
	CR_DECLARE_STRUCT(CCobEngine)

public:
	struct SleepingThread {
		CR_DECLARE_STRUCT(SleepingThread)

		CobThreadHandle id;
		int wt;
	};

	struct CCobThreadComp {
	public:
		bool operator() (const SleepingThread& a, const SleepingThread& b) const {
			return a.wt > b.wt || (a.wt == b.wt && a.id > b.id);
		}
	};

public:
	void Init() {
		threadInstances.reserve(2048);
		tickAddedThreads.reserve(128);

		runningThreadIDs.reserve(512);
		waitingThreadIDs.reserve(512);

		sleepingThreadIDs = {};

		curThread = nullptr;

		currentTime = 0;
	}
	void Kill() {
		// threadInstances is never explicitly iterated in the actual code,
		// but iterated during sync dumps; slot_map::clear() resets storage,
		// freelist, and live count.
		threadInstances.clear();
		spring::clear_unordered_map(deferredCallins);
		tickAddedThreads.clear();

		runningThreadIDs.clear();
		waitingThreadIDs.clear();

		while (!sleepingThreadIDs.empty()) {
			sleepingThreadIDs.pop();
		}
	}

	void Tick(int deltaTime);
	void ShowScriptError(const std::string& msg);


	CCobThread* GetThread(CobThreadHandle threadID) {
		return threadInstances.find(threadID);
	}

	bool RemoveThread(CobThreadHandle threadID);
	CobThreadHandle AddThread(CCobThread&& thread);
	CobThreadHandle ReserveThreadHandle() { return threadInstances.reserve_handle(); }
	void ReleaseThreadHandle(CobThreadHandle h) { threadInstances.release_handle(h); }

	void QueueAddThread(CCobThread&& thread) { tickAddedThreads.emplace_back(std::move(thread)); }
	void QueueRemoveThread(CobThreadHandle threadID) { tickRemovedThreads.emplace_back(threadID); }
	void ProcessQueuedThreads();

	void ScheduleThread(const CCobThread* thread);
	void SanityCheckThreads(const CCobInstance* owner);

	const auto& GetThreadInstances() const { return threadInstances; }
//	const auto& GetTickAddedThreads() const { return tickAddedThreads; }
//	const auto& GetTickRemovedThreads() const { return tickRemovedThreads; }
//	const auto& GetRunningThreadIDs() const { return runningThreadIDs; }
	const auto& GetWaitingThreadIDs() const { return waitingThreadIDs; }
	const auto& GetSleepingThreadIDs() const { return sleepingThreadIDs; }
	const auto  GetCurrTime() const { return currentTime; }

	void AddDeferredCallin(CCobDeferredCallin&& deferredCallin);
	void RunDeferredCallins();
private:
	void TickThread(CCobThread* thread);

	void WakeSleepingThreads();
	void TickRunningThreads();

private:
	// registry of every thread across all script instances
	spring::slot_map<CCobThread> threadInstances;
	// threads that are spawned during Tick
	std::vector<CCobThread> tickAddedThreads;
	// threads that are killed during Tick
	std::vector<CobThreadHandle> tickRemovedThreads;

	std::vector<CobThreadHandle> runningThreadIDs;
	std::vector<CobThreadHandle> waitingThreadIDs;

	spring::unordered_map<int, std::vector<CCobDeferredCallin> > deferredCallins;

	// stores <id, waketime> pairs s.t. after waking up the ID can be checked
	// for validity; thread owner might get removed while a thread is sleeping
	std::priority_queue<SleepingThread, std::vector<SleepingThread>, CCobThreadComp> sleepingThreadIDs;

	CCobThread* curThread = nullptr;

	int currentTime = 0;
};


extern CCobEngine* cobEngine;

#endif // COB_ENGINE_H
