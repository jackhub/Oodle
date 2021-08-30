// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4

#include "bc4rd.h"
#include "blocksurface.h"
#include "bc4compress.h"
#include "perceptualactivity.h"
#include "perceptualactivity.inl"
#include "log2table.h"
#include "newlz_simd.h"
#include "vec128.inl"
#include "templates/rrvector.h"
#include "templates/rralgorithm.h"

//#include "rrsimpleprof.h"
#include "rrsimpleprofstub.h"

RR_NAMESPACE_START

// 04-30-2020 : lambda normalization
// try using the same lambda scale as BC1
//	so that BC3 gets even weighting of RGBA errors
//	if that's viable for BC4 we'd prefer that
//	BC1 is now scale = 2 (in bits/VQD)
//#define BC4RD_LAMBDA_SCALE 1.25f // before
//#define BC4RD_LAMBDA_SCALE	(1.7f) // BC1
#define BC4RD_LAMBDA_SCALE	(1.5f * OO2TEX_GLOBAL_LAMBDA_SCALE) // down a bit from BC1

// HIGH CONCEPT:
//
// BC4 blocks are 2 bytes worth of endpoints then 6 bytes (48 bits) worth of
// indices, 3 bits per texel.
//
// Even if we get an exact match out of both endpoint bytes, that's too short to
// get past our minimum match len (3 or 4 bytes depending on context). The only
// way to get a match involving the endpoints is to either match endpoints _and_
// indices both, or have a match continue from the previous block's indices.
//
// We don't want to explicitly seek out generating whole-block matches; they're
// good for rate but bad perceptually when over-used.
//
// That means we're mainly focusing on the indices here.
//
// We try the following:
// - Keep a window of the most recent N indices, and for every block, try whether
//   reusing one of them gives acceptable error (as per the R-D metric).
// - Bottom-up index merge. Quasi-VQ scheme to merge indices between groups of
//   blocks when doing so is visually acceptable.

// input format is what source format the rrPixelFormat corresponds to
static BC4SourceFormat translate_input_format(rrPixelFormat fmt)
{
	switch (fmt)
	{
	case rrPixelFormat_1_U8:
	case rrPixelFormat_2_U8: 		return BC4SourceFormat_U8;
	case rrPixelFormat_1_S8:
	case rrPixelFormat_2_S8: 		return BC4SourceFormat_S8;
	case rrPixelFormat_1_U16:
	case rrPixelFormat_2_U16:		return BC4SourceFormat_U16;
	case rrPixelFormat_1_S16:
	case rrPixelFormat_2_S16:		return BC4SourceFormat_S16;
	case rrPixelFormat_B8G8R8A8:
	case rrPixelFormat_R8G8B8A8:	return BC4SourceFormat_RGBA_U8; // only care about the alpha portion so RGB channel order is irrelevant
	default:						return BC4SourceFormat_Invalid;
	}
}

static BC4ValueType determine_value_type(rrPixelFormat fmt)
{
	switch (fmt)
	{
	case rrPixelFormat_1_U8:
	case rrPixelFormat_1_U16:
	case rrPixelFormat_2_U8:
	case rrPixelFormat_2_U16:		return BC4ValueType_UNorm;
	case rrPixelFormat_1_S8:
	case rrPixelFormat_1_S16:
	case rrPixelFormat_2_S8:
	case rrPixelFormat_2_S16:		return BC4ValueType_SNorm;
	case rrPixelFormat_B8G8R8A8:
	case rrPixelFormat_R8G8B8A8:	return BC4ValueType_Alpha;
	default:						RR_BREAK(); return BC4ValueType_UNorm; // just to return _something_.
	}
}

union BC4Block
{
	U8 bytes[8];
	struct
	{
		U8 end[2]; // endpoints
		U8 inds[6]; // bit-packed indices, 3 bits per index
	} s;

	static BC4Block make(U8 e0, U8 e1, U64 inds)
	{
		BC4Block blk;
		U64 value = e0 | (e1 << 8) | (inds << 16);
		RR_PUT64_LE_UNALIGNED(blk.bytes, value);
		return blk;
	}

	U16 read_endpoint_pair() const
	{
		return RR_GET16_LE_UNALIGNED(bytes);
	}

	U64 read_inds() const
	{
		return RR_GET64_LE_UNALIGNED(bytes) >> 16;
	}
};

#ifdef __RADSSE2__
struct BC4EndpointVariantPara
{
	Vec128 e0weight;
	Vec128 e1weight;
	Vec128 recip;
	Vec128 interp_consts;
};
#endif

// Endpoint clamp range and related stuff
struct BC4EndpointEncInfo
{
	F32 lo, hi; // lo/hi clamp ranges
	F32 scale; // overall scale to get into endpoint range

#ifdef __RADSSE2__
	VecF32x4 lo_vec;
	VecF32x4 hi_vec;

	BC4EndpointVariantPara variant[2];
#endif

	BC4EndpointEncoding enc;
};

// Tracks which pixels of a block are constrained to either the minimum or maximum value
// in preserve extremes mode
struct BC4IndexConstraints
{
	// bit i+ 0 in mask is set if pixel i is constrained to the minimum value
	// bit i+16 in mask is set if pixel i is constrained to the maximum value
	U32 mask;

	// A set of constraints is feasible if it's not mutually contradictory, i.e. no
	// pixel constrained to two different values
	bool feasible() const
	{
		// mask>>16 gives us an implicit & 0xffff for free
		return (mask & (mask >> 16)) == 0;
	}

	// Two sets of constraints can be merged by ORing their masks together, which
	// gives us the union of the source constraints. Note the result need not be feasible.
	BC4IndexConstraints merge_with(const BC4IndexConstraints &x) const
	{
		BC4IndexConstraints result = { mask | x.mask };
		return result;
	}

	// Checks whether a given set of constraints is the superset of another
	bool superset_of(const BC4IndexConstraints &x) const
	{
		return (mask & x.mask) == x.mask;
	}
};

// Cached info for a set of indices
struct BC4IndexCache
{
	U8 weights[16]; // weight_A | (weight_B << 4)
	F32 inv_ata[4]; // a_00, a_01, a_11 entries of (A^T A)^(-1); 4th is weight to determine mean
	F32 degen_bias;
	BC4IndexConstraints extremes; // position of the extremes in the active index mode
};

static void init_index_cache(BC4IndexCache * ic, U64 inds, int variant, const BC4EndpointEncInfo & info)
{
	RR_ASSERT(variant == 0 || variant == 1);

	// AA,BB: <= 7*7*16 < 1024
	// so 10 bits per weight is enough for everything
#define WEIGHTS(a,b,ismin,ismax) ((a)*(a) + ((a)*(b) << 10) + ((b)*(b) << 20)), (a) + ((b) << 4) + ((ismin) << 8) + ((ismax) << 24)

	static const S32 weight_table[2][16] = // [variant][index * 2 + field]
	{
		// four-interpolated-indices mode
		{
			WEIGHTS(5,0,0,0),
			WEIGHTS(0,5,0,0),
			WEIGHTS(4,1,0,0),
			WEIGHTS(3,2,0,0),
			WEIGHTS(2,3,0,0),
			WEIGHTS(1,4,0,0),
			WEIGHTS(0,0,1,0), // always min
			WEIGHTS(0,0,0,1), // always max
		},
		// six-interpolated-indices mode
		{
			WEIGHTS(7,0,0,1), // can be max (if ep0=255)
			WEIGHTS(0,7,1,0), // can be min (if ep1=0)
			WEIGHTS(6,1,0,0),
			WEIGHTS(5,2,0,0),
			WEIGHTS(4,3,0,0),
			WEIGHTS(3,4,0,0),
			WEIGHTS(2,5,0,0),
			WEIGHTS(1,6,0,0)
		},
	};

#undef WEIGHTS

	const S32 * weights = weight_table[variant];
	S32 products = 0;
	U32 extremes_mask = 0;
	S32 count = 0;
	S32 bias = 0;

	for LOOP(i,16)
	{
		int idx = static_cast<int>(inds & 7);
		inds >>= 3;

		products += weights[idx*2 + 0];
		U32 weights1 = weights[idx*2 + 1]; // weights1 also holds the "is extreme" mask

		ic->weights[i] = static_cast<U8>(weights1 & 0xff);
		extremes_mask |= (weights1 >> 8) << i;

		// count counts number of pixels that influence mean
		count += (weights1 & 0xff) ? 1 : 0;
		// bias is sum of the weights of the second endpoint
		bias += (weights1 >> 4) & 0xf;
	}

	ic->extremes.mask = extremes_mask;

	S32 AA = products & 0x3ff;
	S32 AB = (products >> 10) & 0x3ff;
	S32 BB = products >> 20;
	S32 det = AA*BB - AB*AB;

	// If system singular, we have a 1D (or 0D) LLS problem.
	// We already handle the degenerate case when the two endpoints
	// quantize to the same value or have the wrong ordering; just
	// make sure we end up in that case.
	if ( det == 0 )
	{
		// Setting these all to 0 makes the endpoints come out equal
		// (and 0) which gets us into the "degenerate case" handler.
		ic->inv_ata[0] = 0.0f;
		ic->inv_ata[1] = 0.0f;
		ic->inv_ata[2] = 0.0f;
	}
	else
	{
		// Scale starts out as 1/det
		double scale = (variant == 1) ? 7.0 / det : 5.0 / det;
		scale *= info.scale;

		ic->inv_ata[0] = static_cast<F32>(scale * BB);
		ic->inv_ata[1] = static_cast<F32>(scale * -AB);
		ic->inv_ata[2] = static_cast<F32>(scale * AA);
	}

	// The A/B weights in every row that is not one of the special always-min/max
	// values sum to the same thing, so we can recover the mean later by summing
	// A and B, which is how we handle degenerate cases.
	//
	// In 6-interp mode (variant 1) that sum is 7, in 4-interp mode it's 5.
	F32 count_scale = 1.0f / static_cast<F32>(count * (variant == 1 ? 7 : 5));
	ic->inv_ata[3] = info.scale * count_scale;

	// degen_bias is used for degenerate systems; see explanation in
	// cached_lls_solve for details.
	ic->degen_bias = (variant == 1) ? bias * count_scale : -bias * count_scale;
}

