// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "rrdxtcblock.h"
#include "newlz_simd.h"

#ifdef DO_BUILD_SSE4
#include <smmintrin.h>
#endif

RR_NAMESPACE_START

// 4 channel diff :
static RADINLINE U32 ColorBlock4x4_ComputeSAD_RGBA(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs);
static RADINLINE U32 ColorBlock4x4_ComputeSSD_RGBA(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs);

// previous block endpoints and original colors
//	trying new indices
// compute SSD
//	essentially decode BC1 then color diffs
	// 4 channel RGBA diff, but A's should be 255 before coming in here
static RADFORCEINLINE U32 BC1_Palette_SSD_RGBA(const rrColorBlock4x4 * colors,const rrColor32BGRA * palette,const U32 in_indices);
static RADFORCEINLINE U32 BC1_Palette_SAD_RGBA(const rrColorBlock4x4 * colors,const rrColor32BGRA * palette,const U32 in_indices);


// BC1 only needs RGB diff, but we do all four channels anyway
//	it's free in SIMD unless you wanted to tight pack to 3 RGB vectors
static RADINLINE U32 ColorBlock4x4_ComputeSAD_RGBA(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs)
{
	#ifdef __RADSSE2__

	// rrColorBlock4x4 = 64 bytes = four 16x8 vecs :

	__m128i lhs0 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 0 );
	__m128i lhs1 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 1 );
	__m128i lhs2 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 2 );
	__m128i lhs3 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 3 );

	__m128i rhs0 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 0 );
	__m128i rhs1 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 1 );
	__m128i rhs2 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 2 );
	__m128i rhs3 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 3 );
	
	// @@ this add reduction could be done with one less add I think
	//	we have four sad results that are 2x64
	//	we need to sum those 8
	//	could do that in 3 adds not 4 ?
	
	#if 0 // def DO_BUILD_SSE4
	
	// slower :
	
	__m128i h1 = _mm_packus_epi32(
		_mm_sad_epu8(lhs0,rhs0),
		_mm_sad_epu8(lhs1,rhs1) );
	__m128i h2 = _mm_packus_epi32(
		_mm_sad_epu8(lhs2,rhs2),
		_mm_sad_epu8(lhs3,rhs3) );
	__m128i h = _mm_add_epi32(h1,h2);
	
	U32 ret = hsum_epi32_sse2(h);
	
	#else
	
	// individual sub-SADs are at most 255*8 = 2040 so they comfortably fit in 16 bits

	// sums of two are <= 2*2040 = 4080
	__m128i sad01_2x64 = _mm_add_epi16(
		_mm_sad_epu8(lhs0,rhs0),
		_mm_sad_epu8(lhs1,rhs1) );
	
	__m128i sad23_2x64 = _mm_add_epi16(
		_mm_sad_epu8(lhs2,rhs2),
		_mm_sad_epu8(lhs3,rhs3) );
	
	// sum of four is <= 4*2040 = 8160
	__m128i sad_2x64 = _mm_add_epi16( sad01_2x64, sad23_2x64 );

	// can grab the individual elements slightly faster than with a final reduction
	U32 ret = _mm_cvtsi128_si32(sad_2x64) + _mm_extract_epi16(sad_2x64, 4);
    
    #endif
    
    /*
    {
		U32 err = 0;
		
		for(int i=0;i<16;i++)
		{
			err += rrColor32BGRA_DeltaSADRGBA( lhs.colors[i] , rhs.colors[i] );
		}

		RR_ASSERT( err == ret );	
    }
    /**/
    
    return ret;
    	
	#else

	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += rrColor32BGRA_DeltaSADRGBA( lhs.colors[i] , rhs.colors[i] );
	}
	
	return err;
	
	#endif
}

#ifdef __RADSSE2__

