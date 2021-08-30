// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4

#include "bc67format.h"
#include "bc67tables.h"
#include "bc7decode_fast.h"
#include "bc7bits.h"
#include "cpux86.h"
#include "rrbits.h"
#include "newlz_simd.h"
#include "vec128.inl"
#include "cbradutil.h"
#include "oodlemalloc.h"

#ifdef DO_BUILD_SSE4
#include <smmintrin.h>
#endif

OODLE_NS_START

// For two-subset partitions, one partition representative each for one of the
// 4 equivalence classes wrt anchor pairs (see radtex_subset_anchors[])
static const BC67Partition * two_subset_repr[4] =
{
	&bc67_partitions[1 +  0], // class 0 (second anchor is 15)
	&bc67_partitions[1 + 17], // class 1 (second anchor is 2)
	&bc67_partitions[1 + 18], // class 2 (second anchor is 8)
	&bc67_partitions[1 + 34], // class 3 (second anchor is 6)
};

#ifdef DO_BUILD_SSE4

#include "bc7decode_fast_common.inl"

static RADFORCEINLINE void sse4_interpolate_one_subset_twoind(U8 * out_rgba, U32 crot_idxmode, const U8 * lerpf_buf, const Vec128 &endpoints16_in)
{
	// Determine channel rotate shuffle
	Vec128 crot_shuf = sse4_channel_rotate16(crot_idxmode & 3);

	// Apply to endpoints
	Vec128 endpoints16 = _mm_shuffle_epi8(endpoints16_in, crot_shuf);

	// Grab the 8 lerp factors we'll need and replicate them in the pattern
	// we need, stretching them out to 16 bits by left-shifting by 8
	// along the way. Also apply channel rotate.
	static RAD_ALIGN(const S8, lerpf16_shuffles[8][16], 16) =
	{
		// idxMode=0 (no index swap)
		{ -16,0, -16,0, -16,0, -16,1,  -16,2, -16,2, -16,2, -16,3 }, // rot=0: no change
		{ -16,1, -16,0, -16,0, -16,0,  -16,3, -16,2, -16,2, -16,2 }, // rot=1: swap R<->A
		{ -16,0, -16,1, -16,0, -16,0,  -16,2, -16,3, -16,2, -16,2 }, // rot=2: swap G<->A
		{ -16,0, -16,0, -16,1, -16,0,  -16,2, -16,2, -16,3, -16,2 }, // rot=3: swap B<->A

		// idxMode=1 (index swap)
		{ -16,1, -16,1, -16,1, -16,0,  -16,3, -16,3, -16,3, -16,2 }, // rot=0: no change
		{ -16,0, -16,1, -16,1, -16,1,  -16,2, -16,3, -16,3, -16,3 }, // rot=1: swap R<->A
		{ -16,1, -16,0, -16,1, -16,1,  -16,3, -16,2, -16,3, -16,3 }, // rot=2: swap G<->A
		{ -16,1, -16,1, -16,0, -16,1,  -16,3, -16,3, -16,2, -16,3 }, // rot=3: swap B<->A
	};
	Vec128 lerpf16_shuf0 = load128a(lerpf16_shuffles[crot_idxmode]);
	Vec128 lerpf16_shuf1 = _mm_add_epi8(lerpf16_shuf0, _mm_set1_epi8(4));

	// Shuffle the endpoints around so we have the "low" and "high" end separated out
	Vec128 lo16 = _mm_unpacklo_epi64(endpoints16, endpoints16);
	Vec128 hi16 = _mm_unpackhi_epi64(endpoints16, endpoints16);

	// Set up for interpolation
	Vec128 diff16 = _mm_sub_epi16(lo16, hi16); // yes, lo-hi!

	// Interpolate the pixel values!
	for (SINTa i = 0; i < 4; ++i)
	{
		// Grab lerp factors
		Vec128 lerpf8 = load64u(lerpf_buf + i*8);
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf0);
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf1);

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, diff16), lo16);
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, diff16), lo16);

		// Pack down to 8 bits, perform channel rotate and store
		Vec128 interp8 = _mm_packus_epi16(interp16_0, interp16_1);
		store128u(out_rgba + i * 16, interp8);
	}
}

static RADFORCEINLINE void sse4_interpolate_two_subset(U8 * out_rgba, const BC67Partition * partition, const Vec128 &lerpf8, const Vec128 &lo16, const Vec128 &hi16)
{
	// Set up for interpolation
	Vec128 diff16 = _mm_sub_epi16(lo16, hi16); // yes, lo-hi!

	// lo16/diff16 diffs between the subsets
	// we use math to select subsets and not blends/shuffles because we are
	// already doing a ton of those and Haswell through Skylake only have
	// one shuffle port
	Vec128 lo16_subset0 = shuffle32<0,1,0,1>(lo16);
	Vec128 lo16_subset1 = _mm_sub_epi16(shuffle32<2,3,2,3>(lo16), lo16_subset0);
	Vec128 diff16_subset0 = shuffle32<0,1,0,1>(diff16);
	Vec128 diff16_subset1 = _mm_xor_si128(shuffle32<2,3,2,3>(diff16), diff16_subset0);

	// Shuffle for low/high lerp factors
	// replicates them 4x each and also expands to 16 bits by left-shifting by 8
	// -16 here because we keep adding to this value
	Vec128 lerpf16_shuf = _mm_setr_epi8(-16,0, -16,0, -16,0, -16,0, -16,1, -16,1, -16,1, -16,1);
	const Vec128 lerpf16_shuf_incr = _mm_set1_epi8(2);

	// Subset mask for the pixels
	Vec128 subset_mask = _mm_cvtsi32_si128(partition->subset_mask);
	subset_mask = _mm_unpacklo_epi64(subset_mask, subset_mask);

	const Vec128 in_subset1_mask_0 = _mm_setr_epi32(1<<0, 0, 1<<2, 0);
	const Vec128 in_subset1_mask_1 = _mm_setr_epi32(1<<4, 0, 1<<6, 0);

	// Interpolate the pixel values!
	for (SINTa i = 0; i < 64; i += 16)
	{
		// Determine 64-bit masks for whether a pixel is in subset 1
		Vec128 in_subset1_0 = _mm_cmpeq_epi64(_mm_and_si128(subset_mask, in_subset1_mask_0), in_subset1_mask_0);
		Vec128 in_subset1_1 = _mm_cmpeq_epi64(_mm_and_si128(subset_mask, in_subset1_mask_1), in_subset1_mask_1);

		// Replicate the lerp factors 4x each and expand to 16 bits by shifting
		// left by 8
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf);
		lerpf16_shuf = _mm_add_epi8(lerpf16_shuf, lerpf16_shuf_incr);
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf);
		lerpf16_shuf = _mm_add_epi8(lerpf16_shuf, lerpf16_shuf_incr);

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		Vec128 diff16_0 = _mm_xor_si128(diff16_subset0, _mm_and_si128(diff16_subset1, in_subset1_0));
		Vec128 diff16_1 = _mm_xor_si128(diff16_subset0, _mm_and_si128(diff16_subset1, in_subset1_1));
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, diff16_0), lo16_subset0);
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, diff16_1), lo16_subset0);

		interp16_0 = _mm_add_epi16(interp16_0, _mm_and_si128(lo16_subset1, in_subset1_0));
		interp16_1 = _mm_add_epi16(interp16_1, _mm_and_si128(lo16_subset1, in_subset1_1));

		// Pack down to 8 bits and store
		Vec128 interp8 = _mm_packus_epi16(interp16_0, interp16_1);
		store128u(out_rgba + i, interp8);

		// Advance to the next group of 4 pixels
		subset_mask = _mm_srli_epi64(subset_mask, 8);
	}
}

