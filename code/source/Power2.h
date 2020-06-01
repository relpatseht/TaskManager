#pragma once

#include <intrin.h>
#include <cstdint>

#if defined(_MSC_VER)
inline uint64_t NextPower2(uint64_t v)
{
	if (v <= 1)
		return 1;
	else
	{
		unsigned long bit;
		_BitScanReverse64(&bit, v-1);
		return 1ull << (64-bit);
	}
}

inline uint32_t NextPower2(uint32_t v)
{
	if (v <= 1)
		return 1;
	else
	{
		unsigned long bit;
		_BitScanReverse(&bit, v - 1);
		return 1u << (32-bit);
	}
}
#else //#if defined(_MSC_VER)
inline uint64_t NextPower2(uint64_t v)
{
	return v <= 1 ? 1 : (1ull << (64-__builtin_clz(v - 1)));
}

inline uint32_t NextPower2(uint32_t v)
{
	return v <= 1 ? 1 : (1ull << (32 - __builtin_clz(v - 1)));
}
#endif //#else //#if defined(_MSC_VER)

template<size_t Round>
inline uint64_t RoundUpPower2(uint64_t v)
{
	static_assert((Round & (Round - 1)) == 0, "Round must be power of 2");

	return (v + (Round - 1)) & (Round - 1);
}

template<size_t Round>
inline uint32_t RoundUpPower2(uint32_t v)
{
	static_assert((Round & (Round - 1)) == 0, "Round must be power of 2");

	return (v + (Round - 1)) & (Round - 1);
}
