// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.


#include "newlz_simd.h"
#include "vec128.inl"
#ifdef DO_BUILD_SSE4
#include <smmintrin.h>
#endif

RR_NAMESPACE_START

static RADFORCEINLINE F32 VQD(const rrColorBlock4x4 & colors,const rrColor32BGRA palette[4],const U32 in_indices,const FourFloatBlock4x4 & activity);

#if 0

// straight SSD error, no activity mask
// go back to this if you want to tune for RMSE :
	
static U32 VQD(const rrColorBlock4x4 & colors,const rrDXT1Block & dxtb,const FourFloatBlock4x4 & activity,rrDXT1PaletteMode pal_mode)
{
	//U32 ssd = DXT1_ComputeSSD_RGB(colors,dxtb,pal_mode);
	U32 ssd = DXT1_ComputeSSD_RGBA(colors,dxtb,pal_mode);
	U32 D = 2 * ssd;
	return D;
}	

#else
	
// VQD you can fiddle with and try different ideas :	
static F32 VQD_Research(const rrColorBlock4x4 & colors,const rrColor32BGRA palette[4],const U32 in_indices,const FourFloatBlock4x4 & activity)
{
	F32 sum = 0;
	U32 indices = in_indices;
	
	/*
	rrColor4I src_dc = { };
	rrColor4I dst_dc = { };
	F32 activity_sum = 0;
	*/
	
	for LOOP(i,16)
	{	
		const rrColor32BGRA dxtbc = palette[indices&3]; indices >>= 2;
	
		/*
		rrColor4I_Add(&src_dc,colors.colors[i]);
		rrColor4I_Add(&dst_dc,dxtbc);
		activity_sum += activity.values[i];
		*/
		
		rrColor32BGRA c = colors.colors[i];
		const rrColor4F & act = activity.values[i];
		
		sum += act.r * fsquare((F32)((S32)c.u.r - dxtbc.u.r));
		sum += act.g * fsquare((F32)((S32)c.u.g - dxtbc.u.g));
		sum += act.b * fsquare((F32)((S32)c.u.b - dxtbc.u.b));
		sum += act.a * fsquare((F32)((S32)c.u.a - dxtbc.u.a));
			
		#if 0
		// rdotestset1_activity_masked_second_cut_Wed_Aug_21_17_17_28_2019
		
		U32 ssd = rrColor32BGRA_DeltaSqrRGB( colors.colors[i] , dxtbc );
		
		// assumes PreprocessActivity has been done
		F32 scale = activity.values[i];
		
		sum += ssd * scale;
		#endif
		
		#if 0
		
		// wants raw activity, NOT PreprocessActivity
		
		// rdotestset1_thresholded_sad_squared_Thu_Aug_22_10_31_55_2019
		
		// -> maybe this is slightly perceptually better than SSD version above
		
		// other option I like is SAD with activity as a soft thresh
		//	this looks good perceptually also
		//	this is obviously much worse in RMSE because you want SSD to optimize for RMSE (this could be a red herring)
		//	I believe that I see some anomalies from this (without the squaring at the end)
		//	basically the SAD linear error occasionally lets through individual big pixel changes
		//	 that are quite visible perceptually
		//	SSD does a better job of strongly penalizing big changes
		
		// Thresholding SAD here isn't quite right
		//	in PSNR-HVS-M you threshold only *ACs* not *DCs*
		//	I could fix that by adding an extra error term for DC delta
		//	(now added below)
		
		U32 sad = rrColor32BGRA_DeltaSADRGB( colors.colors[i] , dxtbc );
		
		F32 x = sad * 6.0; // scale sad vs activity ; x is compared to T
		
		// sad & activity are both linear in pixel values
		
		F32 a = activity.values[i];
		F32 T = RR_MIN(a,32.0); // clamp huge activities
		
		F32 K = 1.f; // tweak
		T += K;
		// add K to T, so if T is zero the map is the identity
		
		// sad in [0,T] -> [0,K]
		//	>= T -> K + more
		
		F32 y;
		if ( x < T )
		{
			y = x * K / T;
		}
		else
		{
			y = K + (x - T);
		}	
		
		// scaling for J :
		//y *= 8.0;
		
		// try squaring to increase weighting of large errors
		// AND need a divisor by activity as well
		//	use a weaker activity divisor here because we already thresholded
		//sum += y * y / (1 + a);
		y = y * y / (1 + T);
		y *= 0.7; // scaling for J
		
		sum += y;
		
		#endif
	}
	
	#if 0
	// @@ add another term for DC delta to prevent overall color shift
	//	DC delta should be activity masked but not as strongly
	//	and definitely not thresholded
	//	(maybe log(activity) ?)
	// I can't really see much perceptual benefit from this, but it does help RMSE score a lot
	//	(if used with sad-squared; only helps a little with ssd)
	//	so that's a win
	{
	// src_dc are *16 of average color
	U32 dc_ssd = rrColor4I_DeltaSqrRGB(src_dc,dst_dc);
	// @@ lots of constants and possibilities here
	F32 dc_activity = rrlog2(1.0 + activity_sum); // @@ log or linear in activity ?
	F32 dc_term = dc_ssd / (1.0 + dc_activity);
	sum += dc_term;
	}
	#endif
	
	//U32 D = (U32)(sum + 0.5);
		
	return sum;
}