// Expects lo8 = (R0,R2,R4, G0,G2,G4, B0,B2,B4, A0,A2,A4, <ignored>)
// and     hi8 = (R1,R3,R5, G1,G3,G5, B1,B3,B5, A1,A3,A5, <ignored>)
static RADFORCEINLINE void sse4_interpolate_three_subset(U8 * out_rgba, const BC67Partition * partition, const Vec128 &lerpf8, const Vec128 &lo8, const Vec128 &hi8)
{
	// Shuffle for low/high lerp factors
	// replicates them 4x each and also expands to 16 bits by left-shifting by 8
	// -16 here because we keep adding to this value
	Vec128 lerpf16_shuf = _mm_setr_epi8(-16,0, -16,0, -16,0, -16,0, -16,1, -16,1, -16,1, -16,1);
	const Vec128 lerpf16_shuf_incr = _mm_set1_epi8(2);

	// Subset mask for the pixels
	Vec128 subset_mask = _mm_set1_epi32(partition->subset_mask);
	const Vec128 subset_ind_mask = _mm_setr_epi32(3<<0, 3<<2, 3<<4, 3<<6);

	// Interpolate the pixel values!
	for (SINTa i = 0; i < 64; i += 16)
	{
		// Isolate next 4 subset indices
		Vec128 subset_inds = _mm_and_si128(subset_mask, subset_ind_mask);

		// Shift to place in second byte of every DWord
		Vec128 subset_inds_scaled = _mm_mullo_epi16(subset_inds, _mm_setr_epi16(1<<8, 0, 1<<6, 0, 1<<4, 0, 1<<2, 0));

		// Determine endpoint shuffle; we have the subset index, which we now need to replicate and then add the offset for the channel
		Vec128 endpoint_shuf = _mm_shuffle_epi8(subset_inds_scaled, _mm_setr_epi8(1,1,1,1, 5,5,5,5, 9,9,9,9, 13,13,13,13));
		endpoint_shuf = _mm_add_epi8(endpoint_shuf, _mm_set1_epi32(0x09060300));

		// Grab 4x pixels worth of RGBA lo8/hi8
		Vec128 pix_lo8 = _mm_shuffle_epi8(lo8, endpoint_shuf);
		Vec128 pix_hi8 = _mm_shuffle_epi8(hi8, endpoint_shuf);

		// Then expand to 16 bits
		Vec128 lo16_0 = zext8to16_lo(pix_lo8);
		Vec128 lo16_1 = zext8to16_hi(pix_lo8);
		Vec128 hi16_0 = zext8to16_lo(pix_hi8);
		Vec128 hi16_1 = zext8to16_hi(pix_hi8);

		// Set up for interpolation (lo-hi again)
		Vec128 diff16_0 = _mm_sub_epi16(lo16_0, hi16_0);
		Vec128 diff16_1 = _mm_sub_epi16(lo16_1, hi16_1);

		// Replicate the lerp factors 4x each and expand to 16 bits by shifting
		// left by 8
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf);
		lerpf16_shuf = _mm_add_epi8(lerpf16_shuf, lerpf16_shuf_incr);
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf);
		lerpf16_shuf = _mm_add_epi8(lerpf16_shuf, lerpf16_shuf_incr);

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, diff16_0), lo16_0);
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, diff16_1), lo16_1);

		// Pack down to 8 bits and store
		Vec128 interp8 = _mm_packus_epi16(interp16_0, interp16_1);
		store128u(out_rgba + i, interp8);

		// Advance to the next group of 4 pixels
		subset_mask = _mm_srli_epi32(subset_mask, 8);
	}
}

static void sse4_decode_mode0(U8 * out_rgba, const U8 * block_bits)
{
	//       [0]  mode (1b)
	//     [4:1]  partition (4b)
	//    [28:5]  R0..R5 (4b each)
	//   [52:29]  G0..G5 (4b each)
	//   [76:53]  B0..B5 (4b each)
	//   [82:77]  P0..P5 (1b each)
	//  [127:83]  index (45b)
	RR_ASSERT((block_bits[0] & 0x1) == 1); // mode 0

	Vec128 block128 = load128u(block_bits);

	Vec128 lo8, hi8;
	sse4_decode_mode0_endpoints(&lo8, &hi8, block128, block_bits);

	// Decode partition type and indices
	const BC67Partition * partition = &bc67_partitions[1 + 64 + ((block_bits[0] >> 1) & 0xf)];

	Vec128 lerpf8 = sse4_decode_mode0_inds(block128, partition);

	// And interpolate
	sse4_interpolate_three_subset(out_rgba, partition, lerpf8, lo8, hi8);
}

static void sse4_decode_mode1(U8 * out_rgba, const U8 * block_bits)
{
	//     [1:0]  mode (2b)
	//     [7:2]  partition (6b)
	//    [31:8]  R0..R3 (6b each)
	//   [55:32]  G0..G3 (6b each)
	//   [79:56]  B0..B3 (6b each)
	//   [81:80]  P0..P1 (1b each)
	//  [127:82]  index (46b)
	RR_ASSERT((block_bits[0] & 0x3) == 2); // mode 1

	Vec128 block128 = load128u(block_bits);

	Vec128 lo16, hi16;
	sse4_decode_mode1_endpoints(&lo16, &hi16, block128, block_bits);

	// Decode partition type
	const BC67Partition * partition = &bc67_partitions[1 + (block_bits[0] >> 2)];

	Vec128 lerpf8 = sse4_decode_mode1_inds(block128, partition);

	// And interpolate!
	sse4_interpolate_two_subset(out_rgba, partition, lerpf8, lo16, hi16);
}

