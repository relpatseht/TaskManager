#include "light_mutex.h"
#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <Windows.h>
#elif defined(__linux__) //#if defined(_WIN32)
# include <linux/futex.h>
#else //#elif defined(__linux__) //#if defined(_WIN32)
# error Unknown OS
#endif //#else //#elif defined(__linux__) //#if defined(_WIN32)

namespace
{
	enum State : uint32_t
	{
		UNLOCKED,
		LOCKED_NO_WAITING,
		LOCKED_THREADS_WAITING
	};

	static __forceinline void SysLock(sync::light_mutex* m)
	{
#if defined(_WIN32)
		uint32_t cmp = LOCKED_THREADS_WAITING;
		WaitOnAddress(reinterpret_cast<volatile void*>(m), &cmp, sizeof(*m), INFINITE);
#elif defined(__linux__) //#if defined(_WIN32)
		syscall(SYS_FUTEX, reinterpret_cast<int*>(m), FUTEX_WAIT, LOCKED_THREADS_WAITING, 0, 0, 0);
#endif //#elif defined(__linux__) //#if defined(_WIN32)
	}

	static __forceinline void SysUnlock(sync::light_mutex* m)
	{

#if defined(_WIN32)
		WakeByAddressAll(m);
#elif defined(__linux__) //#if defined(_WIN32)
		syscall(SYS_FUTEX, reinterpret_cast<int*>(m), FUTEX_WAKE, LOCKED_NO_WAITING, 0, 0, 0);
#endif //#elif defined(__linux__) //#if defined(_WIN32)
	}

	static __forceinline uint32_t CompareExchange(sync::light_mutex* m, uint32_t expected, uint32_t desired)
	{
		m->compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
		return expected;
	}
}

namespace sync
{
	bool try_lock(light_mutex* m)
	{
		return CompareExchange(m, UNLOCKED, LOCKED_NO_WAITING) == UNLOCKED;
	}

	void lock(light_mutex* m)
	{
		uint32_t oldState = CompareExchange(m, UNLOCKED, LOCKED_NO_WAITING);

		if (oldState != UNLOCKED)
		{
			do
			{
				if (oldState == LOCKED_THREADS_WAITING || CompareExchange(m, LOCKED_NO_WAITING, LOCKED_THREADS_WAITING) != UNLOCKED)
					SysLock(m);
			} while ((oldState = CompareExchange(m, UNLOCKED, LOCKED_THREADS_WAITING)) != UNLOCKED);
		}
	}

	void unlock(light_mutex* m)
	{
		if (m->fetch_sub(1, std::memory_order_acq_rel) != LOCKED_NO_WAITING)
		{
			m->store(UNLOCKED, std::memory_order_release);
			SysUnlock(m);
		}
	}
}