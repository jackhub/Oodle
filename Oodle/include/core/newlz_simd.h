// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "rrbase.h"
#include "cpux86.h"

//=========

#ifdef __RADSSE2__

#if defined(__RADJAGUAR__) || defined(__RADZEN2__)

#ifndef __SSE4_1__
#define __SSE4_1__
#endif

#include <smmintrin.h> // SSE4

#else

#include <emmintrin.h> // SSE2

#endif

#if defined(_MSC_VER)
#pragma warning(disable : 4324) // structure was padded due to __declspec(align())
#endif

// don't build AVX2 when we explicitly disabled in build
#if ! defined(__RADNOAVX2__)
#define DO_BUILD_AVX2
#endif

// always build SSE4
#define DO_BUILD_SSE4

#elif defined(__RADNEON__)

#include <arm_neon.h>

#else

// other vector ISAs?

#endif

//=========

OODLE_NS_START

//=========

#if ( defined(__RADNT__) || defined(__RADMAC__) || defined(__RADLINUX__) || defined(__RADWINRT__) ) 

#ifdef __RADSSE2__

// platforms where we test for SSE4 and fallback if not found

#define DO_SSE4_TEST

//#pragma message("DO_SSE4_TEST")
		
static rrbool newlz_simd_has_sse4()
{
	return rrCPUx86_feature_present(RRX86_CPU_SSE41);
}

#else

// Linux and Mac ARM

static rrbool newlz_simd_has_sse4()
{
	return false;
}

#endif // __RADSSE2__

#elif defined(__RADJAGUAR__) || defined(__RADZEN2__)

// platforms where we know we have a speciifc CPU and don't need a fallback

#define DO_SSE4_ALWAYS
		
static rrbool newlz_simd_has_sse4()
{
	return true;
}

#else

static rrbool newlz_simd_has_sse4()
{
	return false;
}

#endif

// Update this part with AVX2 feature detection once we're on compilers
// that actually support it for our Windows/Linux/Mac builds, but right
// now we only have this on console targets.
//
// Detecting this and using AVX2 automatically is a problem because of
// Intel's downclocking shenanigans; don't want to use AVX2 in a loop
// that runs for a sub-millisecond and force the whole package into
// a lower-frequency power state as a result when the app is running.
//
// Our test workloads have hit that - be very very careful here.
// (It's a lot less problematic with Zens which don't have any
// drastic clock rate changes from AVX2 usage.)

// -> DO_AVX2_ALWAYS + DO_AVX2_TEST

#if defined(__RADZEN2__)

#define DO_AVX2_ALWAYS

static rrbool newlz_simd_has_avx2()
{
	return true;
}

#else

static rrbool newlz_simd_has_avx2()
{
	return false;
}

#endif

//=========
/**

_256 means do 256

_256 with "num" means array sizes are 256, num <= 256, and you may overrun
	furthermore, the arrays have zeros past "num" so you may over-read

"num" without _256 means don't overrun / don't overread

**/

// to = fm1+fm2 or to = fm1-fm2
// to == fm is okay
void simd_add_u32_256(U32 * to, const U32 * fm1, const U32 * fm2);
void simd_sub_u32_256(U32 * to, const U32 * fm1, const U32 * fm2);

// dotproduct components (v1[i]*v2[i]) must fit in S32
//	overflow is undefined

S32 simd_dotproduct_s16_s16_256(const S16 * v1, const S16 * v2);
S32 simd_dotproduct_s32_s16_256(const S32 * v1, bool v1_fits_in_S16, const S16 * v2);
S32 simd_dotproduct_s32_s8_256( const S32 * v1, bool v1_fits_in_S16, const S8 * v2,int num);

S32 simd_horizontal_sum_s32(const S32 *v,int num);

// offsets = offsets * multiplier - subtrahend
// offsets must be 16-aligned
void simd_mul_s32_sub_u8(S32 * offsets, SINTa offsets_count, S32 multiplier, const U8 * subtrahend );