static void sse4_decode_mode2(U8 * out_rgba, const U8 * block_bits)
{
	//     [2:0]  mode (3b)
	//     [8:3]  partition (6b)
	//    [38:9]  R0..R5 (5b each)
	//   [68:39]  G0..G5 (5b each)
	//   [98:69]  B0..B5 (5b each)
	//  [127:99]  index (29b)
	RR_ASSERT((block_bits[0] & 0x7) == 4); // mode 2

	Vec128 block128 = load128u(block_bits);

	Vec128 lo8, hi8;
	sse4_decode_mode2_endpoints(&lo8, &hi8, block128, block_bits);

	// Decode partition type and indices
	U16 partition_ind = (RR_GET16_LE_UNALIGNED(block_bits) >> 3) & 0x3f;
	const BC67Partition * partition = &bc67_partitions[1 + 64 + partition_ind];

	Vec128 lerpf8 = sse4_decode_mode2_inds(block128, partition);

	// And interpolate!
	sse4_interpolate_three_subset(out_rgba, partition, lerpf8, lo8, hi8);
}

static void sse4_decode_mode3(U8 * out_rgba, const U8 * block_bits)
{
	//     [3:0]  mode (4b)
	//     [9:4]  partition (6b)
	//   [37:10]  R0..R3 (7b each)
	//   [65:38]  G0..G3 (7b each)
	//   [93:66]  B0..B3 (7b each)
	//   [97:94]  P0..P3 (1b each)
	//  [127:98]  index (30b)
	RR_ASSERT((block_bits[0] & 0xf) == 8); // mode 3

	Vec128 block128 = load128u(block_bits);

	Vec128 lo16, hi16;
	sse4_decode_mode3_endpoints(&lo16, &hi16, block128, block_bits);

	// Decode partition type and indices
	const BC67Partition * partition = &bc67_partitions[1 + (block_bits[0] >> 4) + (block_bits[1] & 3) * 16];
	Vec128 lerpf8 = sse4_decode_mode3_inds(block128, partition);

	// And interpolate!
	sse4_interpolate_two_subset(out_rgba, partition, lerpf8, lo16, hi16);
}

static void sse4_decode_mode4(U8 * out_rgba, const U8 * block_bits)
{
	//     [4:0]  mode (5b)
	//     [6:5]  channel rot (2b)
	//     [7:7]  idxMode
	//    [17:8]  R0..R1 (5b each)
	//   [27:18]  G0..G1 (5b each)
	//   [37:28]  B0..B1 (5b each)
	//   [49:38]  A0..A1 (6b each)
	//   [80:50]  index0 (31b)
	//  [127:81]  index1 (47b)
	RR_ASSERT((block_bits[0] & 0x1f) == 0x10); // mode 4

	// First we need to unpack the endpoints to 8 bits each
	Vec128 block128 = load128u(block_bits);

	RAD_ALIGN(U8, lerpf_buf[32], 16);
	sse4_decode_mode4_inds(lerpf_buf, block128);

	Vec128 endpoints16 = sse4_decode_mode4_endpoints(block128);

	sse4_interpolate_one_subset_twoind(out_rgba, block_bits[0] >> 5, lerpf_buf, endpoints16);
}

static void sse4_decode_mode5(U8 * out_rgba, const U8 * block_bits)
{
	//     [5:0]  mode (6b)
	//     [7:6]  channel rot (2b)
	//    [21:8]  R0..R1 (7b each)
	//   [35:22]  G0..G1 (7b each)
	//   [49:36]  B0..B1 (7b each)
	//   [65:50]  A0..A1 (8b each)
	//   [96:66]  index0 (31b)
	//  [127:97]  index1 (31b)
	RR_ASSERT((block_bits[0] & 0x3f) == 0x20); // mode 5

	Vec128 block128 = load128u(block_bits);

	RAD_ALIGN(U8, lerpf_buf[32], 16);
	sse4_decode_mode5_inds(lerpf_buf, block128);

	Vec128 endpoints16 = sse4_decode_mode5_endpoints(block128);

	sse4_interpolate_one_subset_twoind(out_rgba, block_bits[0] >> 6, lerpf_buf, endpoints16);
}

static void sse4_decode_mode6(U8 * out_rgba, const U8 * block_bits)
{
	//     [6:0]  mode (7b)
	//    [20:7]  R0..R1 (7b each)
	//   [34:21]  G0..G1 (7b each)
	//   [48:35]  B0..B1 (7b each)
	//   [62:49]  A0..A1 (7b each)
	//   [64:63]  P0..P1 (1b each)
	//  [127:65]  index (63b)
	RR_ASSERT((block_bits[0] & 0x7f) == 0x40); // mode 6

	// First we need to unpack the endpoints to 8 bits each
	Vec128 block128 = load128u(block_bits + 0);

	// Decode the endpoints
	Vec128 endpoints = sse4_decode_mode6_endpoints(block128);

	// Shuffle the endpoints around so we have the "low" and "high" end separated out
	Vec128 lo16 = _mm_unpacklo_epi64(endpoints, endpoints);
	Vec128 hi16 = _mm_unpackhi_epi64(endpoints, endpoints);

	// Set up for interpolation
	Vec128 diff16 = _mm_sub_epi16(lo16, hi16); // yes, lo-hi!
	Vec128 lerpf8 = sse4_decode_mode6_inds(block128);

	// Shuffle for low/high lerp factors
	// replicates them 4x each and also expands to 16 bits by left-shifting by 8
	// -16 here because we keep adding to this value
	Vec128 lerpf16_shuf0 = _mm_setr_epi8(-16,0, -16,0, -16,0, -16,0, -16,1, -16,1, -16,1, -16,1);
	Vec128 lerpf16_shuf1 = _mm_setr_epi8(-16,2, -16,2, -16,2, -16,2, -16,3, -16,3, -16,3, -16,3);
	const Vec128 lerpf16_shuf_incr = _mm_set1_epi8(4);

	// Interpolate the pixel values!
	for (SINTa i = 0; i < 64; i += 16)
	{
		// Replicate the lerp factors 4x each and expand to 16 bits by shifting
		// left by 8
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf0);
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf1);

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, diff16), lo16);
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, diff16), lo16);

		// Pack down to 8 bits and store
		Vec128 interp8 = _mm_packus_epi16(interp16_0, interp16_1);
		store128u(out_rgba + i, interp8);

		// Advance to the next group of 4 lerp factors
		lerpf16_shuf0 = _mm_add_epi8(lerpf16_shuf0, lerpf16_shuf_incr);
		lerpf16_shuf1 = _mm_add_epi8(lerpf16_shuf1, lerpf16_shuf_incr);
	}
}

static void sse4_decode_mode7(U8 * out_rgba, const U8 * block_bits)
{
	//     [7:0]  mode (8b)
	//    [13:8]  partition (6b)
	//   [33:14]  R0..R3 (5b each)
	//   [53:34]  G0..G3 (5b each)
	//   [73:54]  B0..B3 (5b each)
	//   [93:74]  A0..A3 (5b each)
	//   [97:94]  P0..P3 (1b each)
	//  [127:98]  index (30b)
	RR_ASSERT(block_bits[0] == 0x80); // mode 7

	Vec128 block128 = load128u(block_bits);

	Vec128 lo16, hi16;
	sse4_decode_mode7_endpoints(&lo16, &hi16, block128, block_bits);

	// Decode partition type and indices
	const BC67Partition * partition = &bc67_partitions[1 + (block_bits[1] & 63)];
	Vec128 lerpf8 = sse4_decode_mode7_inds(block128, partition);

	// And interpolate!
	sse4_interpolate_two_subset(out_rgba, partition, lerpf8, lo16, hi16);
}