struct BC4WindowEntry
{
	BC4Block block;
	U8 predecoded_inds[16];
	int most_recent_bi; // block index we were most recently used in
	BC4IndexCache inds[2]; // index caches for the two modes
};

static bool bc4_is_6interp(U8 e0, U8 e1, const BC4EndpointEncInfo & info)
{
	// Linear-space endpoints to compare to decide endpoint ordering
	// for unsigned values, this does nothing
	// for signed values, it gives us (signed_value + 128), which compares correctly
	int e0q = e0 ^ info.enc.sign_mask;
	int e1q = e1 ^ info.enc.sign_mask;
	return e0q > e1q;
}

static U64 bc4_flip_4interp(U64 indices)
{
	U64 flipped = 0;
	for LOOP(i,16)
	{
		U64 ind = (indices >> (3*i)) & 7;
		if (ind < 2)
			ind ^= 1; // 0<->1
		else if (ind < 6)
			ind ^= 7; // 2<->5, 3<->4
		// 6 and 7 stay as they are under endpoint flips

		flipped |= ind << (3*i);
	}

	return flipped;
}

static U64 bc4_flip_6interp(U64 indices)
{
	U64 flipped = 0;
	for LOOP(i,16)
	{
		U64 ind = (indices >> (3*i)) & 7;
		if (ind < 2)
			ind ^= 1; // 0<->1
		else
			ind = 9 - ind; // 2<->7, 3<->6, 4<->5

		flipped |= ind << (3*i);
	}

	return flipped;
}

static void bc4_compute_palette(S16 palette[8], U8 e0, U8 e1, const BC4EndpointEncInfo & info, bool is_sixi)
{
	// Dequantize endpoints
	S16 e0deq = info.enc.dequant[e0];
	S16 e1deq = info.enc.dequant[e1];

	palette[0] = e0deq;
	palette[1] = e1deq;

	if (is_sixi)
	{
		int base = 7 * e0deq;
		int diff = e1deq - e0deq;

		// 6 interpolated values
		palette[2] = static_cast<S16>((base + 1*diff) / 7);
		palette[3] = static_cast<S16>((base + 2*diff) / 7);
		palette[4] = static_cast<S16>((base + 3*diff) / 7);
		palette[5] = static_cast<S16>((base + 4*diff) / 7);
		palette[6] = static_cast<S16>((base + 5*diff) / 7);
		palette[7] = static_cast<S16>((base + 6*diff) / 7);
	}
	else
	{
		int base = 5 * e0deq;
		int diff = e1deq - e0deq;

		// 4 interpolated values
		palette[2] = static_cast<S16>((base + 1*diff) / 5);
		palette[3] = static_cast<S16>((base + 2*diff) / 5);
		palette[4] = static_cast<S16>((base + 3*diff) / 5);
		palette[5] = static_cast<S16>((base + 4*diff) / 5);
		palette[6] = static_cast<S16>(info.enc.min_deq);
		palette[7] = static_cast<S16>(info.enc.max_deq);
	}
}

static void scalar_bc4_decode_with_pal(S16 * out_values, U64 inds, const S16 palette[8])
{
	for (int i = 0; i < 16; i++)
	{
		int j = static_cast<int>(inds & 7);
		out_values[i] = palette[j];
		inds >>= 3;
	}
}

static void scalar_bc4_decode_with_inds_and_pal(S16 * out_values, const U8 predecoded_inds[16], const S16 palette[8])
{
	for (int i = 0; i < 16; i++)
		out_values[i] = palette[predecoded_inds[i] >> 1];
}

static void scalar_bc4_decode(S16 * out_values, const U8 * block, const BC4EndpointEncInfo & info)
{
	S16 palette[8];
	bc4_compute_palette(palette, block[0], block[1], info, bc4_is_6interp(block[0], block[1], info));
	U64 inds = RR_GET64_LE_UNALIGNED(block) >> 16;
	scalar_bc4_decode_with_pal(out_values, inds, palette);
}

#ifdef DO_BUILD_SSE4

// actually purely SSE2
static RADFORCEINLINE Vec128 sse4_compute_palette(U8 e0, U8 e1, const BC4EndpointEncInfo & info, int variant)
{
	// Dequantize endpoints
	Vec128 e0vec = _mm_set1_epi16(info.enc.dequant[e0]);
	Vec128 e1vec = _mm_set1_epi16(info.enc.dequant[e1]);

	const BC4EndpointVariantPara & var = info.variant[variant];

	// Linear combinations
	Vec128 e0scaled = _mm_mullo_epi16(e0vec, var.e0weight);
	Vec128 e1scaled = _mm_mullo_epi16(e1vec, var.e1weight);
	Vec128 endpoints_scaled = _mm_add_epi16(e0scaled, e1scaled);

	// Divide by 5 or 7 by multiplying with the appropriate reciprocal
	// values in mode 0 are in [-0x5000,0x5000]
	// values in mode 1 are in [-0x7000,0x7000]
	// both get sufficient precision with a .17 fraction
	Vec128 endpoints = _mm_srai_epi16(_mm_mulhi_epi16(endpoints_scaled, var.recip), 1);

	// Negative values end up with quotients too small by 1, fix this.
	endpoints = _mm_sub_epi16(endpoints, _mm_srai_epi16(endpoints_scaled, 15));

	// Finally, throw in the constants (if present)
	endpoints = _mm_or_si128(endpoints, var.interp_consts);

	return endpoints;
}

// Returns U8 vector with indices multiplied by 2 since that's what
// we need for our PSHUFBs later
static RADFORCEINLINE Vec128 sse4_bc4_decode_inds(const Vec128 & bytes)
{
	// Grab the bytes containing contiguous groups of 2 indices (6 bits) and
	// expand them out to 16-bit lanes
	const Vec128 index_shuffle = _mm_setr_epi8(
		2,3, 2,3, 3,4, 4,5,
		5,6, 5,6, 6,7, 7,8
	);
	// Multiplies to align those 6 bits in the desired place; namely, we want to move
	// our 6 bits to occupy bits [11:6] of their respective 16-bit lanes. This _just_
	// works out.
	const Vec128 index_mul = _mm_setr_epi16(
		1<<(12-6 - 0), // 0 bits into byte 2
		1<<(12-6 - 6), // 6 bits into byte 2
		1<<(12-6 - 4), // 4 bits into byte 3
		1<<(12-6 - 2), // 2 bits into byte 4
		1<<(12-6 - 0), // 0 bits into byte 5
		1<<(12-6 - 6), // 6 bits into byte 5
		1<<(12-6 - 4), // 4 bits into byte 6
		1<<(12-6 - 2)  // 2 bits into byte 7
	);

	// Grab the 6 index bits each into 16-bit lanes
	Vec128 bitfields_aligned = _mm_mullo_epi16(_mm_shuffle_epi8(bytes, index_shuffle), index_mul);

	// Puzzle the halves together; we want the high 3 bits to end up in bits [11:9] and the
	// low 3 bits to end up in bits [3:1], because we actually want the 3-bit indices shifted left
	// by 1 packed into bytes in the end.
	Vec128 hi_bytes = _mm_and_si128(bitfields_aligned, _mm_set1_epi16(0x0e00));
	Vec128 lo_bytes = _mm_and_si128(_mm_srli_epi16(bitfields_aligned, 5), _mm_set1_epi16(0x000e));
	Vec128 index8_scaled = _mm_or_si128(lo_bytes, hi_bytes);

	return index8_scaled;
}

static RADFORCEINLINE void sse4_bc4_decode_with_inds_and_pal(S16 * out_values, const Vec128 & index8_scaled, const Vec128 & pal)
{
	// Second index to grab second half of 16-bit word
	Vec128 index8_scaled_plus_1 = _mm_add_epi8(index8_scaled, _mm_set1_epi8(1));

	// Our palette LUT entirely fits in a single vector so all we need to do is look up those
	// table entries via PSHUFB
	Vec128 index16_0 = _mm_unpacklo_epi8(index8_scaled, index8_scaled_plus_1);
	Vec128 index16_1 = _mm_unpackhi_epi8(index8_scaled, index8_scaled_plus_1);

	Vec128 result_0 = _mm_shuffle_epi8(pal, index16_0);
	Vec128 result_1 = _mm_shuffle_epi8(pal, index16_1);

	store128u(out_values + 0, result_0);
	store128u(out_values + 8, result_1);
}

static void sse4_bc4_decode(S16 * out_values, const U8 * block, const BC4EndpointEncInfo & info)
{
	bool is_sixi = bc4_is_6interp(block[0], block[1], info);
	Vec128 pal = sse4_compute_palette(block[0], block[1], info, is_sixi ? 1 : 0);
	Vec128 block_bytes = load64u(block);
	Vec128 index8_scaled = sse4_bc4_decode_inds(block_bytes);

	sse4_bc4_decode_with_inds_and_pal(out_values, index8_scaled, pal);
}

#endif

static RADFORCEINLINE void bc4_decode(S16 * out_values, const U8 * block, const BC4EndpointEncInfo & info)
{
#ifdef DO_BUILD_SSE4
	sse4_bc4_decode(out_values, block, info);
#else
	scalar_bc4_decode(out_values, block, info);
#endif
}