// simd_mul_s32_sub_u8 calls simd_mul_s32_sub_u8_sse4 for you if possible
void simd_mul_s32_sub_u8_sse4(S32 * offsets, SINTa offsets_count, S32 multiplier, const U8 * subtrahend );

// out = lo,hi,lo,hi
void simd_interleave_8x2(U16 * off16s,SINTa num_off16s,const U8 * off16s_lo,const U8 * off16s_hi);
		
U32 simd_find_max_U32_256(const U32 * histogram);

//=========

#ifdef __RADSSE2__

void newlz_multiarrays_trellis_core_sse2(
	U64 * switch_flags,
	U8  * cheapest_entropyset,
	const U8 * ptr, const U8 * base, const U8 *end,
	const U16 * entropyset_cost,
	const U16 * histo_codelens_transposed,
	int num_entropysets,int num_entropysets_padded,
	int prev_cost_cheapest,int switch_histo_cost_codelen);

void newlz_multiarrays_trellis_core_sse4(
	U64 * switch_flags,
	U8  * cheapest_entropyset,
	const U8 * ptr, const U8 * base, const U8 *end,
	const U16 * entropyset_cost,
	const U16 * histo_codelens_transposed,
	int num_entropysets,int num_entropysets_padded,
	int prev_cost_cheapest,int switch_histo_cost_codelen);
	
#endif
	
#ifdef __RADNEON__

void newlz_multiarrays_trellis_core_neon(
	U64 * switch_flags,
	U8  * cheapest_entropyset,
	const U8 * ptr, const U8 * base, const U8 *end,
	const U16 * entropyset_cost,
	const U16 * histo_codelens_transposed,
	int num_entropysets,int num_entropysets_padded,
	int prev_cost_cheapest,int switch_histo_cost_codelen);

#endif

//=========

#if defined(__RADSSE2__)
	
// horizontal sum
static RADINLINE S32 hsum_epi32_sse2(__m128i x)
{
    __m128i y = _mm_add_epi32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1,0,3,2)) );
    __m128i z = _mm_add_epi32(y, _mm_shuffle_epi32(y, _MM_SHUFFLE(2,3,0,1)) );
    // horizontal sum, same value in every lane now
    return _mm_cvtsi128_si32(z);
}

// Inclusive prefix sum
//   xout = simd_prefix_sum_u8(x)
//   xout[0] = x[0]
//   xout[1] = x[0] + x[1] = sum(x[0..1])
//   ...
//   xout[k] = sum(x[0..k])
static RADFORCEINLINE __m128i simd_prefix_sum_u8(__m128i x)
{
	x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
	x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
	x = _mm_add_epi8(x, _mm_slli_si128(x, 4));
	x = _mm_add_epi8(x, _mm_slli_si128(x, 8));
	return x;
}

#ifdef DO_SSE4_ALWAYS

// known SSE4 platform, just use it

#define _mm_mullo_epi32_sse2 _mm_mullo_epi32

#else

// _mm_mullo_epi32 is SSE4 , does 4x 32*32 -> 4x 32
// _mm_mul_epu32 is SSE2 , does 2x 32*32 -> 2x64
static RADINLINE __m128i _mm_mullo_epi32_sse2(const __m128i &a, const __m128i &b)
{
    __m128i mul20 = _mm_mul_epu32(a,b); // mul 2,0
    __m128i mul31 = _mm_mul_epu32( _mm_srli_si128(a,4), _mm_srli_si128(b,4)); // mul 3,1
    return _mm_unpacklo_epi32(
			_mm_shuffle_epi32(mul20, _MM_SHUFFLE (0,0,2,0)), 
			_mm_shuffle_epi32(mul31, _MM_SHUFFLE (0,0,2,0))); 
}

#endif

#elif defined(__RADNEON__)