// VQD optimized not easy to fiddle with
// VQD is just SSD with a scaling on each pixel
//  we can do the SIMD SSD like BC1_Palette_SSD_RGB
//	and just mul through by activity
static RADFORCEINLINE F32 VQD(const rrColorBlock4x4 & colors,const rrColor32BGRA palette[4],const U32 in_indices,const SingleFloatBlock4x4 & activity)
{
	F32 sum = 0;
	U32 indices = in_indices;
	
	#ifndef __RADSSE2__
	{
	
	// scalar fallback
	
	for LOOP(i,16)
	{
		const rrColor32BGRA dxtbc = palette[indices&3]; indices >>= 2;
			
		rrColor32BGRA c = colors.colors[i];
		
		#if 0 // 4F
		const rrColor4F & act = activity.values[i];
		
		sum += act.r * fsquare((F32)((S32)c.u.r - dxtbc.u.r));
		sum += act.g * fsquare((F32)((S32)c.u.g - dxtbc.u.g));
		sum += act.b * fsquare((F32)((S32)c.u.b - dxtbc.u.b));
		sum += act.a * fsquare((F32)((S32)c.u.a - dxtbc.u.a));
		#endif
		
		// 1F :
		const F32 act = activity.values[i];
		
		sum += act * rrColor32BGRA_DeltaSqrRGBA(c,dxtbc);
	}
	
	}
	#else
	{
	
	// SSE2 or 4
	
	#ifdef DO_BUILD_SSE4

	// load palette vec and then select from it
	__m128i pal_vec = _mm_loadu_si128((const __m128i *)palette);

	// index_vec has indices pre-shifted 2 to make room for RGBA byte index in the bottom						
	__m128i index_vec = _mm_setr_epi32(	(indices<<2),(indices   ),(indices>>2),(indices>>4) );
	
	index_vec = _mm_and_si128( index_vec , _mm_set1_epi32(0x0C0C0C0C) );
	
	#endif
	
	__m128 accum_f32 = _mm_setzero_ps();
		
	for(int r=0;r<4;r++)
	{
		const rrColor32BGRA * row = colors.colors+r*4;
		//const rrColor4F * activity_row = activity.values + r*4;
		const F32 * activity_row = activity.values + r*4;
		
		// load 4 colors :		
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
		
		__m128i sub8 = _mm_or_si128( _mm_subs_epu8(v1,v2), _mm_subs_epu8(v2,v1) );
		__m128i sub16_1 = _mm_and_si128(sub8, _mm_set1_epi16(0xff)); // 16-bit: R, B, R, B, ...
		__m128i sub16_2 = _mm_srli_epi16(sub8, 8); // 16-bit: G, A, G, A, ...
		
		#if 1 // activity 1F

		// this squares and horizontally adds pairs
		//	we go from 16 bits * 4 colors channels * 4 pixels
		//	-> 32 bits with {R+B} (squares32_1), {G+A} (squares32_2)
		__m128i squares32_1 = _mm_madd_epi16(sub16_1,sub16_1);
		__m128i squares32_2 = _mm_madd_epi16(sub16_2,sub16_2);

		// add the halves together to get R+G+B+A
		__m128i squares32 = _mm_add_epi32(squares32_1, squares32_2);

		// convert to float
		__m128 squares_f32 = _mm_cvtepi32_ps(squares32);

		// sum the values scaled by activity
		__m128 activity_vec = _mm_loadu_ps(activity_row);

		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares_f32, activity_vec));
		
		#endif
		
		#if 0 // activity 4F
		
		// RGBA squares , in 16 bit = 4*4 values in two vectors
		sub16_1 = _mm_mullo_epi16(sub16_1,sub16_1);
		sub16_2 = _mm_mullo_epi16(sub16_2,sub16_2);

		// expand 16 to 32 and convert to floats :
		__m128 squares32_1 = _mm_cvtepi32_ps( _mm_and_si128(sub16_1, _mm_set1_epi32(0xffff)) ); // R only
		__m128 squares32_2 = _mm_cvtepi32_ps( _mm_and_si128(sub16_2, _mm_set1_epi32(0xffff)) ); // G only
		__m128 squares32_3 = _mm_cvtepi32_ps( _mm_srli_epi32(sub16_1, 16) ); // B only
		__m128 squares32_4 = _mm_cvtepi32_ps( _mm_srli_epi32(sub16_2, 16) ); // A only
		
		// mult by activity (per color) and accumulate :
		// _mm_fmadd_ps ?
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_1, _mm_loadu_ps((const float *)(activity_row+0))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_2, _mm_loadu_ps((const float *)(activity_row+1))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_3, _mm_loadu_ps((const float *)(activity_row+2))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_4, _mm_loadu_ps((const float *)(activity_row+3))) );	
	
		#endif
	}
	
	// horizontal sum across the lanes of accum_f32 :
	__m128 x = accum_f32;
    __m128 y = _mm_add_ps(x, _mm_shuffle_ps(x,x, _MM_SHUFFLE(1,0,3,2)) );
    __m128 z = _mm_add_ps(y, _mm_shuffle_ps(y,y, _MM_SHUFFLE(2,3,0,1)) );
    // horizontal sum, same value in every lane now
    
    // _mm_cvtss_f32 is not really an instruction, its not SSE4, I just don't have it in my header
    // a "float" is just the low element of an xmm register
    F32 sum_sse = _mm_cvtss_f32(z);
    
	//RR_ASSERT_ALWAYS( ssd == ssd_ref );
	//rrprintfvar(sum);
	//rrprintfvar(sum_sse);
	//RR_ASSERT( fequal(sum,sum_sse,0.55f) );

	sum = sum_sse;

	}
	#endif
	
	return sum;
}