static RADFORCEINLINE void bc4_decode_with_inds(S16 * out_values, U8 e0, U8 e1, const BC4EndpointEncInfo & info, const U8 predecoded_inds[16], int variant)
{
#ifdef DO_BUILD_SSE4
	Vec128 pal = sse4_compute_palette(e0, e1, info, variant);
	Vec128 index8_scaled = load128u(predecoded_inds);
	sse4_bc4_decode_with_inds_and_pal(out_values, index8_scaled, pal);
#else
	S16 palette[8];
	bc4_compute_palette(palette, e0, e1, info, variant != 0);
	scalar_bc4_decode_with_inds_and_pal(out_values, predecoded_inds, palette);
#endif
}

static void bc4_predecode_palette(S16 palette[8], U8 e0, U8 e1, const BC4EndpointEncInfo & info, int variant)
{
#ifdef DO_BUILD_SSE4
	store128u(palette, sse4_compute_palette(e0, e1, info, variant));
#else
	bc4_compute_palette(palette, e0, e1, info, variant != 0);
#endif
}

static void bc4_predecode_inds(U8 predecoded_inds[16], const U64 * pIndices)
{
#ifdef DO_BUILD_SSE4
	Vec128 inds = _mm_slli_epi64(load64u(pIndices), 16);
	Vec128 index8_scaled = sse4_bc4_decode_inds(inds);
	store128u(predecoded_inds, index8_scaled);
#else
	U64 inds = *pIndices;
	for (int i = 0; i < 16; i++)
	{
		predecoded_inds[i] = static_cast<U8>((inds & 7) << 1);
		inds >>= 3;
	}
#endif
}

static void bc4_decode_with_inds_and_pal(S16 * out_values, const U8 predecoded_inds[16], const S16 palette[8])
{
#ifdef DO_BUILD_SSE4
	Vec128 pal = load128u(palette);
	Vec128 index8_scaled = load128u(predecoded_inds);
	sse4_bc4_decode_with_inds_and_pal(out_values, index8_scaled, pal);
#else
	scalar_bc4_decode_with_inds_and_pal(out_values, predecoded_inds, palette);
#endif
}

static F32 bc4rd_D(const BC4SourceData & decomp, const BC4SourceData & orig, const SingleFloatBlock4x4 & activity)
{
	return VQD(decomp.values, orig.values, activity);
}

// uncompressed block is 64 bits
// R is just in bits
#define bc4rd_R_uncompressed	(64)

static F32 bc4rd_matched_R_lz(int lzi)
{
	// cost of LZ match:
	//	note in BC7 match len and match offset are not constant / multiples of 4
	//	so they take more bits to send than in BC1

	// I don't have strong data to favor this
	// but I do like it theoretically for parse guiding
	// because it at least breaks near-ties in favor of recency

	// lzi is in blocks
	// lzi starts at 1
	RR_ASSERT( lzi >= 1 );

	// @@ lzi only goes up to 256 so this could just be a table lookup

	// starts you off at 14 bits with lzi=1
	F32 offset_bits = (F32)rrlog2_bk(lzi+16);
	F32 match_bits = 10 + offset_bits;
	// could fiddle these constants more but it's over-training on one image

	return match_bits;
}

static F32 bc4rd_matched_index_R(int offset)
{
	// endpoint is assumed to be sent raw
	F32 raw_bits = 16;

	// the index is sent with an LZ match
	F32 match_bits = bc4rd_matched_R_lz(offset);

	F32 bits = raw_bits + match_bits;
	return bits;
}

static F32 bc4rd_J(F32 D, F32 R, F32 lambda)
{
	// D is VQD (~ 2*SSD)
	// R is just in bits (unlike vc1rd where it's 16*bits)
	// lambda is in steps of 10

	F32 J = D + lambda * R;

	return J;
}

struct BC4IndsAndId
{
	U64 packed; // bitfield: (inds << 16) | count

	BC4IndsAndId()
		: packed(0)
	{
	}

	BC4IndsAndId(U64 inds, int mode, int id)
	{
		RR_ASSERT(mode >= 0 && mode <= 1);
		RR_ASSERT(id >= 0 && id < (1 << 15));
		packed = (inds << 16) | (mode << 15) | id;
	}

	U64 inds() const
	{
		return packed >> 16;
	}

	int mode() const
	{
		return static_cast<int>((packed >> 15) & 1);
	}

	int id() const
	{
		return static_cast<int>(packed & 0x7fff);
	}
};

// default ordering: by inds then count (increasing)
static bool operator <(const BC4IndsAndId &a, const BC4IndsAndId &b)
{
	return a.packed < b.packed;
}

#define LARGE_D_S32 0x48000000 // a bit larger than (1<<30) which is the largest D we can produce "naturally" for a single block

struct BC4BlockInfo
{
	F32 baseline_D;
	F32 J;
	F32 D;
	BC4IndexConstraints constraints;
	BC4SourceData pixels;
};

struct BC4IndexVqBlock
{
	U8 endpt_pair[2]; // [which]
	U16 unused;
	S32 link;
	S16 palette[8]; // derived from endpt_pair
};

struct BC4IndexVqEntry
{
	U64 inds;
	S64 distortion_sum;

	S32 block_link; // linked list of blocks with this set of inds
	U32 count;
	U32 count_log2_count;
	int merged_onto; // negative if not merged yet
	BC4IndexConstraints constraints; // merged for whole cluster
	int mode; // 0 or 1

	U8 index8_scaled[16]; // indices decoded to U8 and scaled by 2
	BC4IndexCache indc;
};

// sort by counts decreasing
static bool operator <(const BC4IndexVqEntry &a, const BC4IndexVqEntry &b)
{
	return a.count > b.count;
}

struct BC4IndexVqHeapEntry
{
	int fm, to;
	F32 dJ; // positive dJ is good
	int fm_count_save;
	S64 fm_distortion_onto;
};

// make_heap builds a max heap given regular operator < which is what we want
static bool operator <(const BC4IndexVqHeapEntry &a, const BC4IndexVqHeapEntry &b)
{
	return a.dJ < b.dJ;
}

