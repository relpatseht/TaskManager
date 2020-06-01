#pragma once

#include <atomic>
#include <cstdint>

namespace sync
{
	typedef std::atomic_uint32_t light_mutex;
	static constexpr uint32_t LIGHT_MUTEX_INIT = 0;

	void lock(light_mutex* m);
	bool try_lock(light_mutex* m);
	void unlock(light_mutex* m);
}