#endif // DO_BUILD_SSE4

static void bc7_decode_verify(const U8 * decoded, const void * block)
{
	U8 ref_result[64];
	bc7_decode_block(ref_result, 16, block);
	RR_ASSERT_ALWAYS(memcmp(decoded, ref_result, 64) == 0);
}

static void sse4_decode_malformed(U8 * out_rgba, const U8 * block_bits)
{
	memset(out_rgba, 0, 64);
}

#if 0
#define VERIFY(decoded,block) bc7_decode_verify(decoded,block)
#else
#define VERIFY(decoded,block)
#endif

typedef void BC7BlockDecoder(U8 * out_rgba, const U8 * block_bits);

// The main dispatcher
void bc7_decode_block_fast(U8 * out_rgba, const void * block)
{
	const U8 * block_bytes = (const U8 *)block;

#ifdef DO_BUILD_SSE4
	U32 mode = rrCtz32(block_bytes[0] | 0x100); // gives 8 when byte=zero

#ifdef DO_BUILD_AVX2
	if ( rrCPUx86_feature_present(RRX86_CPU_AVX2) )
	{
		static BC7BlockDecoder * const decoders_avx2[9] =
		{
			sse4_decode_mode0,
			bc7decode::avx2_decode_mode1,
			sse4_decode_mode2,
			bc7decode::avx2_decode_mode3,
			bc7decode::avx2_decode_mode4,
			bc7decode::avx2_decode_mode5,
			bc7decode::avx2_decode_mode6,
			bc7decode::avx2_decode_mode7,
			sse4_decode_malformed
		};

		decoders_avx2[mode](out_rgba, block_bytes);
		VERIFY(out_rgba, block);

		return;
	}
#endif

	static BC7BlockDecoder * const decoders_sse4[9] =
	{
		sse4_decode_mode0,
		sse4_decode_mode1,
		sse4_decode_mode2,
		sse4_decode_mode3,
		sse4_decode_mode4,
		sse4_decode_mode5,
		sse4_decode_mode6,
		sse4_decode_mode7,
		sse4_decode_malformed
	};

	decoders_sse4[mode](out_rgba, block_bytes);
	VERIFY(out_rgba, block);
#else
	// If all else fails, we can always use the reference decoder.
	// (Never wrong, but generally much slower.)
	bc7_decode_block(out_rgba, 16, block_bytes);
#endif
}

// ---- Block error eval

static U32 endpoints_eval_scalar_basic(const BC7PredecodedEndpointsBlock * block, SINTa inds_from)
{
	bc7bits endpts_bits = bc7bits_load(block->endpt_block);
	bc7bits inds_bits = bc7bits_and(bc7bits_load(block->index_cache + inds_from * 16), bc7bits_load(block->index_mask));
	bc7bits block_bits = bc7bits_or_assert_exclusive(endpts_bits, inds_bits);

	U8 out_rgba[64];
	bc7_decode_block_fast(out_rgba, bc7bits_U8ptr(&block_bits));

	if (block->ignore_alpha)
	{
		// set all alphas in decoded block to 255
		for (int i = 0; i < 64; i += 4)
			out_rgba[i + 3] = 255;
	}

	U32 total_err = 0;
	for LOOP(i,64)
	{
		int diff = out_rgba[i] - block->base[i];
		total_err += diff * diff;
	}

	return total_err;
}

static SINTa find_next_at_most_scalar_basic(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	for (SINTa i = 0; i < count; ++i)
	{
		U32 ssd = endpoints_eval_scalar_basic(b, inds_from + i);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

#ifdef DO_BUILD_SSE4

static U32 endpoints_eval_sse4_basic(const BC7PredecodedEndpointsBlock * block, SINTa inds_from)
{
	bc7bits endpts_bits = bc7bits_load(block->endpt_block);
	bc7bits inds_bits = bc7bits_and(bc7bits_load(block->index_cache + inds_from * 16), bc7bits_load(block->index_mask));
	bc7bits block_bits = bc7bits_or_assert_exclusive(endpts_bits, inds_bits);

	U8 out_rgba[64];
	bc7_decode_block_fast(out_rgba, bc7bits_U8ptr(&block_bits));

	Vec128 alpha_override = block->ignore_alpha ? _mm_set1_epi32(-0x01000000) : _mm_setzero_si128();

	Vec128 total_err = _mm_setzero_si128();
	for (SINTa i = 0; i < 64; i += 16)
	{
		// Load decoded bytes
		Vec128 rgba_u8 = load128u(out_rgba + i);
		rgba_u8 = _mm_or_si128(rgba_u8, alpha_override);

		// Expand to 16 bits
		Vec128 rgba_u16_0 = zext8to16_lo(rgba_u8);
		Vec128 rgba_u16_1 = zext8to16_hi(rgba_u8);

		// Diff against the already-expanded reference pixels in "base"
		Vec128 diff_s16_0 = _mm_sub_epi16(rgba_u16_0, load128u(block->base + i));
		Vec128 diff_s16_1 = _mm_sub_epi16(rgba_u16_1, load128u(block->base + i + 8));

		// First half of dot product: R*R + G*G, B*B + A*A
		// the rest we do as part of the reduction
		Vec128 err0 = _mm_madd_epi16(diff_s16_0, diff_s16_0);
		Vec128 err1 = _mm_madd_epi16(diff_s16_1, diff_s16_1);

		// Accumulate
		Vec128 errs = _mm_add_epi32(err0, err1);
		total_err = _mm_add_epi32(total_err, errs);
	}

	// Finish the reduction
	return reduce_add_s32(total_err);
}

static SINTa find_next_at_most_sse4_basic(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	for (SINTa i = 0; i < count; ++i)
	{
		U32 ssd = endpoints_eval_scalar_basic(b, inds_from + i);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static RADFORCEINLINE U32 endpoints_eval_finish_sse4_oneindex(const BC7PredecodedEndpointsBlock * b, const Vec128 &lerpf8)
{
	// Shuffle for low/high lerp factors
	// replicates them 4x each and also expands to 16 bits by left-shifting by 8
	static RAD_ALIGN(const S8, lerpf16_shuffles[128], 16) =
	{
#define FOUR(x) -1,(x),-1,(x),-1,(x),-1,(x)
		FOUR( 0), FOUR( 1), FOUR( 2), FOUR( 3),
		FOUR( 4), FOUR( 5), FOUR( 6), FOUR( 7),
		FOUR( 8), FOUR( 9), FOUR(10), FOUR(11),
		FOUR(12), FOUR(13), FOUR(14), FOUR(15),
#undef FOUR
	};

	Vec128 total_err = _mm_setzero_si128();
	for (SINTa i = 0; i < 64; i += 16)
	{
		// Replicate the lerp factors 4x each and expand to 16 bits by shifting left by 8
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, load128a(lerpf16_shuffles + i*2));
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, load128a(lerpf16_shuffles + i*2 + 16));

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		// also computes diff to original block values which are subtracted from base
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, load128a(b->diff + i + 0)), load128a(b->base + i + 0));
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, load128a(b->diff + i + 8)), load128a(b->base + i + 8));

		// First half of dot product: R*R + G*G, B*B + A*A
		// the rest we do as part of the reduction
		Vec128 err_0 = _mm_madd_epi16(interp16_0, interp16_0);
		Vec128 err_1 = _mm_madd_epi16(interp16_1, interp16_1);
		Vec128 err_sum = _mm_add_epi32(err_0, err_1);
		total_err = _mm_add_epi32(total_err, err_sum);
	}

	// Finish the reduction
	return reduce_add_s32(total_err);
}