static RADFORCEINLINE void cached_lls_solve(U8 * pEp0, U8 * pEp1, const BC4BlockInfo & blki, const BC4IndexCache * ic, const BC4EndpointEncInfo & info, int mode)
{
	// Constraints must be satisfied for you to call this
	RR_ASSERT( ic->extremes.superset_of(blki.constraints) );

	S32 target_sign = -mode;

#ifdef __RADSSE2__
	// Perform the dot products
	const Vec128 mask16_0xf = _mm_set1_epi16(0xf);

	Vec128 weights8 = load128u(ic->weights);

	Vec128 weights16_0 = _mm_unpacklo_epi8(weights8, weights8);
	Vec128 vals0 = load128u(blki.pixels.values + 0);
	Vec128 Adot = _mm_madd_epi16(_mm_and_si128(weights16_0, mask16_0xf), vals0);
	Vec128 Bdot = _mm_madd_epi16(_mm_srli_epi16(weights16_0, 12), vals0);

	Vec128 weights16_1 = _mm_unpackhi_epi8(weights8, weights8);
	Vec128 vals1 = load128u(blki.pixels.values + 8);
	Adot = _mm_add_epi32(Adot, _mm_madd_epi16(_mm_and_si128(weights16_1, mask16_0xf), vals1));
	Bdot = _mm_add_epi32(Bdot, _mm_madd_epi16(_mm_srli_epi16(weights16_1, 12), vals1));

	// Horizontal reduction part 1 (also interleaves A and B)
	Vec128 AB0 = _mm_unpacklo_epi64(Adot, Bdot); // A0,A1, B0,B1
	Vec128 AB1 = _mm_unpackhi_epi64(Adot, Bdot); // A2,A3, B2,B3
	Vec128 ABsum1 = _mm_add_epi32(AB0, AB1); // A02,A13, B02,B13

	// Horizontal reduction part 2
	Vec128 ABsum2 = _mm_add_epi32(ABsum1, shuffle32<1,0,3,2>(ABsum1)); // AX,AX, BX,BX

	// Solve for endpoints 0 and 1
	VecF32x4 ABsum2_flt = VecF32x4::from_int32(ABsum2);
	VecF32x4 inv_ata = VecF32x4::loadu(ic->inv_ata).shuf<0,1,1,2>();
	VecF32x4 ep_prods = ABsum2_flt * inv_ata; // (AX*inv_ata[0], AX*inv_ata[1], BX*inv_ata[1], BX*inv_ata[2])
	VecF32x4 ep_flt = ep_prods + ep_prods.shuf<2,3,0,1>(); // (AX*inv_ata[0] + BX*inv_ata[1], AX*inv_ata[1] + BX*inv_ata[2]) replicated twice

	// Clamp endpoints to valid range
	ep_flt = vmax(ep_flt, info.lo_vec);
	ep_flt = vmin(ep_flt, info.hi_vec);

	// Convert to int
	Vec128 ep_int = ep_flt.to_int32_round();
	int ep0_int = _mm_cvtsi128_si32(ep_int);
	int ep1_int = _mm_cvtsi128_si32(shuffle32<1,1,1,1>(ep_int));

	// ABsum computation (for degenerate case)
	int ABsum = _mm_cvtsi128_si32(_mm_add_epi32(ABsum2, shuffle32<2,3,0,1>(ABsum2)));

	RAD_ALIGN(F32, ep_arr[4], 16);
	ep_flt.storea(ep_arr);
#else
	// Perform the dot products
	int AX = 0;
	int BX = 0;
	for LOOP(i,16)
	{
		int weights = ic->weights[i];
		int X = blki.pixels.values[i];

		AX += (weights & 0xf) * X;
		BX += (weights >> 4) * X;
	}

	// ABsum computation (for degenerate case)
	int ABsum = AX + BX;

	// Solve for endpoints 0 and 1
	F32 AX_flt = static_cast<F32>(AX);
	F32 BX_flt = static_cast<F32>(BX);

	F32 ep0 = AX_flt * ic->inv_ata[0] + BX_flt * ic->inv_ata[1];
	F32 ep1 = AX_flt * ic->inv_ata[1] + BX_flt * ic->inv_ata[2];

	// clamp to valid range
	F32 ep_arr[2];
	ep_arr[0] = RR_CLAMP(ep0, info.lo, info.hi);
	ep_arr[1] = RR_CLAMP(ep1, info.lo, info.hi);

	// convert to int
	int ep0_int = rr_froundint(ep_arr[0]);
	int ep1_int = rr_froundint(ep_arr[1]);
#endif

	// In six-interp mode (which makes target_sign = ~0) and with non-trivial constraints,
	// we need to be careful
	if ( ( blki.constraints.mask & target_sign ) != 0 )
	{
		if ( blki.constraints.mask & 0x0000ffffu ) ep1_int = info.enc.min_q;
		if ( blki.constraints.mask & 0xffff0000u ) ep0_int = info.enc.max_q;
	}

	// ep1 - ep0 >= 0: four-interpolated color mode
	// ep1 - ep0 < 0: six-interpolated-color mode
	//
	// we want to avoid ep0 = ep1 in general (because it gives us no useful
	// interpolated values). That means in four-interp mode we want ep1 - ep0 > 0,
	// and in six-interp mode we want ep1 - ep0 < 0. If they're identical or
	// have the wrong sign, we need to fix it!
	int ep_diff = ((ep1_int - ep0_int) ^ target_sign) - target_sign;
	if (ep_diff <= 0)
	{
		// Degenerate case is hit in three circumstances:
		// 1. the original LLS system was singular. In that case, init_index_cache
		//    sets us up with a linear system that ensures we end up here.
		// 2. the LLS solution gave two endpoints that close enough together to
		//    quantize to the same number.
		// 3. the LLS solution gave us two endpoints that are ordered so they don't
		//    give the mode we want to hit.
		//
		// The third case is the most interesting one: it means the linear inequality
		// constraint implied by the endpoint ordering is active. The non-degenerate
		// cases (i.e. we never allow ep0==ep1) are:
		//
		//   Four-interp: ep0 + 1 <= ep1
		//   Six-interp:  ep1 + 1 <= ep0
		//
		// This is a quadratic problem and the constraint volume implied by these linear
		// inequalities is just a half-space. If the constraint is active, that means
		// the minimum is attained on the boundary, i.e. on the ep0 + 1 = ep1 (or
		// respectively ep1 + 1 = ep0) line.
		//
		// So we need to find the values for ep0 and ep1 that are 1 apart from each
		// other, in the right mode, and minimize the quadratic error. This least-squares
		// problem is (here the version for four-interp)
		//
		// min  ||    [ e ]            ||^2
		//  e   ||A * [e+1] - scale*k*b||
		//
		// where scale = max_quant / max_dequant and the nonzero rows of A all sum
		// to k, the denominator of the interpolation weights (constant per mode).
		// Split A into its two columns A=[a_0 a_1], then a_0 + a_1 = k (in the
		// rows that are not zero) and this simplifies to
		//
		// min ||k * e + a_1 - scale*k*b||^2
		//  e
		//
		// Dividing by k yields
		//
		// min ||e - (scale*b - (1/k)*a_1)||^2
		//  e
		//
		// which is clearly minimized when e = mean(scale*b - a_1/k) = (scale/k)*mean(k*b) - mean(a_1)/k
		// ABsum is sum(k*b), -mean(a_1)/k can be precomputed in the index cache (this is degen_bias).
		//
		// Six-interp mode works the same way but degen_bias has the opposite sign.
		F32 mean = static_cast<F32>(ABsum) * ic->inv_ata[3] + ic->degen_bias;

		ep0_int = rr_froundint(mean);
		if (target_sign == 0)
		{
			ep0_int = RR_CLAMP(ep0_int, info.enc.min_q, info.enc.max_q - 1);
			ep1_int = ep0_int + 1;
		}
		else
		{
			ep0_int = RR_CLAMP(ep0_int, info.enc.min_q + 1, info.enc.max_q);
			ep1_int = ep0_int - 1;

			// and if that violates our constraints, we need to enforce them again,
			// no matter how much it hurts
			if ( blki.constraints.mask & 0x0000ffffu ) ep1_int = info.enc.min_q;
			if ( blki.constraints.mask & 0xffff0000u ) ep0_int = info.enc.max_q;
		}

		RR_ASSERT(ep0_int >= info.enc.min_q && ep0_int <= info.enc.max_q);
		RR_ASSERT(ep1_int >= info.enc.min_q && ep1_int <= info.enc.max_q);
		RR_ASSERT(((ep1_int - ep0_int) ^ target_sign) >= 0);
	}

	*pEp0 = static_cast<U8>(ep0_int & 0xff);
	*pEp1 = static_cast<U8>(ep1_int & 0xff);
}

static int cached_lls_solve_both(U8 * pEp0, U8 * pEp1, const BC4BlockInfo & info, const BC4IndexCache * ic, const BC4EndpointEncInfo & enc_info)
{
	// Usually at least one of the two variants will drop out because
	// the solved-for endpoints have the wrong ordering. If we can
	// make variant 1 happen, prefer it.
	for (int variant = 1; variant >= 0; --variant)
	{
		// constraints satisfiable in this mode?
		if ( ic[variant].extremes.superset_of(info.constraints) )
		{
			cached_lls_solve(pEp0, pEp1, info, &ic[variant], enc_info, variant);
			return variant;
		}
	}

	return -1;
}

static RADFORCEINLINE S32 bc4rd_SSD(const S16 * decoded, const S16 * orig_values)
{
#ifdef __RADSSE2__
	// load all 16 pixels and diff them
	Vec128 diff01_16 = _mm_sub_epi16(load128u(decoded + 0), load128u(orig_values + 0));
	Vec128 diff23_16 = _mm_sub_epi16(load128u(decoded + 8), load128u(orig_values + 8));

	// Dot products with itself
	Vec128 dot01 = _mm_madd_epi16(diff01_16, diff01_16);
	Vec128 dot23 = _mm_madd_epi16(diff23_16, diff23_16);

	// Horizontal reduction
	Vec128 dots = _mm_add_epi32(dot01, dot23); // add to 4-away
	S32 ssd = hsum_epi32_sse2(dots);
#else
	S32 ssd = 0;
	for LOOP(i,16)
	{
		int diff = decoded[i] - orig_values[i];
		ssd += diff*diff;
	}
#endif

	return ssd;
}

#ifdef RR_DO_ASSERTS
static bool bc4rd_assert_block_link_check(const BC4IndexVqEntry * entry,
	const vector<BC4IndexVqBlock> & blocks )
{
	// verify links :
	int link = entry->block_link;
	U32 link_count = 0;
	while( link >= 0 )
	{
		link_count++;
		link = blocks[link].link;
	}
	RR_ASSERT( link_count == entry->count );
	return true;
}
#endif // RR_DO_ASSERTS

static RADFORCEINLINE S64 bc4rd_single_block_change_distortion(int bi,
	const vector<BC4IndexVqBlock> & blocks,
	const vector<BC4BlockInfo> & infos,
	const BC4EndpointEncInfo & enc_info,
	const BC4IndexVqEntry * indices_from,
	const S16 palette[8])
{
	S16 decoded[16];
	bc4_decode_with_inds_and_pal(decoded,indices_from->index8_scaled,palette);

	S32 ssd = bc4rd_SSD(decoded, infos[bi].pixels.values);
	return ssd;
}

static RADFORCEINLINE S64 bc4rd_single_block_index_change_distortion(int bi,
	const vector<BC4IndexVqBlock> & blocks,
	const vector<BC4BlockInfo> & infos,
	const BC4EndpointEncInfo & enc_info,
	const BC4IndexVqEntry * indices_from)
{
	return bc4rd_single_block_change_distortion(bi,blocks,infos,enc_info,indices_from,blocks[bi].palette);
}

// If a candidate merge's initial error isn't this much worse than the cut-off,
// try whether solving for new endpoints gets us below the threshold.
//
// We don't want to do the full-solve on hopeless candidates, which is going
// to be most of them.
#define DISTORTION_RATIO_FOR_REFINE 3

// walk block link starting at bi
// change indices to "new_indices"
// measure total distortion
static S64 bc4rd_block_link_distortion(int bi,
	const vector<BC4IndexVqBlock> & blocks,
	const vector<BC4BlockInfo> & infos,
	const BC4IndexVqEntry * indices_from,
	const BC4EndpointEncInfo & enc_info,
	bool allow_new_endpoints,
	S64 must_beat_D = LARGE_D_S32)
{
	S64 D_sum = 0;

	while( bi >= 0 )
	{
		// we can always keep the existing endpoints
		S64 best_D = bc4rd_single_block_index_change_distortion(bi,blocks,infos,enc_info,indices_from);

		if ( allow_new_endpoints && best_D <= (must_beat_D - D_sum) * DISTORTION_RATIO_FOR_REFINE )
		{
			U8 ep0, ep1;
			cached_lls_solve(&ep0,&ep1,infos[bi],&indices_from->indc,enc_info,indices_from->mode);
			if ( ( ( ep0 ^ blocks[bi].endpt_pair[0] ) | ( ep1 ^ blocks[bi].endpt_pair[1] ) ) != 0 )
			{
				S16 palette[8];
				bc4_predecode_palette(palette,ep0,ep1,enc_info,indices_from->mode);
				S64 D = bc4rd_single_block_change_distortion(bi,blocks,infos,enc_info,indices_from,palette);
				best_D = RR_MIN(best_D,D);
			}
		}

		D_sum += best_D;

		if ( D_sum > must_beat_D )
		{
			// early out when must_beat_D is hit
			// this is hit 99% of the time
			// it's a great speedup for long block lists
			//	but doesn't help at all with singletons
			return LARGE_D_S32;
		}

		bi = blocks[bi].link;
	}

	return D_sum;
}