static RADFORCEINLINE __m128i sse_ssd_u8x16_to_u32x4( __m128i v1, __m128i v2 )
{
	#ifdef DO_BUILD_SSE4
	
	// alternative : (SSSE3)
	 //__m128i plus_minus = _mm_setr_epi8( 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1 );
	__m128i plus_minus = _mm_set1_epi16(0x1FF);
	__m128i sub16_1 = _mm_maddubs_epi16(_mm_unpacklo_epi8(v1, v2), plus_minus);
	__m128i sub16_2 = _mm_maddubs_epi16(_mm_unpackhi_epi8(v1, v2), plus_minus);
	
	#else
	
	__m128i sub8 = _mm_or_si128( _mm_subs_epu8(v1,v2), _mm_subs_epu8(v2,v1) );
	__m128i sub16_1 = _mm_unpacklo_epi8(sub8, _mm_setzero_si128() );
	__m128i sub16_2 = _mm_unpackhi_epi8(sub8, _mm_setzero_si128() );
	
	#endif
	
	__m128i squares32_1 = _mm_madd_epi16(sub16_1,sub16_1);
	__m128i squares32_2 = _mm_madd_epi16(sub16_2,sub16_2);
	
	return _mm_add_epi32(squares32_1,squares32_2);
}

#endif

static RADINLINE U32 ColorBlock4x4_ComputeSSD_RGBA(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs)
{
	#ifdef __RADSSE2__

	// rrColorBlock4x4 = 64 bytes = four 16x8 vecs :

	__m128i lhs0 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 0 );
	__m128i lhs1 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 1 );
	__m128i lhs2 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 2 );
	__m128i lhs3 = _mm_loadu_si128( ((__m128i *)lhs.colors) + 3 );

	__m128i rhs0 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 0 );
	__m128i rhs1 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 1 );
	__m128i rhs2 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 2 );
	__m128i rhs3 = _mm_loadu_si128( ((__m128i *)rhs.colors) + 3 );
	
	__m128i squares32 = sse_ssd_u8x16_to_u32x4(lhs0,rhs0);
	squares32 = _mm_add_epi32(squares32, sse_ssd_u8x16_to_u32x4(lhs1,rhs1) );
	squares32 = _mm_add_epi32(squares32, sse_ssd_u8x16_to_u32x4(lhs2,rhs2) );
	squares32 = _mm_add_epi32(squares32, sse_ssd_u8x16_to_u32x4(lhs3,rhs3) );
	
	U32 ret = hsum_epi32_sse2(squares32);
	
    /*
    {
		U32 err = 0;
		
		for(int i=0;i<16;i++)
		{
			err += rrColor32BGRA_DeltaSqrRGBA( lhs.colors[i] , rhs.colors[i] );
		}

		RR_ASSERT( err == ret );	
    }
    /**/
    
    return ret;
    	
	#else

	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += rrColor32BGRA_DeltaSqrRGBA( lhs.colors[i] , rhs.colors[i] );
	}
	
	return err;
	
	#endif
}


static RADFORCEINLINE U32 BC1_Palette_SAD_RGBA(const rrColorBlock4x4 * colors,const rrColor32BGRA * palette,const U32 in_indices)
{
	// 4 channel RGBA diff, but A's should be 255 before coming in here
	
	U32 indices = in_indices;
	
	#ifndef __RADSSE2__
	
	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += rrColor32BGRA_DeltaSADRGBA( colors->colors[i] , palette[indices&3] );
		indices >>= 2;
	}
	
	//indices = in_indices;
	return err;
	
	#else
	
	__m128i sad2x64_accum = _mm_setzero_si128();
	
	#ifdef DO_BUILD_SSE4

	// load palette vec and then select from it
	__m128i pal_vec = _mm_loadu_si128((const __m128i *)palette);

	// index_vec has indices pre-shifted 2 to make room for RGBA byte index in the bottom						
	__m128i index_vec = _mm_setr_epi32(	(indices<<2),(indices   ),(indices>>2),(indices>>4) );
	
	index_vec = _mm_and_si128( index_vec , _mm_set1_epi32(0x0C0C0C0C) );
	
	#endif
	
	for(int r=0;r<4;r++)
	{
		const rrColor32BGRA * row = colors->colors+r*4;
		
		__m128i v1 = _mm_loadu_si128((const __m128i *)row);
		
		/*
		RAD_ALIGN(rrColor32BGRA,rowpal,16) [4];
		rowpal[0] = palette[(indices   )&3];
		rowpal[1] = palette[(indices>>2)&3];
		rowpal[2] = palette[(indices>>4)&3];
		rowpal[3] = palette[(indices>>6)&3];
		__m128i v2ref = _mm_loadu_si128((const __m128i *)rowpal);
		/**/
		
		#ifdef DO_BUILD_SSE4
		
		// make 4 copies of each index byte
		__m128i index_broadcast = _mm_shuffle_epi8(index_vec, 
			_mm_setr_epi8(0,0,0,0, 4,4,4,4, 8,8,8,8, 12,12,12,12));

		// or in RGBA byte index :
		__m128i pal_index = _mm_or_si128(index_broadcast, _mm_set1_epi32(0x03020100));

		__m128i v2 = _mm_shuffle_epi8(pal_vec,pal_index);

		// indexes >>= 8 for next iter
		index_vec = _mm_srli_epi32(index_vec,8);
		
		#else
				
		__m128i v2 = _mm_set_epi32( palette[(indices>>6)&3].dw,
									palette[(indices>>4)&3].dw,
									palette[(indices>>2)&3].dw,
									palette[(indices>>0)&3].dw);
		
		indices >>= 8;
		
		#endif

		__m128i sad2x64 = _mm_sad_epu8(v1,v2);
		sad2x64_accum = _mm_add_epi64( sad2x64_accum, sad2x64 );
	}
	
	// add two 16-bit partial sums in the 64-bit halves :
    U32 sad_sse = _mm_cvtsi128_si32(_mm_add_epi64(sad2x64_accum, _mm_srli_si128(sad2x64_accum, 8)));		
		
	//RR_ASSERT_ALWAYS( err == sad_sse );
		
	return sad_sse;
	
	#endif
}

