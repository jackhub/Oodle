// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "vec128.inl"
#ifdef DO_BUILD_AVX2
#include <immintrin.h>
#endif

OODLE_NS_START

#ifdef DO_BUILD_AVX2

typedef __m256i Vec256;
typedef __m256i const & Vec256r;

static inline Vec256 load256a(const void *ptr)		{ return _mm256_load_si256((const __m256i *)ptr); }
static inline Vec256 load256u(const void *ptr)		{ return _mm256_loadu_si256((const __m256i *)ptr); }
static inline void store256u(void *ptr, Vec256r x)	{ _mm256_storeu_si256((__m256i *)ptr, x); }

static inline Vec128 lo_half(Vec256r x)				{ return _mm256_castsi256_si128(x); }
static inline Vec128 hi_half(Vec256r x)				{ return _mm256_extracti128_si256(x, 1); }
static inline Vec256 combine(Vec128r a, Vec128r b)	{ return _mm256_inserti128_si256(_mm256_castsi128_si256(a), b, 1); }
static inline Vec256 broadcast128_256(Vec128r x)	{ return _mm256_broadcastsi128_si256(x); }

template<int x,int y,int z,int w>
static inline Vec256 shuffle32in128(Vec256r v)		{ return _mm256_shuffle_epi32(v, _MM_SHUFFLE(w,z,y,x)); }

static inline Vec256 vec_packed_add(Vec256r a, Vec256r b, Vec256r mask_nonmsb)
{
	// sum low bits directly
	Vec256 low = _mm256_add_epi64(_mm256_and_si256(a, mask_nonmsb), _mm256_and_si256(b, mask_nonmsb));
	// carryless sum (=XOR) in MSBs
	return _mm256_xor_si256(low, _mm256_andnot_si256(mask_nonmsb, _mm256_xor_si256(a, b)));
}

#endif

OODLE_NS_END