static F32 bc4rd_index_merge_dJ(const BC4IndexVqEntry & from, const BC4IndexVqEntry & to, S64 D_diff, F32 lambda, const BC4EndpointEncInfo & enc_info)
{
	// diff = distortions
	//	no need to multiply by count as we've already accumulated across the N blocks
	F32 D = static_cast<F32>(D_diff);

	// each entry has to send 48 bit raw indices + count * selection of those indices
	//	same rate model as vq_codelen_palette_count

	// when you do a merge, the main rate savings is the 48 bits for one less index value
	//	but you also save some because the selection gets cheaper

	// log2tabled returns values scaled up by RR_LOG2TABLE_ONE
	// the denominator terms (nblocks) cancel out
	U32 merged_count = from.count + to.count;

	// count*log2count is cached in index_vq_entry :
	U32 log2one_from = from.count_log2_count;
	U32 log2one_to = to.count_log2_count;

	RR_ASSERT( log2one_from == from.count * log2tabled_bk_32(from.count) );
	RR_ASSERT( log2one_to   == to.count * log2tabled_bk_32(to.count) );

	U32 log2one_merged   = merged_count * log2tabled_bk_32(merged_count);
	U32 log2one_delta = log2one_from + log2one_to - log2one_merged;

	F32 rate_delta = log2one_delta * (1.f / (F32)RR_LOG2TABLE_ONE);

	rate_delta += 48.0f;

	F32 R  = rate_delta;

	// R and D are positive here
	//	D is the distortion cost of the merge, R is the rate savings
	//	(D can sometimes by slightly negative when an index change actually helps a block)

	F32 dJ = - D + lambda * R;
	// dJ > 0 is a good merge
	//	the rate savings is enough to justify the distortion penalty
	return dJ;
}

static bool can_satisfy_constraints(const BC4IndexVqEntry &e, const BC4IndexConstraints &constraints)
{
	return e.indc.extremes.superset_of(constraints);
}

static bool is_legal_merge(const BC4IndexVqEntry &fm, const BC4IndexVqEntry &to)
{
	if ( fm.mode != to.mode )
		return false;

	// Compute merged constraint set
	BC4IndexConstraints merged = fm.constraints.merge_with(to.constraints);

	// If the merged set was infeasible, this will automatically fail
	return can_satisfy_constraints(to,merged);
}

static void make_heap_bottom_up_merge_fm_singleton(
	vector<BC4IndexVqHeapEntry> & heap,
	const vector<BC4IndexVqEntry> & entries,
	const vector<BC4IndexVqBlock> & blocks,
	const vector<BC4BlockInfo> & infos,
	int fm,
	F32 lambda,
	S64 max_distortion_increase,
	const BC4EndpointEncInfo & enc_info)
{
	S64 fm_base_distortion = entries[fm].distortion_sum;

	int fm_bi = entries[fm].block_link;
	RR_DURING_ASSERT( S64 check_D = bc4rd_single_block_index_change_distortion(fm_bi, blocks, infos, enc_info, &entries[fm]) );
	RR_ASSERT( check_D == fm_base_distortion );

	// initializing with max_distortion_increase or not is meh
	S64 best_distortion = fm_base_distortion + max_distortion_increase;

	RR_DURING_ASSERT( U64 fm_indices = entries[fm].inds );

	const S16 * palette = blocks[fm_bi].palette;

	for LOOPVEC(to,entries)
	{
		if ( fm == to ) continue;

		if ( ! is_legal_merge(entries[fm],entries[to]) )
			continue;

		RR_ASSERT( fm_indices != entries[to].inds );

		// try changing fm's indices to "to" :
		S64 new_index_D = bc4rd_single_block_change_distortion(fm_bi, blocks, infos, enc_info, &entries[to], palette);

		// if that's close enough that we think it might be viable, try solving for new endpoints
		if ( new_index_D <= best_distortion * DISTORTION_RATIO_FOR_REFINE )
		{
			U8 ep0, ep1;
			cached_lls_solve(&ep0,&ep1,infos[fm_bi],&entries[to].indc,enc_info,entries[to].mode);

			S16 new_palette[8];
			bc4_predecode_palette(new_palette,ep0,ep1,enc_info,entries[to].mode);
			S64 D = bc4rd_single_block_change_distortion(fm_bi,blocks,infos,enc_info,&entries[to],new_palette);
			new_index_D = RR_MIN(new_index_D,D);
		}

		// entries are sorted by count so I only have to look at J when D gets better
		if ( new_index_D <= best_distortion )
		{
			RR_ASSERT( new_index_D < LARGE_D_S32 );
			best_distortion = new_index_D;

			// make D the delta :
			S64 delta_D = new_index_D - fm_base_distortion;

			F32 dJ = bc4rd_index_merge_dJ( entries[fm], entries[to], delta_D, lambda, enc_info );
			if ( dJ > 0 )
			{
				// make a heap entry :
				BC4IndexVqHeapEntry he;
				he.fm = fm;
				RR_ASSERT( entries[fm].count == 1 );
				he.fm_count_save = 1;
				he.fm_distortion_onto = new_index_D;
				he.to = to;
				he.dJ = dJ;
				heap.push_back(he);
			}
		}
	}
}

// final emit for a cluster of blocks using the same indices
// this version just leaves the indices unchanged and solves for new
// endpoints, nothing fancy.
static void bc4rd_final_emit_cluster_basic(BlockSurface * bc4_blocks,
	const BlockSurface * activity_blocks,
	int group_shift,
	int group_mask,
	const BC4IndexVqEntry & cluster,
	const vector<BC4BlockInfo> & infos,
	vector<BC4IndexVqBlock> & blocks,
	const BC4EndpointEncInfo & enc_info)
{
	U64 new_indices = cluster.inds;
	int mode = cluster.mode;

	//U64 flipped_indices = (mode == 1) ? bc4_flip_6interp(new_indices) : bc4_flip_4interp(new_indices);

	// walk these blocks and commit the change
	for ( int bi = cluster.block_link; bi >= 0; bi = blocks[bi].link )
	{
		// We've decided on the indices to use, first try with original endpoints.
		// This is the option we scored during the merge process.
		S32 source_bi = bi >> group_shift;
		BC4Block * block_ptr = (BC4Block *) (BlockSurface_Seek(bc4_blocks,source_bi) + (bi & group_mask) * 8);
		const SingleFloatBlock4x4 * pActivity = BlockSurface_SeekC_SingleFloatBlock4x4(activity_blocks,source_bi);

		// can always do the original endpoints
		RR_ASSERT( cluster.indc.extremes.superset_of(infos[bi].constraints) );
		*block_ptr = BC4Block::make( blocks[bi].endpt_pair[0], blocks[bi].endpt_pair[1], new_indices );

		BC4SourceData decoded;
		bc4_decode(decoded.values, block_ptr->bytes, enc_info);
		F32 best_D = bc4rd_D(decoded, infos[bi].pixels, *pActivity);

		// Also try re-solving for endpoints, using those endpoints if doing so increases quality.
		// The idea is that we treat all choices of endpoints as same rate because we only care
		// about the indices; might as well try and do a good job here. (This solve is not inside any
		// of the hot loops, so it doesn't need to be super-fast.)
		U8 ep0, ep1;
		cached_lls_solve(&ep0,&ep1,infos[bi],&cluster.indc,enc_info,mode);

		bc4_decode_with_inds(decoded.values, ep0, ep1, enc_info, cluster.index8_scaled, mode);
		F32 D = bc4rd_D(decoded, infos[bi].pixels, *pActivity);
		if (D < best_D)
		{
			best_D = D;
			*block_ptr = BC4Block::make(ep0, ep1, new_indices);
		}
	}
}

// final emit for a cluster of blocks that will all get the same indices, but
// then do a new solve for ideal indices within that particular cluster of blocks
static void bc4rd_final_emit_cluster_optimize(BlockSurface * bc4_blocks,
	const BlockSurface * activity_blocks,
	int group_shift,
	int group_mask,
	const BC4IndexVqEntry & cluster,
	const vector<BC4BlockInfo> & infos,
	vector<BC4IndexVqBlock> & blocks,
	const BC4EndpointEncInfo & enc_info)
{
	// do the initial solve for this cluster
	bc4rd_final_emit_cluster_basic(bc4_blocks,activity_blocks,group_shift,group_mask,cluster,infos,blocks,enc_info);

	int mode = cluster.mode;

	// solve for new optimal indices in the whole cluster
	S64 total_errs[16][8] = {}; // [pixel][ind]
	for ( int bi = cluster.block_link; bi >= 0; bi = blocks[bi].link )
	{
		S32 source_bi = bi >> group_shift;
		const BC4Block * block_ptr = (const BC4Block *) (BlockSurface_SeekC(bc4_blocks,source_bi) + (bi & group_mask) * 8);

		S16 pal[8];
		bc4_predecode_palette(pal,block_ptr->s.end[0],block_ptr->s.end[1],enc_info,mode);

		// accumulate squared errors for all palette entries over all pixels
		const BC4SourceData & src = infos[bi].pixels;
		for LOOP(i,16)
		{
			for LOOP(j,8)
			{
				S64 diff = src.values[i] - pal[j];
				total_errs[i][j] += diff * diff;
			}
		}
	}

	// turn that into new indices
	BC4IndexVqEntry new_cluster = cluster;
	new_cluster.inds = 0;

	// index for min/max values
	int min_ind = (mode == 1) ? 1 : 6;
	int max_ind = (mode == 1) ? 0 : 7;

	for LOOP(i,16)
	{
		// find min error
		S64 best_err = total_errs[i][0];
		int best_ind = 0;

		for (int j = 1; j < 8; ++j)
		{
			S64 e = total_errs[i][j];
			if (e < best_err)
			{
				best_err = e;
				best_ind = j;
			}
		}

		// override if pixel is constrained
		if (new_cluster.constraints.mask & (1u << i)) // constrained to min
			best_ind = min_ind;
		if (new_cluster.constraints.mask & (0x10000u << i)) // constrained to max
			best_ind = max_ind;

		new_cluster.inds |= (U64)best_ind << (3 * i);
	}

	// if that's the same indices we already solved for, all good
	if (new_cluster.inds == cluster.inds)
		return;

	// else redo the endpoint solve
	bc4_predecode_inds(new_cluster.index8_scaled, &new_cluster.inds);
	init_index_cache(&new_cluster.indc, new_cluster.inds, mode, enc_info);
	bc4rd_final_emit_cluster_basic(bc4_blocks,activity_blocks,group_shift,group_mask,new_cluster,infos,blocks,enc_info);
}