// colors-colors variant of VQD :
//	!! BEWARE !! LOTS OF CODE DUPE WITH ABOVE PRIMARY VARIANT
// does RGBA SSD
//	rrColorBlock4x4 is BGRA but that doesn't actually matter , RGBA will give the same result
static RADFORCEINLINE F32 VQD(const rrColorBlock4x4 & colors1,const rrColorBlock4x4 & colors2,const FourFloatBlock4x4 & activity)
{
	F32 sum = 0;
	
	#ifndef __RADSSE2__
	{
	
	// scalar fallback
	
	for LOOP(i,16)
	{
		rrColor32BGRA c1 = colors1.colors[i];
		rrColor32BGRA c2 = colors2.colors[i];
		const rrColor4F & act = activity.values[i];
		
		sum += act.r * fsquare((F32)((S32)c1.u.r - c2.u.r));
		sum += act.g * fsquare((F32)((S32)c1.u.g - c2.u.g));
		sum += act.b * fsquare((F32)((S32)c1.u.b - c2.u.b));
		sum += act.a * fsquare((F32)((S32)c1.u.a - c2.u.a));
	}
	
	}
	#else
	{
	
	// SSE2 or 4
		
	__m128 accum_f32 = _mm_setzero_ps();
		
	for(int r=0;r<4;r++)
	{
		const rrColor32BGRA * row1 = colors1.colors+r*4;
		const rrColor32BGRA * row2 = colors2.colors+r*4;
		const rrColor4F * activity_row = activity.values + r*4;
		
		// load 4 colors :		
		__m128i v1 = _mm_loadu_si128((const __m128i *)row1);
		__m128i v2 = _mm_loadu_si128((const __m128i *)row2);
				
		// note : does Alpha diffs too
		//	but all A's should be 255 here so should be the same
		
		
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
				
		// activity 4F :
		
		// RGBA squares , in 16 bit = 4*4 values in two vectors
		sub16_1 = _mm_mullo_epi16(sub16_1,sub16_1);
		sub16_2 = _mm_mullo_epi16(sub16_2,sub16_2);

		// expand 16 to 32 and convert to floats :
		__m128 squares32_1 = _mm_cvtepi32_ps( _mm_unpacklo_epi16(sub16_1, _mm_setzero_si128()) );
		__m128 squares32_2 = _mm_cvtepi32_ps( _mm_unpackhi_epi16(sub16_1, _mm_setzero_si128()) );
		__m128 squares32_3 = _mm_cvtepi32_ps( _mm_unpacklo_epi16(sub16_2, _mm_setzero_si128()) );
		__m128 squares32_4 = _mm_cvtepi32_ps( _mm_unpackhi_epi16(sub16_2, _mm_setzero_si128()) );
		
		// mult by activity (per color) and accumulate :
		// _mm_fmadd_ps ?
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_1, _mm_loadu_ps((const float *)(activity_row+0))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_2, _mm_loadu_ps((const float *)(activity_row+1))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_3, _mm_loadu_ps((const float *)(activity_row+2))) );
		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares32_4, _mm_loadu_ps((const float *)(activity_row+3))) );
	}
	
	// horizontal sum across the lanes of accum_f32 :
	__m128 x = accum_f32;
    __m128 y = _mm_add_ps(x, _mm_shuffle_ps(x,x, _MM_SHUFFLE(1,0,3,2)) );
    __m128 z = _mm_add_ps(y, _mm_shuffle_ps(y,y, _MM_SHUFFLE(2,3,0,1)) );
    // horizontal sum, same value in every lane now
    
    // _mm_cvtss_f32 is not really an instruction, its not SSE4, I just don't have it in my header
    // a "float" is just the low element of an xmm register
    F32 sum_sse = _mm_cvtss_f32(z);
    
	//RR_ASSERT_ALWAYS( ssd == ssd_ref );
	//rrprintfvar(sum);
	//rrprintfvar(sum_sse);
	//RR_ASSERT( fequal(sum,sum_sse,0.55f) );

	sum = sum_sse;

	}
	#endif
	
	return sum;
}

