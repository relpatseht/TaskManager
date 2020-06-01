#include "Logger.h"
#include "fiber.h"
#include "mpmc_queue.h"
#include "Task.h"
#include "light_mutex.h"
#include "TaskManager.h"
#include <thread>

#if defined(_MSC_VER)
# define NOINLINE __declspec(noinline)
#else //#if defined(_MSC_VER)
# define NOINLINE __attribute__ ((noinline))
#endif //#else //#if defined(_MSC_VER)

namespace
{
#pragma pack(push, 1)
	struct QueuedTask
	{
		task::Task task;
		uint32_t counterIndex;
	};
#pragma pack(pop)
}

namespace task
{
	struct Manager;

	struct Counter
	{
		// The manager, fiber pair to wake when val hits 0.
		// Not neccessarily the owning manager.
		std::atomic<Manager*> wakeManager;
		std::atomic<fiber::Fiber*> wakeFiber;

		std::atomic_uint val;
		
		uint8_t cachePadding[64 - (sizeof(Counter::wakeManager) + sizeof(Counter::wakeFiber) + sizeof(Counter::val))];
	};
	static_assert(sizeof(Counter) == 64, "Counter not cache size aligned");

	struct Manager
	{
		fiber::Fiber** fibers;
		std::thread* threads;
		sync::light_mutex* threadTaskLocks;
		mpmc::queue<QueuedTask>* queuedTasks;
		mpmc::queue<fiber::Fiber*> waitingFibers;
		mpmc::queue<fiber::Fiber*> openFibers;
		mpmc::queue<Counter*> openCounters;
		Counter* taskCounters;
		unsigned numWorkers;
		unsigned numFibers;
		std::atomic_bool shutdown;
	};
}

namespace
{
	// Everything in TLS must be NOINLINE to prevent tls optimization
	// which would break fibers (fiber sleeps on one thread, wakes on
	// another, now has bad tls pointer)
	namespace tls
	{
		namespace tls_internal
		{
			static unsigned s_workerIndex = ~0u;
		}

		static NOINLINE void SetWorkerIndex(unsigned index)
		{
			tls_internal::s_workerIndex = index;
		}

		static NOINLINE unsigned GetWorkerIndex()
		{
			return tls_internal::s_workerIndex;
		}
	}

	static __forceinline bool NextTask(task::Manager* inoutTaskManager, mpmc::queue<QueuedTask>* inoutTaskQueue, fiber::Fiber *curFiber, QueuedTask* outTask)
	{
		if (inoutTaskManager->waitingFibers.approx_size())
		{
			fiber::Fiber* waitingFiber;

			if (inoutTaskManager->waitingFibers.try_pop(&waitingFiber))
			{
				const bool pushed = inoutTaskManager->openFibers.try_push(curFiber);
				sanity(pushed && "Open fiber queue full. Shouldn't be possible.");
				fiber::SwitchToFiber(curFiber, waitingFiber);
			}
		}

		return inoutTaskQueue->try_pop(outTask);
	}