// bc4rd_bottom_up_merge_indices
//	bottom-up N^2 merge of indices within each mode group
//	 does only {i -> j} , not {i,j} -> {best single index for i+j}
//	does not try to change endpoints, considers them fixed
static void bc4rd_bottom_up_merge_indices(BlockSurface * bc4_blocks,
	const BlockSurface * activity_blocks,
	F32 vqd_lambda,
	int group_shift,
	const vector<BC4BlockInfo> & infos,
	const BC4EndpointEncInfo & enc_info)
{
	SIMPLEPROFILE_SCOPE(bc4rd_bottom_up_merge);

	// VQD ~ 2*SSD
	// @@@@ NOTE(fg): REALLY not true for some of our BC4s!!!!
	//
	// J = D + lambda*R
	//
	// therefore, if we switch from VQD (outside) to SSD (inside) as a distortion
	// metric, we need to scale lambda down proportionately
	F32 lambda = vqd_lambda / 2.0f;

	int group_mask = (1 << group_shift) - 1;
	int nb = bc4_blocks->count << group_shift;

	vector<BC4IndsAndId> v_indices;
	vector<BC4IndexVqBlock> blocks;

	blocks.resize(nb);

	for LOOP(bi,nb)
	{
		// Figure out where we are in the current group
		int source_bi = bi >> group_shift;
		int block_in_group = bi & group_mask;

		const BC4Block * block_ptr = (const BC4Block *) (BlockSurface_SeekC(bc4_blocks,source_bi) + block_in_group * 8);

		// Block infos
		BC4IndexVqBlock &bbi = blocks[bi];
		bbi.endpt_pair[0] = block_ptr->s.end[0];
		bbi.endpt_pair[1] = block_ptr->s.end[1];
		int mode = bc4_is_6interp(bbi.endpt_pair[0],bbi.endpt_pair[1],enc_info) ? 1 : 0;
		bbi.link = -1;
		bc4_predecode_palette(bbi.palette, bbi.endpt_pair[0], bbi.endpt_pair[1], enc_info, mode);

		// Index array
		v_indices.push_back(BC4IndsAndId(block_ptr->read_inds(), mode, bi));
	}

	// Sort blocks by inds then count field to put duplicate inds next to each other
	stdsort(v_indices.begin(),v_indices.end());

	vector<BC4IndexVqEntry> entries;
	vector<BC4IndexVqHeapEntry> heap;
	entries.reserve(nb);
	heap.reserve(nb);

	for LOOPVEC(vi,v_indices)
	{
		int bi = v_indices[vi].id();
		int mode = v_indices[vi].mode();
		RR_ASSERT(blocks[bi].link == -1);

		if ( infos[bi].J == 0.0f ) // single color blocks excluded
			continue;

		if ( entries.empty() || entries.back().inds != v_indices[vi].inds() || entries.back().mode != mode )
		{
			// new run
			BC4IndexVqEntry entry;
			entry.inds = v_indices[vi].inds();
			entry.block_link = bi;
			entry.count = 1;
			entry.distortion_sum = 0; // will be updated in a second
			entry.merged_onto = -1;
			entry.constraints = infos[bi].constraints;
			entry.mode = mode;
			bc4_predecode_inds(entry.index8_scaled, &entry.inds);
			init_index_cache(&entry.indc, entry.inds, entry.mode, enc_info);

			RR_ASSERT( can_satisfy_constraints(entry,entry.constraints) );

			// not actually using VQD but SSD here:
			S64 D = bc4rd_single_block_index_change_distortion(bi,blocks,infos,enc_info,&entry);
			entry.distortion_sum = D;

			entries.push_back(entry);
		}
		else
		{
			// add to existing run
			BC4IndexVqEntry & entry = entries.back();
			blocks[bi].link = entry.block_link;
			entry.block_link = bi;

			S64 D = bc4rd_single_block_index_change_distortion(bi,blocks,infos,enc_info,&entry);

			entry.count++;
			entry.distortion_sum += D;
			entry.constraints = entry.constraints.merge_with(infos[bi].constraints);

			RR_ASSERT( can_satisfy_constraints(entry,entry.constraints) );
		}
	}

	if ( entries.size() < 2 )
		return;

	// update cached fields in entries :
	for LOOPVEC(ei,entries)
	{
		entries[ei].count_log2_count = entries[ei].count * log2tabled_bk_32( entries[ei].count );
	}

	// D threshold for dj > 0 :
	S64 max_distortion_increase = static_cast<S64>(48.0f*lambda);

	// Max number of blocks in a set for us to consider recalculating the endpoints
	const U32 kMaxCountForRecalc = 8;
	const bool kAllowRecalcForLink = true;

	// sort entries by count (decreasing) so we go in rate-increasing order
	stdsort(entries.begin(),entries.end());
	RR_ASSERT( entries[0].count >= entries[1].count );

	//===========================================
	// make heap for non-singletons :

	{
	int entries_size = entries.size32();
	int first_singleton = entries_size;

	{
	SIMPLEPROFILE_SCOPE(bum_groups_make);

	vector<S64> best_distortions;
	best_distortions.resize(first_singleton);

	// process chunks of the entries array that fit in L1 to minimize cache thrashing
	const int entry_chunk_size = (16 * 1024) / sizeof(entries[0]);

	for (int to_chunk_begin = 0; to_chunk_begin < entries_size; to_chunk_begin += entry_chunk_size)
	{
		const int to_chunk_end = RR_MIN(to_chunk_begin + entry_chunk_size, entries_size);
		int fm = 0;

		for (;fm<entries_size;fm++)
		{
			// FIRST LOOP OF SIMILAR PAIR
			// HEAP FOR NON-SINGLETON FM's :
			if ( entries[fm].count <= 1 ) break; // count sorted order so when we hit a 1 we break

			S64 fm_base_distortion = entries[fm].distortion_sum;
			bool allow_new_endpoints = kAllowRecalcForLink && entries[fm].count <= kMaxCountForRecalc;

			RR_DURING_ASSERT( S64 check_D = bc4rd_block_link_distortion( entries[fm].block_link, blocks, infos, &entries[fm], enc_info, false ) );
			RR_ASSERT( check_D == fm_base_distortion );

			// only bother looking if within max_distortion_increase
			// initializing with max_distortion_increase or not is meh
			S64 & best_distortion = best_distortions[fm];

			// initialize in first pass
			if ( to_chunk_begin == 0 )
				best_distortion = fm_base_distortion + max_distortion_increase;

			// entries are sorted by count (increasing rate)
			//	best_distortion decreases as we go
			// -> decreasing best_distortion is a big time saver and not bad for quality

			for (int to = to_chunk_begin; to < to_chunk_end; to++)
			{
				if ( fm == to ) continue;

				if ( ! is_legal_merge(entries[fm],entries[to]) )
					continue;

				RR_ASSERT( entries[fm].inds != entries[to].inds );

				// [fm] is multiple blocks, possibly of different partitions (all same mode)
				RR_ASSERT( entries[fm].count > 1 );

				// try changing fm sets' indices to "to" :
				S64 new_index_D = bc4rd_block_link_distortion( entries[fm].block_link, blocks, infos, &entries[to], enc_info, allow_new_endpoints, best_distortion );

				// entries are sorted by count so I only have to look at J when D gets better
				//  (this is an approximation, tests indicate it's okay)
				//		(it's exact for the first merge step, but not later as things get merged up)
				//	(to be exact you should go ahead and add everything with dj > 0 to the heap)
				//	(if we only did one merge this would be fine, it would not be an approximation
				//	 the issue is that later on, all your desired merge targets may be gone
				//	 so the best thing left may be one of the ones that we ruled out here)
				if ( new_index_D <= best_distortion )
				{
					RR_ASSERT( new_index_D < LARGE_D_S32 );
					best_distortion = new_index_D;

					// make D the delta :
					S64 delta_D = new_index_D - fm_base_distortion;

					F32 dJ = bc4rd_index_merge_dJ( entries[fm], entries[to], delta_D, lambda, enc_info );
					if ( dJ > 0 )
					{
						// make a heap entry :
						BC4IndexVqHeapEntry he;
						he.fm = fm;
						he.fm_count_save = entries[fm].count;
						he.fm_distortion_onto = new_index_D;
						he.to = to;
						he.dJ = dJ;
						heap.push_back(he);
					}
				}
			}
		}

		first_singleton = fm;
	}
	}

	//===========================================
	// make heap for singletons
	{
	SIMPLEPROFILE_SCOPE(bum_singles_make);

	// fm starts where we broke out of groups
	RR_ASSERT( first_singleton == entries_size || entries[first_singleton].count == 1 );

	for (int fm = first_singleton;fm<entries_size;fm++)
	{
		// SECOND LOOP OF SIMILAR PAIR
		// SINGLETON FMS ONLY

		// only singleton sources :
		RR_ASSERT( entries[fm].count == 1 );

		make_heap_bottom_up_merge_fm_singleton(heap,entries,blocks,infos,fm,lambda,max_distortion_increase,enc_info);
	}
	}
	}
	
	make_heap(heap.begin(),heap.end());

	// now pop the heap and do the merges :

	{
	//SIMPLEPROFILE_SCOPE(bum_merge); // near zero
	// heap is sorted by dj, largest first
	while( ! heap.empty() )
	{
		BC4IndexVqHeapEntry heap_entry = heap[0];
		popped_heap(heap.begin(),heap.end());
		heap.pop_back();

		// if from entry is gone, ignore me
		int fm = heap_entry.fm;
		if ( entries[ fm ].merged_onto >= 0 )
			continue;

		// if [fm] entry changed (count changed) since I computed heap.dj
		//	 then it needs to recompute D & I should repush this heap entry
		// [to] entry changing entry doesn't affect us, unless it was merged onto someone else
		bool dirty = ( heap_entry.fm_count_save != (int)entries[ fm ].count );

		int to = heap_entry.to;
		if ( entries[ to ].merged_onto >= 0 )
		{
			// if my dest was merged, chase where he went
			do
			{
				to = entries[ to ].merged_onto;
			} while( entries[to].merged_onto >= 0 );
			if ( to == fm ) // I'm considering A->C , but C already did C->A or C->B->A
				continue;

			dirty = true;
		}

		if ( dirty )
		{
			// needs to be a legal merge
			if ( ! is_legal_merge(entries[fm],entries[to]) )
				continue;

			// make a new candidate for me to merge onto merged_to

			S64 fm_base_distortion = entries[fm].distortion_sum;
			bool allow_new_endpoints = kAllowRecalcForLink && entries[fm].count <= kMaxCountForRecalc;

			RR_DURING_ASSERT( S64 check_D = bc4rd_block_link_distortion( entries[fm].block_link , blocks, infos, &entries[fm], enc_info, false ) );
			RR_ASSERT( check_D == fm_base_distortion );

			S64 must_beat_D = fm_base_distortion + max_distortion_increase;

			// try changing indices to "to" :
			S64 new_index_D = bc4rd_block_link_distortion( entries[fm].block_link, blocks, infos, &entries[to], enc_info, allow_new_endpoints, must_beat_D );

			if ( new_index_D < LARGE_D_S32 )
			{
				// make D the delta :
				S64 delta_D = new_index_D - fm_base_distortion;

				F32 dJ = bc4rd_index_merge_dJ( entries[fm], entries[to], delta_D, lambda, enc_info );
				if ( dJ > 0 )
				{
					// make a heap entry :
					BC4IndexVqHeapEntry he;
					he.fm = fm;
					he.fm_count_save = entries[fm].count;
					he.fm_distortion_onto = new_index_D;
					he.to = to;
					he.dJ = dJ;
					heap.push_back(he);
					push_heap(heap.begin(),heap.end());
				}
			}

			continue;
		}

		// should be ensured now
		RR_ASSERT( is_legal_merge(entries[fm],entries[to]) );

		// fm and to are both alive
		// do the merge

		RR_ASSERT( heap_entry.fm_count_save == (int)entries[ fm ].count );
		//RR_ASSERT( heap_entry.fm_distortion_onto == block_link_index_distortion(&entries[fm],blocks,entries[to].indices,LARGE_DIFF,palvec) );

		RR_ASSERT( bc4rd_assert_block_link_check(&entries[fm],blocks) );
		RR_ASSERT( bc4rd_assert_block_link_check(&entries[to],blocks) );

		//RR_ASSERT( entries[fm].distortion_sum == block_link_index_distortion(&entries[fm],blocks,entries[fm].indices,LARGE_DIFF,palvec) );
		//RR_ASSERT( entries[to].distortion_sum == block_link_index_distortion(&entries[to],blocks,entries[to].indices,LARGE_DIFF,palvec) );

		entries[fm].merged_onto = to;
		//entries[fm].count = 0; // optional, for debug tracking, keeps sum of all entries counts constant
		entries[to].count += entries[fm].count;
		entries[to].constraints = entries[to].constraints.merge_with(entries[fm].constraints);

		entries[to].count_log2_count = entries[to].count * log2tabled_bk_32( entries[to].count );

		// merge the actual blocks :
		// all [fm] indices change to [to]

		// find the tail of the link to connect them
		int link = entries[fm].block_link;
		U32 link_count = 1;
		while( blocks[link].link >= 0 )
		{
			link_count++;
			RR_ASSERT( link_count <= entries[fm].count );

			link = blocks[link].link;
		}
		RR_ASSERT( link_count == entries[fm].count );

		// link the [to] chain onto it :
		int orig_to_head = entries[to].block_link;
		blocks[link].link = orig_to_head;
		entries[to].block_link = entries[fm].block_link;

		//RR_ASSERT( assert_block_link_check(&entries[to],blocks) );

		// If the "from" cluster doesn't have too many blocks in it, recalculate endpoints and
		// resulting errors for all of them. The idea is that for smaller clusters, we expect we can
		// often decrease the error significantly by doing this. We don't want to do this every time
		// since it leads to quadratic blow-up, but also the expectation is that once a cluster has
		// a bunch of blocks in it, we are very unlikely to decided on a merge that would change its
		// endpoints completely. (Either way, we'll do a full solve for every block at the end.)
		//
		// NOTE: I expected this might really help and it just doesn't, even if I set kMaxCountForRecalc
		// very high (I tried 128). It does run, and changes endpoints, it just never makes much of
		// a difference. OTOH it also turns out to be fairly cheap so I'm keeping it.
		if ( entries[fm].count <= kMaxCountForRecalc )
		{
			// "from" blocks are at the start of the newly merged list
			for ( int bi = entries[fm].block_link; bi != orig_to_head; bi = blocks[bi].link )
			{
				// solve for new endpoints
				int mode = entries[to].mode;
				cached_lls_solve(&blocks[bi].endpt_pair[0],&blocks[bi].endpt_pair[1],infos[bi],&entries[to].indc,enc_info,mode);

				bc4_predecode_palette(blocks[bi].palette,blocks[bi].endpt_pair[0],blocks[bi].endpt_pair[1],enc_info,mode);
				S64 D = bc4rd_single_block_change_distortion(bi,blocks,infos,enc_info,&entries[to],blocks[bi].palette);

				entries[to].distortion_sum += D;
			}
		}
		else
		{
			// just add on the distortion of [fm] list adding on , which we already computed for heap J
			//	could store that in the heap record, then just check it here
			entries[to].distortion_sum += heap_entry.fm_distortion_onto;
		}

		RR_DURING_ASSERT( S64 check_distortion_sum = bc4rd_block_link_distortion(entries[to].block_link,blocks,infos,&entries[to],enc_info,false) );
		RR_ASSERT( entries[to].distortion_sum == check_distortion_sum );
	}
	}

	// scan out just the un-merged entries :
	// indices_out and total_count_out are accumulated from both the 3c and 4c loop
	for LOOPVEC(entry_i,entries)
	{
		if ( entries[entry_i].merged_onto >= 0 )
			continue;

		RR_ASSERT( entries[entry_i].count > 0 );
		RR_ASSERT( bc4rd_assert_block_link_check(&entries[entry_i],blocks) );

		//bc4rd_final_emit_cluster_basic(bc4_blocks,activity_blocks,group_shift,group_mask,entries[entry_i],infos,blocks,enc_info);
		bc4rd_final_emit_cluster_optimize(bc4_blocks,activity_blocks,group_shift,group_mask,entries[entry_i],infos,blocks,enc_info);
	}
}