// previous block endpoints and original colors
//	trying new indices
// compute SSD
//	essentially decode BC1 then color diffs
static RADFORCEINLINE U32 BC1_Palette_SSD_RGBA(const rrColorBlock4x4 * colors,const rrColor32BGRA * palette,const U32 in_indices)
{
	// 4 channel RGBA diff, but A's should be 0/255 before coming in here
	// NOTE: this is actual RGBA SSD , no A bias
	
	U32 indices = in_indices;
	
	#ifndef __RADSSE2__
	
	U32 ssd_ref = 0;
	for(int i=0;i<16;i++)
	{
		ssd_ref += rrColor32BGRA_DeltaSqrRGBA( colors->colors[i] , palette[indices&3] );
		indices >>= 2;
	}
	//indices = in_indices;

	return ssd_ref;

	#else
	
	// SSE2 or 4
	
	#ifdef DO_BUILD_SSE4

	// load palette vec and then select from it
	__m128i pal_vec = _mm_loadu_si128((const __m128i *)palette);

	// index_vec has indices pre-shifted 2 to make room for RGBA byte index in the bottom						
	__m128i index_vec = _mm_setr_epi32(	(indices<<2),(indices   ),(indices>>2),(indices>>4) );
	
	index_vec = _mm_and_si128( index_vec , _mm_set1_epi32(0x0C0C0C0C) );
	
	#endif
	
	__m128i squares32 = _mm_setzero_si128();
		
	for(int r=0;r<4;r++)
	{
		const rrColor32BGRA * row = colors->colors+r*4;
				
		__m128i v1 = _mm_loadu_si128((const __m128i *)row);
		
		#ifdef DO_BUILD_SSE4
		
		// make 4 copies of each index byte
		__m128i index_broadcast = _mm_shuffle_epi8(index_vec, 
			_mm_setr_epi8(0,0,0,0, 4,4,4,4, 8,8,8,8, 12,12,12,12));

		// or in RGBA byte index :
		__m128i pal_index = _mm_or_si128(index_broadcast, _mm_set1_epi32(0x03020100));

		__m128i v2 = _mm_shuffle_epi8(pal_vec,pal_index);

		// indexes >>= 8 for next iter
		index_vec = _mm_srli_epi32(index_vec,8);
		
		#else
				
		__m128i v2 = _mm_set_epi32( palette[(indices>>6)&3].dw,
									palette[(indices>>4)&3].dw,
									palette[(indices>>2)&3].dw,
									palette[(indices>>0)&3].dw);
		
		indices >>= 8;
		
		#endif
		
		// note : does Alpha diffs too
		//	but all A's should be 255 here so should be the same
			
		squares32 = _mm_add_epi32(squares32, sse_ssd_u8x16_to_u32x4(v1,v2) );
	}
	
	U32 ssd = hsum_epi32_sse2(squares32);
	
	//RR_ASSERT_ALWAYS( ssd == ssd_ref );
	
	return ssd;
	
	#endif
}


//=============================================================

