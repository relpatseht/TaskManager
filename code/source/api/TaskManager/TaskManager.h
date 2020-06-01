#pragma once

#include "Task.h"

namespace task
{
	struct Manager;
	struct Counter;

	enum class Flags
	{
		NONE       = 0,
		AFFINITIZE = 1<<0
	};

	Manager *Create(unsigned numWorkers, unsigned numFibers, unsigned numTasksPerWorker, unsigned fiberStackSize, Flags flags);
	void Destroy(Manager* manager);

	void RunJobs(Manager* manager, Task* tasks, unsigned numTasks, Counter** outCounter);
	void WaitForCounter(Manager* manager, Counter* counter, unsigned value);
}