static void canonicalize_for_constraints(BC4Block * block, const BC4IndexConstraints & constraints, const BC4EndpointEncInfo & enc_info)
{
	// 6-interp is always fine
	if (bc4_is_6interp(block->s.end[0],block->s.end[1],enc_info))
		return;

	U64 new_inds = block->read_inds();
	U32 mask = constraints.mask;

	// iterate over pixels that need index 6
	while (mask & 0xffff)
	{
		int pos = rrCtz32(mask);
		mask &= mask - 1;

		new_inds &= ~(7ull << (pos * 3));
		new_inds |= 6ull << (pos * 3);
	}

	// iterate over pixels that need index 7
	while (mask)
	{
		int pos = rrCtz32(mask) - 16;
		mask &= mask - 1;

		new_inds |= 7ull << (pos * 3);
	}

	*block = BC4Block::make(block->s.end[0],block->s.end[1],new_inds);
}

bool BC4_RD(BlockSurface * to_blocks,
	const BlockSurface * from_blocks,
	const BlockSurface * baseline_blocks,
	const BlockSurface * activity_blocks,
	int lambdai,
	rrDXTCOptions options)
{
	SIMPLEPROFILE_SCOPE(bc4rd);

	BC4ValueType value_type = determine_value_type( from_blocks->pixelFormat );
	F32 lambda = lambdai * BC4RD_LAMBDA_SCALE;

	// 16-bit inputs have much larger SSD range; we use 12 bits internal, not a full 16,
	// but that's still a 256x increase
	switch ( value_type )
	{
	case BC4ValueType_UNorm:
		lambda *= 16.0f * 16.0f; // 16x from going 8->12 bits, squared because we work with squares of errors
		break;

	case BC4ValueType_SNorm:
		lambda *= 32.0f * 32.0f; // 16x from going 8 bits signed->13 bits signed, squared because we work with squares of errors
		break;

	case BC4ValueType_Alpha:
		// has our assumed-native 8-bit scale
		break;
	}

	BC4SourceFormat from_fmt = translate_input_format( from_blocks->pixelFormat );
	RR_ASSERT( from_fmt != BC4SourceFormat_Invalid );
	RR_ASSERT( to_blocks->pixelFormat == rrPixelFormat_BC3 ||
			   to_blocks->pixelFormat == rrPixelFormat_BC4U || to_blocks->pixelFormat == rrPixelFormat_BC4S ||
			   to_blocks->pixelFormat == rrPixelFormat_BC5U || to_blocks->pixelFormat == rrPixelFormat_BC5S );
	RR_ASSERT( baseline_blocks->pixelFormat == to_blocks->pixelFormat );
	RR_ASSERT( to_blocks->count == from_blocks->count );

	/*
	// FG TEST: disable BC3 alpha RDO entirely
	if ( to_blocks->pixelFormat == rrPixelFormat_BC3 )
	{
		for LOOP(bi,from_blocks->count)
		{
			const U8 * baseline_block_ptr = BlockSurface_SeekC(baseline_blocks,bi);
			U8 * output_bc4 = BlockSurface_Seek(to_blocks,bi);
			memcpy(output_bc4,baseline_block_ptr,8);
		}

		return true;
	}
	*/

	bool preserve_extremes = (options & rrDXTCOptions_BC345_PreserveExtremes) != 0;

	BC4EndpointEncInfo info;
	BC4_DescribeEncoding(value_type, &info.enc);

	info.lo = static_cast<F32>(info.enc.min_q);
	info.hi = static_cast<F32>(info.enc.max_q);
	info.scale = info.hi / static_cast<F32>(info.enc.max_deq); // scaling to go from dequant -> quant space

#ifdef __RADSSE2__
	info.lo_vec = VecF32x4(info.lo);
	info.hi_vec = VecF32x4(info.hi);

	info.variant[0].e0weight = _mm_setr_epi16(5,0,4,3,2,1,0,0);
	info.variant[0].e1weight = _mm_setr_epi16(0,5,1,2,3,4,0,0);
	info.variant[0].recip = _mm_set1_epi16((0x20000 + 4) / 5);
	info.variant[0].interp_consts = _mm_setr_epi16(0,0,0,0,0,0, (S16)info.enc.min_deq, (S16)info.enc.max_deq);

	info.variant[1].e0weight = _mm_setr_epi16(7,0,6,5,4,3,2,1);
	info.variant[1].e1weight = _mm_setr_epi16(0,7,1,2,3,4,5,6);
	info.variant[1].recip = _mm_set1_epi16((0x20000 + 6) / 7);
	info.variant[1].interp_consts = _mm_setzero_si128();
#endif

	int nblocks = from_blocks->count;
	RR_ASSERT( nblocks <= 16*1024 );

	const int kWindowSize = 256;
	RR_COMPILER_ASSERT( kWindowSize <= 32*1024 ); // no point having a window larger than max # blocks

	BC4WindowEntry past_block_window[kWindowSize];
	U16 window_inds[kWindowSize];
	int num_blocks_in_window = 0;

	for LOOP(i,kWindowSize)
		window_inds[i] = static_cast<U16>(i);

	// handle BC4 and BC5 with the same code by effectively treating BC5 as a double-size
	// array of BC4 blocks
	BC4SourceData source_pixels[2];
	int group_mask = 0;
	int group_shift = 0;

	if ( baseline_blocks->pixelFormat == rrPixelFormat_BC5U || baseline_blocks->pixelFormat == rrPixelFormat_BC5S )
	{
		group_mask = 1;
		group_shift = 1;
	}

	nblocks <<= group_shift;

	vector<BC4BlockInfo> infos;
	infos.reserve(nblocks);

	for LOOP(bi,nblocks)
	{
		// Figure out where we are in the current group
		int source_bi = bi >> group_shift;
		int block_in_group = bi & group_mask;

		const SingleFloatBlock4x4 * pActivity = BlockSurface_SeekC_SingleFloatBlock4x4(activity_blocks,source_bi);

		const U8 * baseline_block_ptr = BlockSurface_SeekC(baseline_blocks,source_bi) + block_in_group * 8;
		U8 * output_bc4 = BlockSurface_Seek(to_blocks,source_bi) + block_in_group * 8;

		if ( block_in_group == 0 ) // need to read new inputs?
		{
			const U8 * fmPtr = BlockSurface_SeekC(from_blocks,source_bi);
			if ( group_mask == 0 )
				BC4_ReadSourceFromBlock(&source_pixels[0],fmPtr,from_fmt);
			else
				BC5_ReadSourceFromBlock(&source_pixels[0],&source_pixels[1],fmPtr,from_fmt);
		}

		BC4BlockInfo blk_info;
		blk_info.pixels = source_pixels[block_in_group];
		blk_info.constraints.mask = 0;

		// assume we'll use the baseline for now
		BC4Block original_block;
		memcpy(original_block.bytes,baseline_block_ptr,8);

		// Set up constraints and canonicalize in "preserve extremes" mode
		if (preserve_extremes)
		{
			for LOOP(i,16)
			{
				S16 v = blk_info.pixels.values[i];
				if (v == info.enc.min_deq) blk_info.constraints.mask |= 1u << i;
				if (v == info.enc.max_deq) blk_info.constraints.mask |= (1u << 16) << i;
			}

			// Fox 4-interp blocks, make sure to always use the special indices for min/max.
			// Our index merge processing relies on it.
			canonicalize_for_constraints(&original_block,blk_info.constraints,info);
		}

		BC4Block chosen = original_block;

		BC4SourceData decoded;
		decoded.type = static_cast<U8>(value_type);
		bc4_decode(decoded.values,chosen.bytes,info);

		blk_info.baseline_D = bc4rd_D(decoded,blk_info.pixels,*pActivity);
		blk_info.D = blk_info.baseline_D;

		// if the chosen inds are all the same (i.e. solid block), don't need to try anything
		// (also don't want to because all-same indices are degenerate for LLS solve)
		U64 initial_inds = chosen.read_inds();
		U64 all_same = initial_inds ^ ((initial_inds << 3) & 0xffffFFFFffffull);
		if ( all_same < 8 )
		{
			memcpy(output_bc4,original_block.bytes,8);

			// flag block as single-color so we leave it alone
			// still have to push it to "infos" since that needs to be in 1:1 correspondence
			// with physical block IDs in the block surface
			blk_info.J = 0.0f;
			infos.push_back(blk_info);
			continue;
		}

		// take past blocks and try using their indices
		F32 best_D = blk_info.baseline_D;
		F32 best_J = bc4rd_J(best_D,bc4rd_R_uncompressed,lambda);;
		int best_index = -1;

		for LOOP(pb,num_blocks_in_window)
		{
			const BC4WindowEntry * wnd = &past_block_window[window_inds[pb]];
			U64 packed_inds = wnd->block.read_inds();

			// compute match distance we would have
			F32 match_R = bc4rd_matched_index_R(bi - wnd->most_recent_bi);

			// in case that matches our original inds, our baseline R was too high!
			// re-score it.
			if ( packed_inds == initial_inds )
			{
				F32 J = bc4rd_J(blk_info.baseline_D,match_R,lambda);
				if (J < best_J)
				{
					best_J = J;
					best_D = blk_info.baseline_D;
					best_index = pb;
					memcpy(chosen.bytes,original_block.bytes,8);
				}
			}
			else
			{
				U8 ep0, ep1;
				int variant = cached_lls_solve_both(&ep0, &ep1, blk_info, wnd->inds, info);
				if (variant >= 0)
				{
					bc4_decode_with_inds(decoded.values, ep0, ep1, info, wnd->predecoded_inds, variant);

					F32 D = bc4rd_D(decoded,blk_info.pixels,*pActivity);
					F32 J = bc4rd_J(D,match_R,lambda);
					if (J < best_J)
					{
						best_J = J;
						best_D = D;
						best_index = pb;
						chosen = BC4Block::make(ep0, ep1, packed_inds);;
					}
				}
			}
		}

		memcpy(output_bc4,chosen.bytes,8);
		blk_info.D = best_D;
		blk_info.J = best_J;
		infos.push_back(blk_info);

		// add to window/perform MTF
		if (best_index == -1)
		{
			if (num_blocks_in_window < kWindowSize) // grow window
				best_index = num_blocks_in_window++;
			else // replace LRU block
				best_index = kWindowSize - 1;

			// insert new block
			BC4WindowEntry * entry = &past_block_window[window_inds[best_index]];
			entry->block = chosen;

			U64 inds = chosen.read_inds();
			bc4_predecode_inds(entry->predecoded_inds, &inds);
			init_index_cache(&entry->inds[0], inds, 0, info);
			init_index_cache(&entry->inds[1], inds, 1, info);
		}

		// move to front
		RR_ASSERT(best_index >= 0 && best_index < kWindowSize);

		U16 best_slot = window_inds[best_index];
		memmove(window_inds + 1, window_inds, best_index * sizeof(window_inds[0]));
		window_inds[0] = best_slot;

		past_block_window[best_slot].most_recent_bi = bi;
	}

	RR_ASSERT(infos.size32() == nblocks);

	bc4rd_bottom_up_merge_indices(to_blocks,activity_blocks,lambda,group_shift,infos,info);

	return true;
}

RR_NAMESPACE_END

