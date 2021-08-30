// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "oodlebase.h"
#include "rrmem.h"
#include "newlz_simd.h"
#ifdef __RADX86__
#include <emmintrin.h>
#endif

OODLE_NS_START

#ifdef __RADX86__

typedef __m128i Vec128;
typedef __m128i const & Vec128r;

static inline Vec128 load32u(const void *ptr)			{ return _mm_cvtsi32_si128(RR_GET32_NATIVE_UNALIGNED(ptr)); }
static inline Vec128 load64u(const void *ptr)			{ return _mm_loadl_epi64((const __m128i *)ptr); }
static inline Vec128 load128u(const void *ptr)			{ return _mm_loadu_si128((const __m128i *)ptr); }
static inline Vec128 load128a(const void *ptr)			{ return _mm_load_si128((const __m128i *)ptr); }
static inline void store32u(void *ptr, Vec128r x)		{ RR_PUT32_NATIVE_UNALIGNED(ptr, _mm_cvtsi128_si32(x)); }
static inline void store64u(void *ptr, Vec128r x)		{ _mm_storel_epi64((__m128i *)ptr, x); }
static inline void store128u(void *ptr, Vec128r x)		{ _mm_storeu_si128((__m128i *)ptr, x); }
static inline void store128a(void *ptr, Vec128r x)		{ _mm_store_si128((__m128i *)ptr, x); }

static inline Vec128 vec_packed_add(Vec128r a, Vec128r b, Vec128r mask_nonmsb)
{
	// sum low bits directly
	Vec128 low = _mm_add_epi64(_mm_and_si128(a, mask_nonmsb), _mm_and_si128(b, mask_nonmsb));
	// carryless sum (=XOR) in MSBs
	return _mm_xor_si128(low, _mm_andnot_si128(mask_nonmsb, _mm_xor_si128(a, b)));
}

static inline Vec128 zext8to16_lo(Vec128r x)			{ return _mm_unpacklo_epi8(x, _mm_setzero_si128()); }
static inline Vec128 zext8to16_hi(Vec128r x)			{ return _mm_unpackhi_epi8(x, _mm_setzero_si128()); }

static inline Vec128 zext16to32_lo(Vec128r x)			{ return _mm_unpacklo_epi16(x, _mm_setzero_si128()); }
static inline Vec128 zext16to32_hi(Vec128r x)			{ return _mm_unpackhi_epi16(x, _mm_setzero_si128()); }

static inline Vec128 sext16to32_lo(Vec128r x)			{ return _mm_srai_epi32(_mm_unpacklo_epi16(x, x), 16); }
static inline Vec128 sext16to32_hi(Vec128r x)			{ return _mm_srai_epi32(_mm_unpackhi_epi16(x, x), 16); }

template<int x,int y,int z,int w>
static inline Vec128 shuffle32(Vec128r v)				{ return _mm_shuffle_epi32(v, _MM_SHUFFLE(w,z,y,x)); }

template<int x,int y,int z,int w>
static inline Vec128 shuffle_two32(Vec128r a, Vec128r b){ return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b), _MM_SHUFFLE(w,z,y,x))); }

// some reduction steps
static inline Vec128 reduce_add_s32_2away(Vec128r x)	{ return _mm_add_epi32(x, shuffle32<2,3,0,1>(x)); }
static inline Vec128 reduce_min_u8_8away(Vec128r x)		{ return _mm_min_epu8(x,  shuffle32<2,3,0,1>(x)); }
static inline Vec128 reduce_max_u8_8away(Vec128r x)		{ return _mm_max_epu8(x,  shuffle32<2,3,0,1>(x)); }
static inline Vec128 reduce_add_s32_1away(Vec128r x)	{ return _mm_add_epi32(x, shuffle32<1,0,3,2>(x)); }
static inline Vec128 reduce_min_u8_4away(Vec128r x)		{ return _mm_min_epu8(x,  shuffle32<1,0,3,2>(x)); }
static inline Vec128 reduce_max_u8_4away(Vec128r x)		{ return _mm_max_epu8(x,  shuffle32<1,0,3,2>(x)); }