// colors-colors variant of VQD :
//	!! BEWARE !! LOTS OF CODE DUPE WITH ABOVE PRIMARY VARIANT
// does RGBA SSD
//	rrColorBlock4x4 is BGRA but that doesn't actually matter , RGBA will give the same result
static RADFORCEINLINE F32 VQD(const rrColorBlock4x4 & colors1,const rrColorBlock4x4 & colors2,const SingleFloatBlock4x4 & activity)
{
	F32 sum = 0;
	
	#ifndef __RADSSE2__
	{
	
	// scalar fallback
	
	for LOOP(i,16)
	{
		rrColor32BGRA c1 = colors1.colors[i];
		rrColor32BGRA c2 = colors2.colors[i];
		const F32 act = activity.values[i];
		
		sum += act * rrColor32BGRA_DeltaSqrRGBA(c1,c2);
	}
	
	}
	#else
	{
	
	// SSE2 or 4
		
	__m128 accum_f32 = _mm_setzero_ps();
		
	for(int r=0;r<4;r++)
	{
		const rrColor32BGRA * row1 = colors1.colors+r*4;
		const rrColor32BGRA * row2 = colors2.colors+r*4;
		const F32 * activity_row = activity.values + r*4;
		
		// load 4 colors :		
		__m128i v1 = _mm_loadu_si128((const __m128i *)row1);
		__m128i v2 = _mm_loadu_si128((const __m128i *)row2);
				
		// note : does Alpha diffs too
		//	but all A's should be 255 here so should be the same
		
		__m128i sub8 = _mm_or_si128( _mm_subs_epu8(v1,v2), _mm_subs_epu8(v2,v1) );
		__m128i sub16_1 = _mm_and_si128(sub8, _mm_set1_epi16(0xff)); // 16-bit: R, B, R, B, ...
		__m128i sub16_2 = _mm_srli_epi16(sub8, 8); // 16-bit: G, A, G, A, ...
		
		// activity F32
		
		__m128i squares32_1 = _mm_madd_epi16(sub16_1,sub16_1); // {R*R+B*B} sums
		__m128i squares32_2 = _mm_madd_epi16(sub16_2,sub16_2); // {G*G+A*A} sums
		__m128i squares32 = _mm_add_epi32(squares32_1, squares32_2);

		// convert to float
		__m128 squares_f32 = _mm_cvtepi32_ps(squares32);

		// sum the values scaled by activity
		__m128 activity_vec = _mm_loadu_ps(activity_row);

		accum_f32 = _mm_add_ps(accum_f32, _mm_mul_ps(squares_f32, activity_vec));
	}
	
	// horizontal sum across the lanes of accum_f32 :
	__m128 x = accum_f32;
    __m128 y = _mm_add_ps(x, _mm_shuffle_ps(x,x, _MM_SHUFFLE(1,0,3,2)) );
    __m128 z = _mm_add_ps(y, _mm_shuffle_ps(y,y, _MM_SHUFFLE(2,3,0,1)) );
    // horizontal sum, same value in every lane now
    
    // _mm_cvtss_f32 is not really an instruction, its not SSE4, I just don't have it in my header
    // a "float" is just the low element of an xmm register
    F32 sum_sse = _mm_cvtss_f32(z);
    
	//RR_ASSERT_ALWAYS( ssd == ssd_ref );
	//rrprintfvar(sum);
	//rrprintfvar(sum_sse);
	//RR_ASSERT( fequal(sum,sum_sse,0.55f) );

	sum = sum_sse;

	}
	#endif
	
	return sum;
}