// WARNING : reject_endpoints_color_bbox much code dupe with endpoints_color_bbox_dsqr !!
//	

#ifdef __RADSSE2__

// reject returns true if too far away		
static RADINLINE bool reject_endpoints_color_bbox_sse2(const rrColor32BGRA * endpoints, const rrColor32BGRA * color_bbox_lohi, U32 min_d)
{
	// just load off the end of endpoints[] and color bbox , which are 8 byte arrays but we load 16
	//   that is definitely safe at the moment but be aware

	__m128i ep = _mm_loadu_si128( (const __m128i *) endpoints );
	// ep0 and ep1 in ep[0] and ep[1]

	// make endpoint midpoint :
	//__m128i ep_mid = _mm_avg_epu8(ep, _mm_shuffle_epi32(ep,_MM_SHUFFLE(1,1,1,1)) );
	__m128i ep_mid = _mm_avg_epu8(ep, _mm_srli_si128(ep,4) );

	// get ep_mid into ep[2]
	ep = _mm_unpacklo_epi64(ep, ep_mid);

	__m128i bbox_lohi = _mm_loadu_si128( (const __m128i *) color_bbox_lohi );
	__m128i bbox_lo = _mm_shuffle_epi32(bbox_lohi, _MM_SHUFFLE(0,0,0,0));
	__m128i bbox_hi = _mm_shuffle_epi32(bbox_lohi, _MM_SHUFFLE(1,1,1,1));
	
	__m128i ep_in_box = _mm_min_epu8( _mm_max_epu8( ep, bbox_lo), bbox_hi );
	
	// now dsqr on ep to ep_in_box :
	//	(only the bottom 2 dwords have anything useful)

	__m128i v1 = ep;
	__m128i v2 = ep_in_box;

	#ifdef DO_BUILD_SSE4
	
	// alternative : (SSSE3)
	 //__m128i plus_minus = _mm_setr_epi8( 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1 );
	__m128i plus_minus = _mm_set1_epi16(0x1FF);
	__m128i sub16_1 = _mm_maddubs_epi16(_mm_unpacklo_epi8(v1, v2), plus_minus);
	__m128i sub16_2 = _mm_maddubs_epi16(_mm_unpackhi_epi8(v1, v2), plus_minus);
	
	#else
	
	__m128i sub8 = _mm_or_si128( _mm_subs_epu8(v1,v2), _mm_subs_epu8(v2,v1) );
	__m128i sub16_1 = _mm_unpacklo_epi8(sub8, _mm_setzero_si128() );
	__m128i sub16_2 = _mm_unpackhi_epi8(sub8, _mm_setzero_si128() );
	
	#endif
	
	// sub16_1 has the two endpoint distances
	// sub16_2 has midpoint distance
		
	__m128i squares32_1 = _mm_madd_epi16(sub16_1,sub16_1);
	// I want d0 = squares32_1[0] + squares32_1[1]
	// d1 = squares32_1[2] + squares32_1[3];
	
	// _mm_hadd_epi32 ? nah slower
	__m128i squares32 = _mm_add_epi32( squares32_1, _mm_srli_si128(squares32_1, 4) );
	
	// squares32_2[0] & squares32_2[2] are what I want
	// I want the min of the two
	
	#ifdef DO_BUILD_SSE4
	
	// _mm_min_epi32 is SSE4
	__m128i min_d_vec = _mm_min_epi32( squares32 , _mm_srli_si128(squares32,8) );
	U32 d =  _mm_cvtsi128_si32(min_d_vec);
	
	#else
	
	U32 d0 = _mm_cvtsi128_si32(squares32);
	U32 d1 = _mm_cvtsi128_si32( _mm_srli_si128(squares32,8) );
	U32 d = RR_MIN(d0,d1);
	
	#endif
	
	if ( d <= min_d )
		return false;
				
	// @@ check midpoint of the endpoints also, and then use a tighter distance?
	//	how is it that it's important to consider encodings where neither endpoint
	//	 is close to the color bbox !?
	// with midpoint a dsqr of 200 is nearly a quality nop
	//	that's a step of 14 in one component
	//	in 565 endpoints the step is 8 or 4
	
	squares32_1 = _mm_madd_epi16(sub16_2,sub16_2);
	
	// squares32_1[0] + squares32_1[1]
	// _mm_hadd_epi32 is slower
	squares32 = _mm_add_epi32( squares32_1, _mm_srli_si128(squares32_1,4) );
	
	U32 midpoint_d = _mm_cvtsi128_si32(squares32);
		
	if ( midpoint_d > min_d )
	{
		return true;
	}
		
	// some d <= min_d
	return false;
}