// Inclusive prefix sum
//   xout = simd_prefix_sum_u8(x)
//   xout[0] = x[0]
//   xout[1] = x[0] + x[1] = sum(x[0..1])
//   ...
//   xout[k] = sum(x[0..k])
static RADFORCEINLINE uint8x16_t simd_prefix_sum_u8(uint8x16_t x)
{
	uint8x16_t zero = vdupq_n_u8(0);
	// Same algorithm as SSE2 version above.
	x = vaddq_u8(x, vextq_u8(zero, x, 16-1));
	x = vaddq_u8(x, vextq_u8(zero, x, 16-2));
	x = vaddq_u8(x, vextq_u8(zero, x, 16-4));
	x = vaddq_u8(x, vextq_u8(zero, x, 16-8));
	return x;
}

// horizontal sum
static RADINLINE S32 hsum_s32_neon(int32x4_t x)
{
	int32x2_t sum_pairs = vadd_s32(vget_low_s32(x), vget_high_s32(x)); // sum with elements 2 away
	int32x2_t final_sum = vpadd_s32(sum_pairs, sum_pairs); // sum with element 1 away
	return vget_lane_s32(final_sum, 0);
}

// Checks whether all lanes after a U8 compare are 0xff
static RADFORCEINLINE bool check_all_set_u8_neon(uint8x16_t compare_result)
{
	// kind of tortured way to test due to lack of a SSE2 PMOVMSKB equivalent.
	// we want 0xff in all lanes (and the lanes post-compare are either 0x00 or 0xff)
	// so sum groups of 4 lanes, then check wheter they're all -4
	uint8x8_t sum1 = vadd_u8(vget_low_u8(compare_result), vget_high_u8(compare_result)); // reduce 16 lanes -> 8
	uint8x8_t sum2 = vpadd_u8(sum1, sum1); // reduce 8 lanes -> 4
	return vget_lane_u32(vreinterpret_u32_u8(sum2), 0) == 0xfcfcfcfcu;
}

#endif

//=========

// _mm_cmpgt_epi8 is for signed char (S8)
//	need a cmp for U8 :
#define _mm_cmpge_epu8(a,b)	_mm_cmpeq_epi8( a, _mm_max_epu8(a,b))
#define _mm_cmple_epu8(a,b)	_mm_cmpge_epu8(b, a)

// GT is just the NOT of LE
//#define _mm_cmpgt_epu8(a,b) _mm_xor_si128(_mm_cmple_epu8(a,b), _mm_set1_epi8(-1))
//#define _mm_cmplt_epu8(a,b) _mm_cmpgt_epu8(b,a)

// alternative :
//#define _mm_cmpgt_epu8(a,b) _mm_cmpgt_epi8( _mm_add_epi8(a,_mm_set1_epi8(-0x80)) , _mm_add_epi8(b,_mm_set1_epi8(-0x80)) )
//#define _mm_cmplt_epu8(a,b) _mm_cmpgt_epu8(b,a)

//=========

// returns true if any value in [vec] is > vs
// needs to be inline in header so vec of "vs" can become a constant
//	(could pass that in if we had a generic vec mechanism)
static RADINLINE rrbool simd_cmpgt_u8(const U8 * vec, SINTa len, const int vs)
{

#ifdef __RADSSE2__

	// we want cmpgt, we're using cmpge, so do +1
	__m128i vs_plus1 = _mm_set1_epi8((char)(vs+1));

	SINTa i = 0;
	for(;(i+16)<=len;i+=16)
	{
		__m128i x = _mm_loadu_si128((__m128i const *)(vec+i));
		__m128i c = _mm_cmpge_epu8(x,vs_plus1); // x >= vs_plus1
		int m = _mm_movemask_epi8(c);
		if ( m )
			return true;
	}

	for(;i<len;i++)
	{
		if ( vec[i] > vs ) return true;
	}
	return false;

#else

	// @@ need NEON

	for (SINTa i=0;i<len;i++)
	{
		if ( vec[i] > vs ) return true;
	}
	return false;

#endif

}



OODLE_NS_END