// For S16 pixels _that don't use the whole range_ (so we can subtract them w/o overflows)
static RADFORCEINLINE F32 VQD(const S16 colors1[16],const S16 colors2[16],const SingleFloatBlock4x4 & activity)
{
	F32 sum = 0;

	#ifndef __RADSSE2__

	// scalar fallback

	for LOOP(i,16)
	{
		int diff = colors1[i] - colors2[i];
		// assumes PreprocessActivity has been done
		F32 scale = activity.values[i];

		sum += (diff * diff) * scale;
	}

	#else

	// SSE2 or 4

	// load all 16 pixels and diff them
	Vec128 diff01_16 = _mm_sub_epi16(load128u(colors1 + 0), load128u(colors2 + 0));
	Vec128 diff23_16 = _mm_sub_epi16(load128u(colors1 + 8), load128u(colors2 + 8));

	// Sign-extend to 32-bit float
	VecF32x4 diff0_f32 = VecF32x4::from_int32(sext16to32_lo(diff01_16));
	VecF32x4 diff1_f32 = VecF32x4::from_int32(sext16to32_hi(diff01_16));
	VecF32x4 diff2_f32 = VecF32x4::from_int32(sext16to32_lo(diff23_16));
	VecF32x4 diff3_f32 = VecF32x4::from_int32(sext16to32_hi(diff23_16));

	// Square and multiply by activity values
	VecF32x4 ssd0_f32 = (diff0_f32 * diff0_f32) * VecF32x4::loadu(activity.values + 0);
	VecF32x4 ssd1_f32 = (diff1_f32 * diff1_f32) * VecF32x4::loadu(activity.values + 4);
	VecF32x4 ssd2_f32 = (diff2_f32 * diff2_f32) * VecF32x4::loadu(activity.values + 8);
	VecF32x4 ssd3_f32 = (diff3_f32 * diff3_f32) * VecF32x4::loadu(activity.values + 12);

	// Sum rows
	VecF32x4 sum01_f32 = ssd0_f32 + ssd1_f32;
	VecF32x4 sum23_f32 = ssd2_f32 + ssd3_f32;
	VecF32x4 sum_f32 = sum01_f32 + sum23_f32;

	sum = sum_f32.sum_across().scalar_x();

	#endif

	return sum;
}

#endif // VQD

/*
// ! make sure you turn off threading when accumulating totals
double g_total_vqd = 0;
double g_total_ssd = 0;
double g_total_sad = 0;

void log_total_vqd()
{
	F32 reference = 2 * g_total_ssd;
	F32 total_sad_scale = reference / g_total_sad;
	F32 total_vqd_scale = reference / g_total_vqd;
	rrprintfvar(total_sad_scale);
	rrprintfvar(total_vqd_scale);
}
*/

RR_NAMESPACE_END