#endif
	
// reject returns true if too far away		
static bool reject_endpoints_color_bbox_scalar(const rrColor32BGRA * endpoints, const rrColor32BGRA * color_bbox_lohi, U32 min_d)
{
	// find distance of candidate vq endpoints to the color bbox of this block
	//	one or the other vq endpoint must be close to the color bbox
	rrColor32BGRA ep0 = endpoints[0];
	rrColor32BGRA ep1 = endpoints[1];
	rrColor32BGRA color_bbox_lo = color_bbox_lohi[0];
	rrColor32BGRA color_bbox_hi = color_bbox_lohi[1];
	
	// RGB part only :
	RR_ASSERT( color_bbox_lo.u.a == 255 );
	RR_ASSERT( color_bbox_hi.u.a == 255 );
	RR_ASSERT( ep0.u.a == 255 );
	RR_ASSERT( ep1.u.a == 255 );

	rrColor32BGRA ep0_in_box;
	rrColor32BGRA ep1_in_box;
	
	ep0_in_box.u.b = RR_CLAMP(ep0.u.b,color_bbox_lo.u.b,color_bbox_hi.u.b);
	ep0_in_box.u.g = RR_CLAMP(ep0.u.g,color_bbox_lo.u.g,color_bbox_hi.u.g);
	ep0_in_box.u.r = RR_CLAMP(ep0.u.r,color_bbox_lo.u.r,color_bbox_hi.u.r);
	
	ep1_in_box.u.b = RR_CLAMP(ep1.u.b,color_bbox_lo.u.b,color_bbox_hi.u.b);
	ep1_in_box.u.g = RR_CLAMP(ep1.u.g,color_bbox_lo.u.g,color_bbox_hi.u.g);
	ep1_in_box.u.r = RR_CLAMP(ep1.u.r,color_bbox_lo.u.r,color_bbox_hi.u.r);
	
	U32 d0 = rrColor32BGRA_DeltaSqrRGB(ep0,ep0_in_box);
	U32 d1 = rrColor32BGRA_DeltaSqrRGB(ep1,ep1_in_box);
	
	// vq endpoints one or the other must be close to bbox
	U32 d = RR_MIN(d0,d1);
	
	if ( d <= min_d )
		return false;
				
	// @@ check midpoint of the endpoints also, and then use a tighter distance?
	//	how is it that it's important to consider encodings where neither endpoint
	//	 is close to the color bbox !?
	// with midpoint a dsqr of 200 is nearly a quality nop
	//	that's a step of 14 in one component
	//	in 565 endpoints the step is 8 or 4
	
	#if 1
	{
		rrColor32BGRA midpoint,midpoint_in_box;
		// +1 to match _mm_avg_epu8
		midpoint.u.b = (ep0.u.b + ep1.u.b +1)>>1;
		midpoint.u.g = (ep0.u.g + ep1.u.g +1)>>1;
		midpoint.u.r = (ep0.u.r + ep1.u.r +1)>>1;
		
		midpoint_in_box.u.b = RR_CLAMP(midpoint.u.b,color_bbox_lo.u.b,color_bbox_hi.u.b);
		midpoint_in_box.u.g = RR_CLAMP(midpoint.u.g,color_bbox_lo.u.g,color_bbox_hi.u.g);
		midpoint_in_box.u.r = RR_CLAMP(midpoint.u.r,color_bbox_lo.u.r,color_bbox_hi.u.r);
		
		U32 midpoint_d = rrColor32BGRA_DeltaSqrRGB(midpoint,midpoint_in_box);
		if ( midpoint_d > min_d )
		{
			return true;
		}
	}
	#else
	// don't check midpoint, use larger min_d
			return true;
	#endif
	
	// some d <= min_d
	return false;
}

#ifdef __RADSSE2__
#define reject_endpoints_color_bbox reject_endpoints_color_bbox_sse2
#else
#define reject_endpoints_color_bbox reject_endpoints_color_bbox_scalar
#endif


RR_NAMESPACE_END