	static void TaskLoop(void* userData)
	{
		fiber::Fiber* thisFiber; // Hackery. All fibers store a pointer to themselves as their first stack entry
		task::Manager* const manager = reinterpret_cast<task::Manager*>(userData);

		while (!manager->shutdown.load(std::memory_order_relaxed))
		{
			const unsigned workerIndex = tls::GetWorkerIndex();
			mpmc::queue<QueuedTask>* const taskQueue = manager->queuedTasks + workerIndex;
			QueuedTask curTask;

			sanity(workerIndex < manager->numWorkers);

			if (!taskQueue->approx_size() && !manager->waitingFibers.approx_size())
			{
				sync::light_mutex* const taskLock = manager->threadTaskLocks + workerIndex;

				// Wait until tasks are available
				sync::lock(taskLock);
				sync::unlock(taskLock);
			}

			while (NextTask(manager, taskQueue, )
			{
				task::Counter* const counter = manager->taskCounters + curTask.counterIndex;

				curTask.task();
				const unsigned oldVal = counter->val.fetch_sub(1, std::memory_order_acq_rel);
				sanity(oldVal > 0 && "Counter underflow");

				// This was the last task of a counter. Move the waiting fiber to
				// the wait queue for continuation
				if (oldVal == 1)
				{
					task::Manager* const wakeManager = counter->wakeManager.load(std::memory_order_acquire);

					// It may be that WaitForCounter (which sets wakeManager and wakeFiber) hasn't
					// been called yet. WaitForCounter handles val already being 0. Don't add to queue
					// if not yet possible.
					if (wakeManager)
					{
						fiber::Fiber* const wakeFiber = counter->wakeFiber.load(std::memory_order_acquire);

						if (wakeFiber)
						{
							const bool fiberPushed = wakeManager->waitingFibers.try_push(wakeFiber);
							sanity(fiberPushed && "Fiber waite queue full. Shouldn't be possible.");
						}
					}
				}
			}
		}
	}

	static void Worker(task::Manager* manager, unsigned workerIndex)
	{
		s_workerIndex = workerIndex;
		sync::light_mutex* const taskLock = manager->threadTaskLocks + workerIndex;

		fiber::Fiber *threadFiber = fiber::InitForThread();

		while (!manager->shutdown.load(std::memory_order_relaxed))
		{
			// Wait until tasks are available
			sync::lock(taskLock);
			sync::unlock(taskLock);

			fiber::Fiber* fiber;
			bool fiberAcquired = manager->openFibers.try_pop(&fiber);
			sanity(fiberAcquired && "Ran out of open fibers. Too many stalled tasks");

			fiber::SwitchToFiber(threadFiber, fiber);
		}
	}
}

namespace task
{
	Manager* Create(unsigned numWorkers, unsigned numFibers, unsigned numTasksPerWorker, unsigned fiberStackSize, Flags flags)
	{
		const unsigned numCounters = numTasksPerWorker * numWorkers;
		const size_t fibersSize = sizeof(fiber::Fiber*) * numFibers;
		const size_t threadsSize = sizeof(std::thread) * numWorkers;
		const size_t locksSize = sizeof(sync::light_mutex) * numWorkers;
		const size_t tasksSize = sizeof(mpmc::queue<QueuedTask>) * numWorkers;
		const size_t countersSize = sizeof(Counter) * numCounters;
		const size_t totalAllocSize = sizeof(Manager) + fibersSize + tasksSize + locksSize + threadsSize + countersSize;
		void* const mem = malloc(totalAllocSize);
		uint8_t* u8Mem = reinterpret_cast<uint8_t*>(mem);

		std::memset(mem, 0, totalAllocSize);

		Manager* const manager = reinterpret_cast<Manager*>(u8Mem);
		u8Mem += sizeof(Manager);

		manager->numWorkers = numWorkers;
		manager->numFibers = numFibers;
		manager->shutdown.store(false, std::memory_order_relaxed);

		manager->fibers = reinterpret_cast<fiber::Fiber**>(u8Mem);
		u8Mem += fibersSize;

		manager->threads = reinterpret_cast<std::thread*>(u8Mem);
		u8Mem += threadsSize;

		manager->threadTaskLocks = reinterpret_cast<sync::light_mutex*>(u8Mem);
		u8Mem += locksSize;

		manager->queuedTasks = reinterpret_cast<mpmc::queue<QueuedTask>*>(u8Mem);
		u8Mem += tasksSize;

		manager->taskCounters = reinterpret_cast<Counter*>(u8Mem);
		u8Mem += countersSize;

		for (unsigned workerIndex = 0; workerIndex < numWorkers; ++workerIndex)
		{
			new(manager->queuedTasks + workerIndex) mpmc::queue<QueuedTask>(numTasksPerWorker);
			manager->threadTaskLocks[workerIndex].store(sync::LIGHT_MUTEX_INIT, std::memory_order_relaxed);
			sync::lock(manager->threadTaskLocks + workerIndex); // All threads start locked until tasks are added
		}

		new(&manager->waitingFibers) mpmc::queue<fiber::Fiber*>(numFibers);
		new(&manager->openFibers) mpmc::queue<fiber::Fiber*>(numFibers);
		for (unsigned fiberIndex = 0; fiberIndex < numFibers; ++fiberIndex)
		{
			fiber::Fiber * const fiber = fiber::Create(fiberStackSize, TaskLoop, manager);
			manager->fibers[fiberIndex] = fiber;
			const bool pushed = manager->openCounters.try_push(fiber);
			sanity(pushed, "Failed to initalize fiber queue.");
		}

		new(&manager->openCounters) mpmc::queue<Counter*>(numCounters);
		for (unsigned counterIndex = 0; counterIndex < numCounters; ++counterIndex)
		{
			Counter* const counter = manager->taskCounters + counterIndex;

			counter->val.store(0, std::memory_order_relaxed);
			counter->wakeFiber.store(nullptr, std::memory_order_relaxed);
			counter->wakeManager.store(nullptr, std::memory_order_relaxed);

			const bool pushed = manager->openCounters.try_push(counter);
			sanity(pushed, "Failed to initialize counter queue.");
		}

		for (unsigned workerIndex = 0; workerIndex < numWorkers; ++workerIndex)
			new(manager->threads + workerIndex) std::thread(&Worker, manager, workerIndex);

		return manager;
	}

	void Destroy(Manager* manager)
	{
		manager->shutdown.store(true, std::memory_order_relaxed);

		// Wake up all threads
		for (unsigned workerIndex = 0; workerIndex < manager->numWorkers; ++workerIndex)
		{
			sync::try_lock(manager->threadTaskLocks + workerIndex);
			sync::unlock(manager->threadTaskLocks + workerIndex);
		}

		for (unsigned workerIndex = 0; workerIndex < manager->numWorkers; ++workerIndex)
		{
			manager->threads[workerIndex].join();
			manager->threads[workerIndex].~thread();
			manager->queuedTasks[workerIndex].~queue();
		}

		for (unsigned fiberIndex = 0; fiberIndex < manager->numFibers; ++fiberIndex)
			fiber::Destroy(manager->fibers[fiberIndex]);
		
		manager->openCounters.~queue();
		manager->openFibers.~queue();
		manager->waitingFibers.~queue();

		free(manager);
	}

	void RunJobs(Manager* manager, Task* tasks, unsigned numTasks, Counter** outCounter)
	{

	}

	void WaitForCounter(Manager* manager, Counter* counter, unsigned value)
	{

	}
}