static RADFORCEINLINE U32 endpoints_eval_finish_sse4_twoindex(const BC7PredecodedEndpointsBlock * b, const U8 * lerpf_buf, bool index_swap)
{
	// Shuffle the interleaved lerp factors to replicate them in the 3-1 scalar-vector pattern
	// we always work with values in the pre-color-rotate RGB=vector, A=scalar space
	static RAD_ALIGN(const S8, lerpf16_shuffles[2][32], 16) = // [index_swap][i]
	{
#define INDS(x,y) -1,(x),-1,(x),-1,(x),-1,(y)
		{ // index_swap=false
			INDS(0,1), INDS(2,3), INDS(4,5), INDS(6,7),
		},
		{ // index_swap=true
			INDS(1,0), INDS(3,2), INDS(5,4), INDS(7,6),
		},
#undef INDS
	};

	// Shuffle the interleaved lerp factors to replicate them in the 3-1 scalar-vector pattern
	// we always work with values in the pre-color-rotate RGB=vector, A=scalar space
	const Vec128 lerpf16_shuf_0 = load128a(lerpf16_shuffles[index_swap] + 0);
	const Vec128 lerpf16_shuf_1 = load128a(lerpf16_shuffles[index_swap] + 16);

	Vec128 total_err = _mm_setzero_si128();
	for (SINTa i = 0; i < 16; i += 4)
	{
		Vec128 lerpf8 = load64u(lerpf_buf + i*2);
		Vec128 lerpf16_0 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf_0);
		Vec128 lerpf16_1 = _mm_shuffle_epi8(lerpf8, lerpf16_shuf_1);

		// Interpolate via rounding_shift_right(neg_factor * (lo - hi), 15) + lo
		// also computes diff to original block values which are subtracted from base
		Vec128 interp16_0 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_0, load128a(b->diff + i*4 + 0)), load128a(b->base + i*4 + 0));
		Vec128 interp16_1 = _mm_add_epi16(_mm_mulhrs_epi16(lerpf16_1, load128a(b->diff + i*4 + 8)), load128a(b->base + i*4 + 8));

		// First half of dot product: R*R + G*G, B*B + A*A
		// the rest we do as part of the reduction
		Vec128 err_0 = _mm_madd_epi16(interp16_0, interp16_0);
		Vec128 err_1 = _mm_madd_epi16(interp16_1, interp16_1);
		Vec128 err_sum = _mm_add_epi32(err_0, err_1);
		total_err = _mm_add_epi32(total_err, err_sum);
	}

	// Finish the reduction
	return reduce_add_s32(total_err);
}

static U32 endpoints_eval_sse4_onesubset_twoind(const BC7PredecodedEndpointsBlock * b, SINTa inds_from)
{
	return endpoints_eval_finish_sse4_twoindex(b, b->index_cache + inds_from * 32, b->index_swap != 0);
}