static inline S32 reduce_add_s32(Vec128r x)				{ return _mm_cvtsi128_si32(reduce_add_s32_1away(reduce_add_s32_2away(x))); }

// Float arithmetic is way less likely to be hopping between types all the time,
// so it's convenient to have a "batteries included" class with operator overloads
//
// The intent is just to simplify notation, it's written so it's trivial to come in
// and out of native ops so if what you want isn't in here and is contextual, just
// go for it.

struct VecF32x4;
typedef const VecF32x4 & VecF32x4r;

//operator __m128(VecF32x4r a) { return a.v; }

struct VecF32x4
{
	__m128 v;

	VecF32x4() {}
	VecF32x4(__m128 x) : v(x) {}
	explicit VecF32x4(F32 x) : v(_mm_set1_ps(x)) {}
	VecF32x4(F32 a, F32 b, F32 c, F32 d) : v(_mm_setr_ps(a, b, c, d)) {}

	// initialization and conversions
	static VecF32x4 zero()							{ return _mm_setzero_ps(); }
	static VecF32x4 from_int32(Vec128 x)			{ return _mm_cvtepi32_ps(x); }
	//static VecF32x4 from_int32(VecF32x4r x)			{ return _mm_cvtepi32_ps(x); }
	static VecF32x4 loadu(const float *ptr)			{ return _mm_loadu_ps(ptr); } // unaligned
	static VecF32x4 loada(const float *ptr)			{ return _mm_load_ps(ptr); } // 16-byte aligned
	static VecF32x4 load_scalar(const float *ptr)	{ return _mm_load_ss(ptr); }
	static VecF32x4 load_pair(const float *ptr)		{ return _mm_castpd_ps(_mm_load_sd((const double *)ptr)); }

	Vec128 to_int32_trunc()	const					{ return _mm_cvttps_epi32(v); }
	Vec128 to_int32_round()	const					{ return _mm_cvtps_epi32(v); }
	float scalar_x() const							{ float tmp; _mm_store_ss(&tmp, v); return tmp; }
	void storeu(float *ptr)	const					{ _mm_storeu_ps(ptr, v); } // unaligned
	void storea(float *ptr)	const					{ _mm_store_ps(ptr, v); } // 16-byte aligned

	// unary operations
	operator __m128() const							{ return v; }
	VecF32x4 operator -() const						{ return _mm_xor_ps(v, _mm_set1_ps(-0.0f)); }
//	VecF32x4 operator ~() const						{ return _mm_xor_ps(v, _mm_castsi128_ps(_mm_set1_epi32(-1))); }

	// binary assignment operators
	VecF32x4 &operator +=(VecF32x4r b)				{ v = _mm_add_ps(v, b); return *this; }
	VecF32x4 &operator -=(VecF32x4r b)				{ v = _mm_sub_ps(v, b); return *this; }
	VecF32x4 &operator *=(VecF32x4r b)				{ v = _mm_mul_ps(v, b); return *this; }
	VecF32x4 &operator /=(VecF32x4r b)				{ v = _mm_div_ps(v, b); return *this; }

	VecF32x4 &operator &=(VecF32x4r b)				{ v = _mm_and_ps(v, b); return *this; }
	VecF32x4 &operator |=(VecF32x4r b)				{ v = _mm_or_ps(v, b); return *this; }
	VecF32x4 &operator ^=(VecF32x4r b)				{ v = _mm_xor_ps(v, b); return *this; }

