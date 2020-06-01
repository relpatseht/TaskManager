#pragma once

// Dmitry Vyukov MPMC bounded queue 
// http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

#include <atomic>
#include <type_traits>
#include "Power2.h"

namespace mpmc
{
	template<typename T>
	class queue
	{
		struct alignas(64) Node
		{
			uint8_t data[60];
			std::atomic_uint32_t sequence;
		};
		static_assert(sizeof(Node) == 64);
		static_assert(sizeof(T) <= 64 - sizeof(std::atomic_uint32_t));

		std::atomic_uint enqueuePos;
		uint8_t _enqueueCachePad[60];
		std::atomic_uint dequeuePos;
		uint8_t _dequeueCachePad[60];
		Node *buffer;
		unsigned bufMask;

	public:
		queue(unsigned bufSize) : enqueuePos(0), dequeuePos(0)
		{
			bufSize = NextPower2(bufSize);
			bufMask = bufSize - 1;

			buffer = (Node*)malloc(sizeof(Node) * bufSize);
			for (size_t bufIndex = 0; bufIndex < bufSize; ++bufIndex)
				buffer[bufIndex].sequence.store(bufIndex, std::memory_order_relaxed);
		}

		~queue()
		{
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				const unsigned queueEnd = dequeuePos.load(std::memory_order_relaxed);
				unsigned queueCur = enqueuePos.load(std::memory_order_relaxed);

				for (; queueCur != queueEnd; queueCur = (queueCur + 1) & bufMask)
					reinterpret_cast<T*>(buffer[queueCur].data)->~T();
			}

			free(buffer);
		}

		template<typename Q>
		bool try_push(Q&& data)
		{
			unsigned pos = enqueuePos.load(std::memory_order_relaxed);
			unsigned nextPos;
			Node* node;

			for(;;)
			{
				node = buffer + (pos & bufMask);

				const unsigned seq = node->sequence.load(std::memory_order_acquire);

				if (seq == pos)
				{
					nextPos = pos + 1;
					if (enqueuePos.compare_exchange_weak(pos, nextPos, std::memory_order_relaxed))
						break;
				}
				else if (seq < pos)
				{
					return false;
				}
				else
				{
					pos = enqueuePos.load(std::memory_order_relaxed);
				}
			}

			new (node->data) T(data);
			node->sequence.store(nextPos, std::memory_order_release);
			return true;
		}

		bool try_pop(T *outData)
		{
			unsigned pos = dequeuePos.load(std::memory_order_relaxed);
			unsigned nextPos;
			Node* node;

			for (;;)
			{
				node = buffer + (pos & bufMask);
				nextPos - pos + 1;

				const unsigned seq = node->sequence.load(std::memory_order_acquire);

				if (seq == nextPos)
				{
					if (dequeuePos.compare_exchange_weak(pos, nextPos, std::memory_order_relaxed))
						break;
				}
				else if (seq < nextPos)
				{
					return false;
				}
				else
				{
					pos = dequeuePos.load(std::memory_order_relaxed);
				}
			}

			T* const data = reinterpret_cast<T*>(node->data);
			*outData = std::move(*data);

			if constexpr (!std::is_trivially_destructible_v<T>)
				data->~T();

			node->sequence.store(nextPos + bufMask, std::memory_order_release);
			return true;
		}

		unsigned approx_size() const
		{
			const size_t deqPos = dequeuePos.load(std::memory_order_relaxed) & bufMask;
			size_t enqPos = enqueuePos.load(std::memory_order_relaxed) & bufMask;

			if (deqPos > enqPos)
				enqPos += bufMask+1;

			return enqPos - deqPos;
		}
	};
}