static SINTa find_next_at_most_sse4_onesubset_twoind(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	bool index_swap = b->index_swap != 0;

	for (SINTa i = 0; i < count; ++i)
	{
		U32 ssd = endpoints_eval_finish_sse4_twoindex(b, b->index_cache + (inds_from + i) * 32, index_swap);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static void preendpoint_init_sse4_twoind(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const Vec128 &endpoints16, U32 crot, bool ignore_alpha_chan)
{
	// Set up decode kernel pointers
#ifdef DO_BUILD_AVX2
	if (rrCPUx86_feature_present(RRX86_CPU_AVX2))
	{
		b->eval = bc7decode::endpoints_eval_avx2_onesubset_twoind;
		b->find_next_at_most = bc7decode::find_next_at_most_avx2_onesubset_twoind;
	}
	else
#endif
	{
		b->eval = endpoints_eval_sse4_onesubset_twoind;
		b->find_next_at_most = find_next_at_most_sse4_onesubset_twoind;
	}

	// For the modes with two indices, we stay fully in the space that BC7 decodes into,
	// where the vector channels are always RGB and the scalar channel is always A.
	//
	// Instead of having the decoder apply the final channel swap at the end, we apply
	// the inverse swap to the "base" values we diff against - comes out to the same
	// result, but we never have to actually swap anything in the decoder.

	// The channel "rotates" are actually always swaps, so idempotent:
	// the swap and its inverse are the same.
	Vec128 crot_shuf = sse4_channel_rotate16(crot);
	Vec128 endpoints = endpoints16;

	// In "ignore alpha" mode, make sure we decode the channel that we would
	// ordinarily swap into A to all-255
	if (ignore_alpha_chan)
	{
		const Vec128 base_alpha_all_0xff = _mm_setr_epi16(0,0,0,0xff, 0,0,0,0xff);
		Vec128 alpha_all_0xff = _mm_shuffle_epi8(base_alpha_all_0xff, crot_shuf);

		endpoints = _mm_or_si128(endpoints, alpha_all_0xff);
	}

	// Shuffle the endpoints around so we have the "low" and "high" end separated out
	Vec128 lo16 = _mm_unpacklo_epi64(endpoints, endpoints);
	Vec128 hi16 = _mm_unpackhi_epi64(endpoints, endpoints);
	Vec128 diff16 = _mm_sub_epi16(lo16, hi16); // yes, lo-hi!

	// Shuffles to expand pixel values to 16 bits without channel rotate
	const Vec128 base_pix16_lo_shuf = _mm_setr_epi8(0,-1, 1,-1, 2,-1, 3,-1, 4,-1, 5,-1, 6,-1, 7,-1);
	const Vec128 base_pix16_hi_shuf = _mm_setr_epi8(8,-1, 9,-1, 10,-1, 11,-1, 12,-1, 13,-1, 14,-1, 15,-1);

	// Apply channel rotate to them to get our actual shuffles
	Vec128 pix16_lo_shuf = _mm_shuffle_epi8(base_pix16_lo_shuf, crot_shuf);
	Vec128 pix16_hi_shuf = _mm_shuffle_epi8(base_pix16_hi_shuf, crot_shuf);

	for (SINTa i = 0; i < 64; i += 16)
	{
		Vec128 pix8 = load128u(tgt_pixels_rgba + i);
		Vec128 pix16_0 = _mm_shuffle_epi8(pix8, pix16_lo_shuf);
		Vec128 pix16_1 = _mm_shuffle_epi8(pix8, pix16_hi_shuf);

		store128u(b->base + i + 0, _mm_sub_epi16(lo16, pix16_0));
		store128u(b->base + i + 8, _mm_sub_epi16(lo16, pix16_1));
		store128u(b->diff + i + 0, diff16);
		store128u(b->diff + i + 8, diff16);
	}
}

static U32 endpoints_eval_sse4_twosubset(const BC7PredecodedEndpointsBlock * b, SINTa inds_from)
{
	Vec128 lerpf8 = load128u(b->index_cache + inds_from*16 + b->partition_eqv * (b->index_cache_size >> 2));
	return endpoints_eval_finish_sse4_oneindex(b, lerpf8);
}

static SINTa find_next_at_most_sse4_twosubset(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	const U8 * index_cache = b->index_cache + b->partition_eqv * (b->index_cache_size >> 2);
	for (SINTa i = 0; i < count; ++i)
	{
		Vec128 lerpf8 = load128u(index_cache + (inds_from + i) * 16);
		U32 ssd = endpoints_eval_finish_sse4_oneindex(b, lerpf8);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static void preendpoint_init_sse4_two_subset(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], U8 part_id, const Vec128 &both_lo16, const Vec128 &both_hi16)
{
	// Set up partition info and evaluators
	const BC67Partition * part = &bc67_partitions[1 + part_id];

	b->partition_id = part_id;
	b->partition_eqv = radtex_anchor_eqv[1][part_id];
#ifdef DO_BUILD_AVX2
	if (rrCPUx86_feature_present(RRX86_CPU_AVX2))
	{
		b->eval = bc7decode::endpoints_eval_avx2_twosubset;
		b->find_next_at_most = bc7decode::find_next_at_most_avx2_twosubset;
	}
	else
#endif
	{
		b->eval = endpoints_eval_sse4_twosubset;
		b->find_next_at_most = find_next_at_most_sse4_twosubset;
	}

	Vec128 both_diff16 = _mm_sub_epi16(both_lo16, both_hi16); // yes, lo-hi!

	// lo16/diff16 diffs between the subsets
	Vec128 lo16_subset0 = shuffle32<0,1,0,1>(both_lo16);
	Vec128 lo16_subset1 = shuffle32<2,3,2,3>(both_lo16);
	Vec128 diff16_subset0 = shuffle32<0,1,0,1>(both_diff16);
	Vec128 diff16_subset1 = shuffle32<2,3,2,3>(both_diff16);

	// Subset mask for the pixels
	Vec128 subset_mask = _mm_cvtsi32_si128(part->subset_mask);
	subset_mask = _mm_unpacklo_epi64(subset_mask, subset_mask);

	const Vec128 in_subset1_mask = _mm_setr_epi32(1<<0, 0, 1<<2, 0);

	for (SINTa i = 0; i < 64; i += 8)
	{
		// Determine 64-bit masks for whether a pixel is in subset 1
		Vec128 in_subset1 = _mm_cmpeq_epi64(_mm_and_si128(subset_mask, in_subset1_mask), in_subset1_mask);

		// Select the right lo and diff values for the subset
		Vec128 lo16 = _mm_blendv_epi8(lo16_subset0, lo16_subset1, in_subset1);
		Vec128 diff16 = _mm_blendv_epi8(diff16_subset0, diff16_subset1, in_subset1);

		// Load the source pixels and convert to 16 bits
		Vec128 pix8 = load64u(tgt_pixels_rgba + i);
		Vec128 pix16 = zext8to16_lo(pix8);

		// Write out the bae and diff values
		store128u(b->base + i, _mm_sub_epi16(lo16, pix16));
		store128u(b->diff + i, diff16);

		// Advance to the next group of 2 pixels
		subset_mask = _mm_srli_epi64(subset_mask, 4);
	}
}

// Expects lo8 = (R0,R2,R4, G0,G2,G4, B0,B2,B4, A0,A2,A4, <ignored>)
// and     hi8 = (R1,R3,R5, G1,G3,G5, B1,B3,B5, A1,A3,A5, <ignored>)
static void preendpoint_init_sse4_three_subset(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const BC67Partition * part, const Vec128 &lo8, const Vec128 &hi8)
{
	// Subset mask for the pixels
	Vec128 subset_mask = _mm_set1_epi32(part->subset_mask);
	const Vec128 subset_ind_mask = _mm_setr_epi32(3<<0, 3<<2, 0, 0);

	for (SINTa i = 0; i < 64; i += 8)
	{
		// Isolate next 2 subset indices
		Vec128 subset_inds = _mm_and_si128(subset_mask, subset_ind_mask);

		// Shift to place in second byte of every DWord
		Vec128 subset_inds_scaled = _mm_mullo_epi16(subset_inds, _mm_setr_epi32(1<<8, 1<<6, 0, 0));

		// Determine endpoint shuffle; we have the subset index, which we now need to replicate and then add the offset for the channel
		// also set all the odd pixels (=high halves of 16-bit values) to 0
		Vec128 endpoint_shuf = _mm_shuffle_epi8(subset_inds_scaled, _mm_setr_epi8(1,-1, 1,-1, 1,-1, 1,-1,  5,-1, 5,-1, 5,-1, 5,-1));
		endpoint_shuf = _mm_add_epi8(endpoint_shuf, _mm_setr_epi8(0,-1, 3,-1, 6,-1, 9,-1, 0,-1, 3,-1, 6,-1, 9,-1));

		// Extract 2 pixels worth of lo16 / hi16
		Vec128 lo16 = _mm_shuffle_epi8(lo8, endpoint_shuf);
		Vec128 hi16 = _mm_shuffle_epi8(hi8, endpoint_shuf);

		Vec128 diff16 = _mm_sub_epi16(lo16, hi16);

		// Load the source pixels and convert to 16 bits
		Vec128 pix8 = load64u(tgt_pixels_rgba + i);
		Vec128 pix16 = zext8to16_lo(pix8);

		// Write out the bae and diff values
		store128u(b->base + i, _mm_sub_epi16(lo16, pix16));
		store128u(b->diff + i, diff16);

		// Advance to the next group of 2 pixels
		subset_mask = _mm_srli_epi32(subset_mask, 4);
	}
}

static U32 endpoints_eval_sse4_mode0(const BC7PredecodedEndpointsBlock * b, SINTa inds_from)
{
	// Get indices from inds_from, everything else is already set up
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	Vec128 lerpf8 = sse4_decode_mode0_inds(load128u(b->index_cache + inds_from * 16), partition);
	return endpoints_eval_finish_sse4_oneindex(b, lerpf8);
}

static SINTa find_next_at_most_sse4_mode0(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	for (SINTa i = 0; i < count; ++i)
	{
		Vec128 lerpf8 = sse4_decode_mode0_inds(load128u(b->index_cache + (inds_from + i) * 16), partition);
		U32 ssd = endpoints_eval_finish_sse4_oneindex(b, lerpf8);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static void preendpoint_init_sse4_mode0(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from)
{
	// Decode the endpoints
	Vec128 lo8, hi8;
	sse4_decode_mode0_endpoints(&lo8, &hi8, load128u(endpts_from), endpts_from);

	// Set our evaluator
	b->partition_id = (endpts_from[0] >> 1) & 0xf;
	b->eval = endpoints_eval_sse4_mode0;
	b->find_next_at_most = find_next_at_most_sse4_mode0;

	// Set up for two-subset interpolation, standard way
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	preendpoint_init_sse4_three_subset(b, tgt_pixels_rgba, partition, lo8, hi8);
}

static void preendpoint_init_sse4_mode1(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from)
{
	// Decode the endpoints and initialize "base" and "diff"
	Vec128 lo16, hi16;
	sse4_decode_mode1_endpoints(&lo16, &hi16, load128u(endpts_from), endpts_from);

	preendpoint_init_sse4_two_subset(b, tgt_pixels_rgba, endpts_from[0] >> 2, lo16, hi16);
}

static U32 endpoints_eval_sse4_mode2(const BC7PredecodedEndpointsBlock * b, SINTa inds_from)
{
	// Get indices from inds_from, everything else is already set up
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	Vec128 lerpf8 = sse4_decode_mode2_inds(load128u(b->index_cache + inds_from * 16), partition);
	return endpoints_eval_finish_sse4_oneindex(b, lerpf8);
}

static SINTa find_next_at_most_sse4_mode2(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	for (SINTa i = 0; i < count; ++i)
	{
		Vec128 lerpf8 = sse4_decode_mode2_inds(load128u(b->index_cache + (inds_from + i) * 16), partition);
		U32 ssd = endpoints_eval_finish_sse4_oneindex(b, lerpf8);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static void preendpoint_init_sse4_mode2(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from)
{
	// Decode the endpoints
	Vec128 lo8, hi8;
	sse4_decode_mode2_endpoints(&lo8, &hi8, load128u(endpts_from), endpts_from);

	// Set our evaluator
	b->partition_id = (RR_GET16_LE_UNALIGNED(endpts_from) >> 3) & 0x3f;
	b->eval = endpoints_eval_sse4_mode2;
	b->find_next_at_most = find_next_at_most_sse4_mode2;

	// Set up for two-subset interpolation, standard way
	const BC67Partition * partition = &bc67_partitions[1 + 64 + b->partition_id];
	preendpoint_init_sse4_three_subset(b, tgt_pixels_rgba, partition, lo8, hi8);
}

// mode 3 shares eval/find_next_at_most funcs with mode 1 because the endpoint
// and index encoding differences are flattened out come eval time

static void preendpoint_init_sse4_mode3(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from)
{
	Vec128 lo16, hi16;
	sse4_decode_mode3_endpoints(&lo16, &hi16, load128u(endpts_from), endpts_from);

	U8 part_id = (endpts_from[0] >> 4) + (endpts_from[1] & 3) * 16;
	preendpoint_init_sse4_two_subset(b, tgt_pixels_rgba, part_id, lo16, hi16);
}

static void preendpoint_init_sse4_mode4(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from, bool ignore_alpha_chan)
{
	Vec128 endpoints16 = sse4_decode_mode4_endpoints(load128u(endpts_from));

	b->index_swap = (endpts_from[0] & 0x80) != 0;
	preendpoint_init_sse4_twoind(b, tgt_pixels_rgba, endpoints16, (endpts_from[0] >> 5) & 3, ignore_alpha_chan);
}

// mode 5 has different index and endpoint encoding but all that's resolved by eval time;
// actual eval/find_next_at_most funcs are shared with mode 4

static void preendpoint_init_sse4_mode5(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from, bool ignore_alpha_chan)
{
	Vec128 endpoints16 = sse4_decode_mode5_endpoints(load128u(endpts_from));

	b->index_swap = 0;
	preendpoint_init_sse4_twoind(b, tgt_pixels_rgba, endpoints16, endpts_from[0] >> 6, ignore_alpha_chan);
}

static U32 endpoints_eval_sse4_mode6(const BC7PredecodedEndpointsBlock * b, SINTa inds_from)
{
	// Get indices from inds_from, everything else is already set up
	Vec128 lerpf8 = load128u(b->index_cache + inds_from * 16);
	return endpoints_eval_finish_sse4_oneindex(b, lerpf8);
}

static SINTa find_next_at_most_sse4_mode6(const BC7PredecodedEndpointsBlock * b, SINTa inds_from, SINTa count, U32 * out_err, U32 err_thresh)
{
	for (SINTa i = 0; i < count; ++i)
	{
		Vec128 lerpf8 = load128u(b->index_cache + (inds_from + i) * 16);
		U32 ssd = endpoints_eval_finish_sse4_oneindex(b, lerpf8);
		if (ssd <= err_thresh)
		{
			*out_err = ssd;
			return i;
		}
	}

	return count;
}

static void preendpoint_init_sse4_mode6(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from, bool ignore_alpha_chan)
{
	Vec128 endpoints = sse4_decode_mode6_endpoints(load128u(endpts_from));

	// In "ignore alpha" mode, set things up so we always decode alpha to 0xff no matter what
	if (ignore_alpha_chan)
		endpoints = _mm_or_si128(endpoints, _mm_setr_epi16(0,0,0,0xff, 0,0,0,0xff));

	// Shuffle the endpoints around so we have the "low" and "high" end separated out
	Vec128 lo16 = _mm_unpacklo_epi64(endpoints, endpoints);
	Vec128 hi16 = _mm_unpackhi_epi64(endpoints, endpoints);
	Vec128 diff16 = _mm_sub_epi16(lo16, hi16); // yes, lo-hi!

	for (SINTa i = 0; i < 64; i += 16)
	{
		Vec128 pix8 = load128u(tgt_pixels_rgba + i);
		Vec128 pix16_0 = zext8to16_lo(pix8);
		Vec128 pix16_1 = zext8to16_hi(pix8);

		store128u(b->base + i + 0, _mm_sub_epi16(lo16, pix16_0));
		store128u(b->base + i + 8, _mm_sub_epi16(lo16, pix16_1));
		store128u(b->diff + i + 0, diff16);
		store128u(b->diff + i + 8, diff16);
	}

	// Set our evaluator
#ifdef DO_BUILD_AVX2
	if (rrCPUx86_feature_present(RRX86_CPU_AVX2))
	{
		b->eval = bc7decode::endpoints_eval_avx2_mode6;
		b->find_next_at_most = bc7decode::find_next_at_most_avx2_mode6;
	}
	else
#endif
	{
		b->eval = endpoints_eval_sse4_mode6;
		b->find_next_at_most = find_next_at_most_sse4_mode6;
	}
}

// mode 7 shares eval/find_next_at_most funcs with mode 1 because the endpoint
// and index encoding differences are flattened out come eval time

static void preendpoint_init_sse4_mode7(BC7PredecodedEndpointsBlock * b, const U8 tgt_pixels_rgba[8], const U8 * endpts_from, bool ignore_alpha_chan)
{
	// Decode the endpoints and initialize "base" and "diff"
	Vec128 lo16, hi16;
	sse4_decode_mode7_endpoints(&lo16, &hi16, load128u(endpts_from), endpts_from);

	// In "ignore alpha" mode, set things up so we always decode alpha to 0xff
	if (ignore_alpha_chan)
	{
		const Vec128 alpha_all_0xff = _mm_setr_epi16(0,0,0,0xff, 0,0,0,0xff);
		lo16 = _mm_or_si128(lo16, alpha_all_0xff);
		hi16 = _mm_or_si128(hi16, alpha_all_0xff);
	}

	preendpoint_init_sse4_two_subset(b, tgt_pixels_rgba, endpts_from[1] & 63, lo16, hi16);
}

#endif

BC7PredecodedEndpointsBlock::BC7PredecodedEndpointsBlock()
{
	index_cache = 0;
	index_cache_size = 0;
	eval = 0;
	find_next_at_most = 0;
	target_mode = 8;
	ignore_alpha = 0;
	partition_id = 0;
	partition_eqv = 0;
	index_swap = 0;
}

BC7PredecodedEndpointsBlock::~BC7PredecodedEndpointsBlock()
{
	if ( index_cache )
		OODLE_FREE_ARRAY( index_cache, index_cache_size );

}

void BC7PredecodedEndpointsBlock::init_indices(int mode, const U8 * inds_from, SINTa inds_stride, SINTa inds_count)
{
	RR_ASSERT(mode >= 0 && mode <= 7);
	target_mode = (U8)mode;

#ifdef DO_BUILD_SSE4
	SINTa group_size;

	// Some modes do their own thing for setup
	switch (target_mode)
	{
		// One-subset modes are easy since we don't need to worry about anchor positions
	case 4:
		index_cache_size = inds_count * 32;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; ++i)
			sse4_decode_mode4_inds(index_cache + i*32, load128u(inds_from + i*inds_stride));
		return;

	case 5:
		index_cache_size = inds_count * 32;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; ++i)
			sse4_decode_mode5_inds(index_cache + i*32, load128u(inds_from + i*inds_stride));
		return;

	case 6:
		index_cache_size = inds_count * 16;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; ++i)
			store128u(index_cache + i*16, sse4_decode_mode6_inds(load128u(inds_from + i*inds_stride)));
		return;

		// Two-subset modes, we need to decode multiple times because the second anchor can be in several positions
	case 1:
		group_size = inds_count * 16;
		index_cache_size = group_size * 4;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; i++)
		{
			Vec128 raw_inds = load128u(inds_from + i*inds_stride);
			for (SINTa j = 0; j < 4; j++)
				store128u(index_cache + i*16 + j*group_size, sse4_decode_mode1_inds(raw_inds, two_subset_repr[j]));
		}
		return;

	case 3:
		group_size = inds_count * 16;
		index_cache_size = group_size * 4;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; i++)
		{
			Vec128 raw_inds = load128u(inds_from + i*inds_stride);
			for (SINTa j = 0; j < 4; j++)
				store128u(index_cache + i*16 + j*group_size, sse4_decode_mode3_inds(raw_inds, two_subset_repr[j]));
		}
		return;

	case 7:
		group_size = inds_count * 16;
		index_cache_size = group_size * 4;
		index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
		for (SINTa i = 0; i < inds_count; i++)
		{
			Vec128 raw_inds = load128u(inds_from + i*inds_stride);
			for (SINTa j = 0; j < 4; j++)
				store128u(index_cache + i*16 + j*group_size, sse4_decode_mode7_inds(raw_inds, two_subset_repr[j]));
		}
		return;
	}
#endif

	// General path
	index_cache_size = inds_count * 16;
	index_cache = OODLE_MALLOC_ARRAY_ALIGNED(U8,index_cache_size,16);
	for (SINTa i = 0; i < inds_count; ++i)
		memcpy(index_cache + i*16, inds_from + i*inds_stride, 16);
}

void BC7PredecodedEndpointsBlock::init_endpoints(const U8 * endpts_from, const U8 tgt_pixels_rgba[64], bool ignore_alpha_chan)
{
	// endpt_block = endpoints with pbits, indices zeroed
	bc7bits endpts = bc7bits_load(endpts_from);
	RR_ASSERT(bc7bits_get_mode(endpts) == target_mode);

	bc7bits idxmask(c_bc7bitrange_indices_mask[target_mode]);

	bc7bits_store(endpt_block, bc7bits_andnot(endpts, idxmask));
	bc7bits_store(index_mask, idxmask);

	ignore_alpha = ignore_alpha_chan ? 1 : 0;

#ifdef DO_BUILD_SSE4
	switch (target_mode)
	{
	case 0: preendpoint_init_sse4_mode0(this, tgt_pixels_rgba, endpts_from); break;
	case 1: preendpoint_init_sse4_mode1(this, tgt_pixels_rgba, endpts_from); break;
	case 2: preendpoint_init_sse4_mode2(this, tgt_pixels_rgba, endpts_from); break;
	case 3: preendpoint_init_sse4_mode3(this, tgt_pixels_rgba, endpts_from); break;
	case 4: preendpoint_init_sse4_mode4(this, tgt_pixels_rgba, endpts_from, ignore_alpha_chan); break;
	case 5: preendpoint_init_sse4_mode5(this, tgt_pixels_rgba, endpts_from, ignore_alpha_chan); break;
	case 6: preendpoint_init_sse4_mode6(this, tgt_pixels_rgba, endpts_from, ignore_alpha_chan); break;
	case 7: preendpoint_init_sse4_mode7(this, tgt_pixels_rgba, endpts_from, ignore_alpha_chan); break;

	default:
		// Placeholder/debug impl!
		eval = endpoints_eval_sse4_basic;
		find_next_at_most = find_next_at_most_sse4_basic;

		for LOOP(i,64)
			base[i] = tgt_pixels_rgba[i];
		break;
	}
#else
	eval = endpoints_eval_scalar_basic;
	find_next_at_most = find_next_at_most_scalar_basic;

	for LOOP(i,64)
		base[i] = tgt_pixels_rgba[i];
#endif
}

OODLE_NS_END