	// comparisons are not as operator overloads because they return a mask vector, not a bool
	VecF32x4 cmp_gt(VecF32x4r b) const				{ return _mm_cmpgt_ps(v, b); } // >
	VecF32x4 cmp_ge(VecF32x4r b) const				{ return _mm_cmpge_ps(v, b); } // >=
	VecF32x4 cmp_lt(VecF32x4r b) const				{ return _mm_cmplt_ps(v, b); } // <
	VecF32x4 cmp_le(VecF32x4r b) const				{ return _mm_cmple_ps(v, b); } // <=
	VecF32x4 cmp_eq(VecF32x4r b) const				{ return _mm_cmpeq_ps(v, b); } // ==
	VecF32x4 cmp_ngt(VecF32x4r b) const				{ return _mm_cmpngt_ps(v, b); } // ! >
	VecF32x4 cmp_nge(VecF32x4r b) const				{ return _mm_cmpnge_ps(v, b); } // ! >=
	VecF32x4 cmp_nlt(VecF32x4r b) const				{ return _mm_cmpnlt_ps(v, b); } // ! <
	VecF32x4 cmp_nle(VecF32x4r b) const				{ return _mm_cmpnle_ps(v, b); } // ! <=
	VecF32x4 cmp_neq(VecF32x4r b) const				{ return _mm_cmpneq_ps(v, b); } // ! ==

	// shuffles
	template<int x, int y, int z, int w>
	VecF32x4 shuf() const							{ return _mm_shuffle_ps(v, v, _MM_SHUFFLE(w,z,y,x)); }

	// cyclic permutations of elements
	VecF32x4 yzwx() const							{ return shuf<1,2,3,0>(); }
	VecF32x4 wxyz() const							{ return shuf<3,0,1,2>(); }

	// cyclic permutations of first 3 elements
	VecF32x4 yzxw() const							{ return shuf<1,2,0,3>(); }
	VecF32x4 zxyw() const							{ return shuf<2,0,1,3>(); }

	// blockwise swaps
	VecF32x4 yxwz() const							{ return shuf<1,0,3,2>(); }
	VecF32x4 zwxy() const							{ return shuf<2,3,0,1>(); }

	// broadcasts
	VecF32x4 xxxx() const							{ return shuf<0,0,0,0>(); }
	VecF32x4 yyyy() const							{ return shuf<1,1,1,1>(); }
	VecF32x4 zzzz() const							{ return shuf<2,2,2,2>(); }
	VecF32x4 wwww() const							{ return shuf<3,3,3,3>(); }

	// utils
	VecF32x4 square() const							{ return _mm_mul_ps(v, v); }

	// some reductions
	VecF32x4 sum_across() const						{ VecF32x4 t = _mm_add_ps(*this, zwxy()); return _mm_add_ps(t, t.yxwz()); }
};

static inline VecF32x4 operator +(VecF32x4r a, VecF32x4r b)		{ return _mm_add_ps(a, b); }
static inline VecF32x4 operator -(VecF32x4r a, VecF32x4r b)		{ return _mm_sub_ps(a, b); }
static inline VecF32x4 operator *(VecF32x4r a, VecF32x4r b)		{ return _mm_mul_ps(a, b); }
static inline VecF32x4 operator /(VecF32x4r a, VecF32x4r b)		{ return _mm_div_ps(a, b); }

static inline VecF32x4 operator &(VecF32x4r a, VecF32x4r b)		{ return _mm_and_ps(a, b); }
static inline VecF32x4 operator |(VecF32x4r a, VecF32x4r b)		{ return _mm_or_ps(a, b); }
static inline VecF32x4 operator ^(VecF32x4r a, VecF32x4r b)		{ return _mm_xor_ps(a, b); }
static inline VecF32x4 andnot(VecF32x4r a, VecF32x4r b)			{ return _mm_andnot_ps(b, a); } // a & ~b (mm_andnot is ~a & b)

static inline VecF32x4 vmin(VecF32x4r a, VecF32x4r b)			{ return _mm_min_ps(a, b); }
static inline VecF32x4 vmax(VecF32x4r a, VecF32x4r b)			{ return _mm_max_ps(a, b); }

static inline VecF32x4 VecF32x4r_sqrt(VecF32x4r a)						{ return _mm_sqrt_ps(a); }

template<int x,int y,int z,int w>
static inline VecF32x4 shuffle_two32(VecF32x4r a, VecF32x4r b)	{ return _mm_shuffle_ps(a, b, _MM_SHUFFLE(w,z,y,x)); }

#endif

OODLE_NS_END

