// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#include "rrdxtcompresshelp.h"
#include "rrcolorvecc.h"
#include "templates/rrstl.h"
#include "rrlog.h"
#include "rrlogutil.h"

RR_NAMESPACE_START

/*

Compress_SingleColor_Compact restricts quantized endpoints to be within +-1 of the original color

*/

void Compress_SingleColor_Compact_4C(rrDXT1Block * dxtBlock,
							const rrColor32BGRA & c)
{
	rrDXT1Block & bco = *dxtBlock;
	bco.c0.u.r = BC1_Opt5Tab_4C[c.u.r*2+0];
	bco.c0.u.g = BC1_Opt6Tab_4C[c.u.g*2+0];
	bco.c0.u.b = BC1_Opt5Tab_4C[c.u.b*2+0];
	bco.c1.u.r = BC1_Opt5Tab_4C[c.u.r*2+1];
	bco.c1.u.g = BC1_Opt6Tab_4C[c.u.g*2+1];
	bco.c1.u.b = BC1_Opt5Tab_4C[c.u.b*2+1];
	bco.indices = 0xAAAAAAAA;
	
	// note : bco.c0.w == bco.c1.w is totally possible
	//	(happens for example when the color is 0 or 255)
	// that's a degenerate block that uses 3-color mode!
	// indices 0xAAA (= 1/2 interp) works fine for that too, so leave it
	
	if (bco.c0.w < bco.c1.w)
	{
		RR_NAMESPACE::swap(bco.c0.w, bco.c1.w);
		bco.indices = 0xFFFFFFFF; // change 1/3 to 2/3
	}
}

void Compress_SingleColor_Compact_3C(rrDXT1Block * dxtBlock,
							const rrColor32BGRA & c)
{
	rrDXT1Block & bco = *dxtBlock;
	bco.c0.u.r = BC1_Opt5Tab_3C[c.u.r*2+0];
	bco.c0.u.g = BC1_Opt6Tab_3C[c.u.g*2+0];
	bco.c0.u.b = BC1_Opt5Tab_3C[c.u.b*2+0];
	bco.c1.u.r = BC1_Opt5Tab_3C[c.u.r*2+1];
	bco.c1.u.g = BC1_Opt6Tab_3C[c.u.g*2+1];
	bco.c1.u.b = BC1_Opt5Tab_3C[c.u.b*2+1];
	bco.indices = 0xAAAAAAAA; // the 1/2 interp
	
	Make3ColorOrder(bco.c0,bco.c1);
}

void Compress_SingleColor_Compact(rrDXT1Block * pdxtBlock,
							const rrColor32BGRA & c)
{
	//iTable_init();
	
	// old behavior, just do 4C:
	Compress_SingleColor_Compact_4C(pdxtBlock,c);
	
	#if 0
	// try the 3c color too?
	// does help a micro bit
	// frymire :
	// 6.2060
	// 6.2046
	// linear ramp :
	// 1.4694
	// 1.445
	{	
		rrColor32BGRA palette[4];
		DXT1_ComputePalette(pdxtBlock->c0,pdxtBlock->c1,palette,rrDXT1PaletteMode_NoAlpha);
		rrColor32BGRA interp4c = palette[pdxtBlock->indices&3];
		
		rrDXT1Block block3c;
		Compress_SingleColor_Compact_3C(&block3c,c);
		
		DXT1_ComputePalette(block3c.c0,block3c.c1,palette,rrDXT1PaletteMode_NoAlpha);
		rrColor32BGRA interp3c = palette[2];
		
		U32 d4c = rrColor32BGRA_DeltaSqrRGB(c,interp4c);
		U32 d3c = rrColor32BGRA_DeltaSqrRGB(c,interp3c);
		
		if ( d3c <= d4c ) // on == prefer 3c? is 3c interpolation lower error on NV5x ?
		{
			*pdxtBlock = block3c;
		}	
	}
	#endif
}

bool Compress_EndPointsQ_NoReverse(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor565Bits & c0,const rrColor565Bits & c1)
{
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(c0,c1,palette,mode);
				
	U32 err;
	U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
	if ( err < *pError )
	{
		*pError = err;
		pBlock->c0 = c0;
		pBlock->c1 = c1;
		pBlock->indices = indices;
		
		return true;
	}
	
	return false;
}
		
bool Compress_EndPoints(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	rrColor565Bits end1q = rrColor565Bits_Quantize(end1);
	rrColor565Bits end2q = rrColor565Bits_Quantize(end2);
	
	// solid color should have already been done?
	//	we checked for flat 8-bit but this is flat in 565
	//	 should step off to next wider in 565
	if ( end1q.w == end2q.w )
	{
		// degenerate endpoints in 565
		// source block is not flat (already early-outed that)
	
		#if 0
		// this sucks ; we already did SingleColor_Compact with average color
		//	it is occasionally a tiny bit better to do it with one of hte endpoints
		//	but only a little
		// SingleColor_Compact doesn't handle alpha at all
		//	 will just come out as a large error if any pixels were transparent
		
		rrDXT1Block block;
		Compress_SingleColor_Compact(&block,end1);
		U32 err = DXT1_ComputeSSD_OneBitTransparent(colors,block,mode);
		// hmm does happen;
		//	Compress_SingleColor_Optimal has been done already
		//	but somehow Compress_SingleColor_Compact is better?
		//	(impossible on a true flat block, I'm not actually flat in 8-bit, just flat in 565)
		//RR_ASSERT( err >= *pError );
		if ( err < *pError )
		{
			*pError = err;
			*pBlock = block;
			return true;
		}
		else
		{
			return false;
		}
		#endif
		
		#if 0
		
		//if ( end1.dw == end2.dw ) // don't bother
		if ( rrColor32BGRA_EqualsRGB(end1,end2) ) // don't bother
			return false;
		
		// step endpoints apart in quantized space
		//	along the largest axis of the delta :
		
		int dr = RR_ABS( end1.u.r - end2.u.r );
		int dg = RR_ABS( end1.u.g - end2.u.g );
		int db = RR_ABS( end1.u.b - end2.u.b );
		
		// must have some non-zero axis :
		RR_ASSERT( (dr+dg+db) != 0 );
		
		if ( dr >= dg && dr >= db )
		{
			int qr = end1q.u.r; // == end2q
			if ( qr > 0  ) end1q.u.r--;
			if ( qr < 31 ) end2q.u.r++;
		}
		else if ( dg >= db )
		{
			int qg = end1q.u.g; // == end2q
			if ( qg > 0  ) end1q.u.g--;
			if ( qg < 63 ) end2q.u.g++;
		}
		else
		{
			int qb = end1q.u.b; // == end2q
			if ( qb > 0  ) end1q.u.b--;
			if ( qb < 31 ) end2q.u.b++;
		}
		 
		RR_ASSERT( end1q.w != end2q.w );
		// drop through to non-degenerate case
		
		// 410.562
		// 389.925
		#endif
		
		#if 1
		// just do nothing
		//	not bad!
		// this hurts a tiny bit on linear_ramp1.BMP
		//	nobody else cares
		// levels >= 2 that optimize will find this encoding anyway
		//	by doing Wiggles out of the flat block encoding
		//	this only hurts linear_ramp on levels 0 & 1
		return false; // 410.592 , 389.934
		#endif
	}
	
	bool ret = false;
	
	// try 3-color and 4-color :
	for(int twice=0;twice<2;twice++)
	{
		rrColor32BGRA palette[4];
		DXT1_ComputePalette(end1q,end2q,palette,mode);
					
		U32 err;
		U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
		if ( err < *pError )
		{
			*pError = err;
			pBlock->c0 = end1q;
			pBlock->c1 = end2q;
			pBlock->indices = indices;
			
			ret = true;
		}
		
		// in forced four-color mode (DXT3/5 and BC2/3) there's no point in
		// swapping the end points since both orders yield four-color mode
		if ( mode == rrDXT1PaletteMode_FourColor )
			return ret;

		RR_NAMESPACE::swap(end1q,end2q);
	}
	
	return ret;
}
	
bool Compress_EndPoints_Force3C(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	// force 3 color order

	rrColor565Bits end1q = rrColor565Bits_Quantize(end1);
	rrColor565Bits end2q = rrColor565Bits_Quantize(end2);
	Make3ColorOrder(end1q,end2q);
	// now 3 color order
	
	// no need to detect degeneracy end1q == end2q
	//	because that is valid 3 color order
	
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(end1q,end2q,palette,mode);
	
	U32 err;
	U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
	if ( err < *pError )
	{
		*pError = err;
		pBlock->c0 = end1q;
		pBlock->c1 = end2q;
		pBlock->indices = indices;
		return true;
	}
	else
	{
		return false;
	}
}

bool Compress_TwoColorBest(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & c1,const rrColor32BGRA & c2)
{
	// this is wasteful, fix to work directly on colors instead of going through vec3i :
	rrVec3i v1 = ColorToVec3i(c1);
	rrVec3i v2 = ColorToVec3i(c2);
	rrVec3i delta = v2 - v1;
	
	// try to hit two colors exactly by either
	//	using them as the ends or trying to hit them at the 1/3 or 2/3 points
	
	// I only actually try 4 ways, I should just unroll them :
	// 0 : c1 , 1 : c2
	// 0 : c1 , 2/3 : c2
	// 1/3 : c1 , 1 : c2
	// 1/3 : c1 , 2/3 : c2
	
	// these ways are actually not tried now :
	// 2/3 : c1 , 1 : c2
	// 0 : c1 , 1/3 : c2	
	
	bool ret = false;
	
	//0,3 : v1->v2
	//0,2 : v1->v2+delta2
	//1,2 : v1-delta->v2+delta
	//1,3 : v1-delta2->v2

	ret |= Compress_EndPoints(pBlock,pError,colors,mode,c1,c2);

	// toggle just doing endpoints
	//	0.1 rmse win from this
	
	rrVec3i delta2;
	delta2.x = delta.x / 2;
	delta2.y = delta.y / 2;
	delta2.z = delta.z / 2;

	// tiny len, don't bother :
	// -> doing this check early helps the speed of VeryFast quite a bit
	//	if Compress_EndPoints was faster it might make sense to move this check later
	//if ( LengthSqr(delta2) == 0 )
	if ( LengthSqr(delta2) < 6 )
		return ret;
		
	{		
		// in 4Means it seems better to check /3 instead of the correct step-off to hit the 1/3,2/3		
		// in 2Means full delta is better
		/*
		delta.x /= 3;
		delta.y /= 3;
		delta.z /= 3;
		*/
		
		rrVec3i end1( v1 );
		end1 -= delta;
			
		rrVec3i end2( v2 );
		end2 += delta;
		
		rrColor32BGRA cend1 = Vec3iToColorClamp(end1);
		rrColor32BGRA cend2 = Vec3iToColorClamp(end2);
		
		ret |= Compress_EndPoints(pBlock,pError,colors,mode,cend1,cend2);
	}
	
	#if 0 // $$$$
	// on BC1-non-RD this is a small quality improvement
	// in BC1-RD it's decidedly meh
	//	sometimes slightly better, sometimes worse
	// I'm turning it off just to preserve consistency with old numbers
	{
		// 1/4 step off makes the interp points straddle c1,c2
	
		rrVec3i delta4;
		delta4.x = delta.x / 4;
		delta4.y = delta.y / 4;
		delta4.z = delta.z / 4;
	
		// meh not much difference in checking 0 or 6
		//	we've already checked delta2 so this is meh
		//if ( LengthSqr(delta4) != 0 )
		if ( LengthSqr(delta4) >= 6 )
		{
			rrVec3i end1( v1 );
			end1 -= delta4;
				
			rrVec3i end2( v2 );
			end2 += delta4;
			
			rrColor32BGRA cend1 = Vec3iToColorClamp(end1);
			rrColor32BGRA cend2 = Vec3iToColorClamp(end2);
			
			ret |= Compress_EndPoints(pBlock,pError,colors,mode,cend1,cend2);
		}
	}
	#endif
	
	/*
	// tiny len, don't bother :
	//if ( LengthSqr(delta2) == 0 )
	if ( LengthSqr(delta2) < 6 )
		return ret;
	*/
		
	{					
		rrVec3i end1( v1 );
		end1 -= delta2;
			
		rrVec3i end2( v2 );
		end2 += delta2;
		
		rrColor32BGRA cend1 = Vec3iToColorClamp(end1);
		rrColor32BGRA cend2 = Vec3iToColorClamp(end2);
		
		ret |= Compress_EndPoints(pBlock,pError,colors,mode,cend1,c2);
		
		ret |= Compress_EndPoints(pBlock,pError,colors,mode,c1,cend2);
		
		#if 0
		// rmse_total = 389.061  385.255
		// rmse_total = 388.469  384.945
		// @@ is this worth it ?
		ret |= Compress_EndPoints(pBlock,pError,colors,mode,cend1,cend2);
		#endif
	}
		
	return ret;
}

U32 * ColorPair_AddEndPoints_BothWays(U32 * pEndPoints,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2)
{
	rrDXT1EndPoints ep;
	ep.u.c0 = rrColor565Bits_Quantize(end1);
	ep.u.c1 = rrColor565Bits_Quantize(end2);
	if ( ep.u.c0.w == ep.u.c1.w ) // degenerate, just skip!
		return pEndPoints;
	*pEndPoints++ = ep.dw;	
	
	// @@
	// in forced four-color mode (DXT3/5 and BC2/3) there's no point in
	// swapping the end points since both orders yield four-color mode
	//if ( mode == rrDXT1PaletteMode_FourColor )
	//	return pEndPoints;
		
	swap(ep.u.c0,ep.u.c1);
	*pEndPoints++ = ep.dw;
	return pEndPoints;
}

U32 * Compress_TwoColorBest_AddEndPoints(U32 * pEndPoints,
							const rrColor32BGRA & c1,const rrColor32BGRA & c2)
{
	U32 * ptr = pEndPoints;

	// this is wasteful, fix to work directly on colors instead of going through vec3i :
	rrVec3i v1 = ColorToVec3i(c1);
	rrVec3i v2 = ColorToVec3i(c2);
	rrVec3i delta = v2 - v1;
	
	// try to hit two colors exactly by either
	//	using them as the ends or trying to hit them at the 1/3 or 2/3 points
	
	// I only actually try 4 ways, I should just unroll them :
	// 0 : c1 , 1 : c2
	// 0 : c1 , 2/3 : c2
	// 1/3 : c1 , 1 : c2
	// 1/3 : c1 , 2/3 : c2
	
	// these ways are actually not tried now :
	// 2/3 : c1 , 1 : c2
	// 0 : c1 , 1/3 : c2	
	
	//0,3 : v1->v2
	//0,2 : v1->v2+delta2
	//1,2 : v1-delta->v2+delta
	//1,3 : v1-delta2->v2

	ptr = ColorPair_AddEndPoints_BothWays(ptr,c1,c2);	

	// toggle just doing endpoints
	//	0.1 rmse win from this
	
	rrVec3i delta2;
	delta2.x = delta.x / 2;
	delta2.y = delta.y / 2;
	delta2.z = delta.z / 2;

	// tiny len, don't bother :
	//if ( LengthSqr(delta2) == 0 )
	if ( LengthSqr(delta2) < 6 )
		return ptr;


	{					
		rrVec3i end1( v1 );
		end1 -= delta;
			
		rrVec3i end2( v2 );
		end2 += delta;
		
		rrColor32BGRA cend1 = Vec3iToColorClamp(end1);
		rrColor32BGRA cend2 = Vec3iToColorClamp(end2);
		
		ptr = ColorPair_AddEndPoints_BothWays(ptr,cend1,cend2);	
	}
	
	{			
		rrVec3i end1( v1 );
		end1 -= delta2;
			
		rrVec3i end2( v2 );
		end2 += delta2;
		
		rrColor32BGRA cend1 = Vec3iToColorClamp(end1);
		rrColor32BGRA cend2 = Vec3iToColorClamp(end2);
		
		ptr = ColorPair_AddEndPoints_BothWays(ptr,cend1,c2);		
		
		ptr = ColorPair_AddEndPoints_BothWays(ptr,c1,cend2);	
		
		#if 0
		ptr = ColorPair_AddEndPoints_BothWays(ptr,cend1,cend2);	
		#endif
	}

	return ptr;
}

void TestDXT1Error()
{
	double max_err3 = 0.0;
	double max_err2 = 0.0;

	for(int c0=0;c0<64;c0++)
	{
		for(int c1=0;c1<64;c1++)
		{
			rrColor565Bits col0 = { 0 };
			rrColor565Bits col1 = { 0 };
			col0.u.g = (U16) c0;
			col1.u.g = (U16) c1;
			
			rrColor32BGRA palette[4];
			rrColor32BGRA palette_NV5[4];
			DXT1_ComputePalette(col0,col1,palette,rrDXT1PaletteMode_NoAlpha);
			DXT1_ComputePalette_NV5x(col0,col1,palette_NV5,rrDXT1PaletteMode_NoAlpha);
	
			RR_ASSERT( palette[0].dw == palette_NV5[0].dw );
			RR_ASSERT( palette[1].dw == palette_NV5[1].dw );
	
			int end_dg = RR_ABS( palette[0].u.g - palette[1].u.g );
	
			// |generated – reference| < 1.0 / 255.0 + 0.03 * |color_0_p – color_1_p|
			// |generated – reference| < 1.0 + 7.65 * |color_0_p – color_1_p|
			
			int dg3 = RR_ABS( palette[3].u.g - palette_NV5[3].u.g );
			if ( dg3 > 1.0 + max_err3 * end_dg )
			{
				max_err3 = (dg3 - 1.0) / end_dg;
			}
			
			int dg2 = RR_ABS( palette[2].u.g - palette_NV5[2].u.g );
			if ( dg2 > 1.0 + max_err2 * end_dg )
			{
				max_err2 = (dg2 - 1.0) / end_dg;
			}
		}
	}
	
	rrprintf("max_err3 : %f\n",max_err3);
	rrprintf("max_err2 : %f\n",max_err2);
	// G : max_err : 0.021277
	// R,B : max_err = 0 !!
	// max_err3 : 0.017316
	// max_err2 : 0.021277
}

// Tables generated by utils/bc1single.py, targeting a blend of different HW
// decoders

// sum_all_err=[('ref', 316), ('nv', 400), ('amd', 316), ('intel', 316)]
const U8 BC1_Opt5Tab_3C[512] =
{
         0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 1, 2, 2, 2, 2, 2,
         2, 2, 0, 4, 0, 4, 2, 3, 2, 3, 1, 4, 1, 4, 3, 3, 3, 3, 2, 4, 2, 4, 3, 4, 3, 4, 3, 4, 3, 4, 3, 5,
         3, 5, 4, 4, 4, 4, 3, 6, 3, 6, 4, 5, 4, 5, 3, 7, 3, 7, 5, 5, 5, 5, 5, 5, 5, 6, 5, 6, 5, 6, 6, 6,
         6, 6, 6, 6, 4, 8, 4, 8, 6, 7, 6, 7, 5, 8, 5, 8, 7, 7, 7, 7, 6, 8, 6, 8, 7, 8, 7, 8, 7, 8, 7, 8,
         7, 9, 7, 9, 8, 8, 8, 8, 7,10, 7,10, 8, 9, 8, 9, 7,11, 7,11, 9, 9, 9, 9, 9, 9, 9,10, 9,10, 9,10,
        10,10,10,10,10,10, 8,12, 8,12,10,11,10,11, 9,12, 9,12,11,11,11,11,10,12,10,12,11,12,11,12,11,12,
        11,12,11,13,11,13,12,12,12,12,11,14,11,14,12,13,12,13,11,15,11,15,13,13,13,13,13,13,13,14,13,14,
        13,14,14,14,14,14,14,14,12,16,12,16,14,15,14,15,13,16,13,16,15,15,15,15,14,16,14,16,15,16,15,16,
        15,16,15,16,15,17,15,17,16,16,16,16,15,18,15,18,16,17,16,17,15,19,15,19,17,17,17,17,17,17,17,18,
        17,18,17,18,18,18,18,18,18,18,16,20,16,20,18,19,18,19,17,20,17,20,19,19,19,19,18,20,18,20,19,20,
        19,20,19,20,19,20,19,21,19,21,20,20,20,20,19,22,19,22,20,21,20,21,19,23,19,23,21,21,21,21,21,21,
        21,22,21,22,21,22,22,22,22,22,22,22,20,24,20,24,22,23,22,23,21,24,21,24,23,23,23,23,22,24,22,24,
        23,24,23,24,23,24,23,24,23,25,23,25,24,24,24,24,23,26,23,26,24,25,24,25,23,27,23,27,25,25,25,25,
        25,25,25,26,25,26,25,26,26,26,26,26,26,26,24,28,24,28,26,27,26,27,25,28,25,28,27,27,27,27,26,28,
        26,28,27,28,27,28,27,28,27,28,27,29,27,29,28,28,28,28,27,30,27,30,28,29,28,29,27,31,27,31,29,29,
        29,29,29,29,29,30,29,30,29,30,30,30,30,30,30,30,30,30,30,30,30,31,30,31,30,31,31,31,31,31,31,31,
};

// sum_all_err=[('ref', 123), ('nv', 162), ('amd', 96), ('intel', 120)]
const U8 BC1_Opt5Tab_4C[512] =
{
         0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 2, 0, 4, 2, 1, 2, 1, 0, 5, 2, 2,
         2, 2, 2, 2, 2, 3, 1, 5, 1, 5, 3, 2, 4, 0, 3, 3, 3, 3, 4, 1, 3, 4, 3, 4, 5, 0, 2, 7, 4, 3, 4, 3,
         3, 6, 4, 4, 4, 4, 5, 3, 4, 5, 5, 4, 5, 4, 5, 4, 3, 9, 5, 5, 5, 5, 3,10, 4, 8, 6, 5, 6, 5, 8, 1,
         6, 6, 6, 6, 8, 2, 6, 7, 5, 9, 5, 9, 7, 6, 8, 4, 7, 7, 7, 7, 8, 5, 7, 8, 7, 8, 9, 4, 6,11, 8, 7,
         8, 7, 7,10, 8, 8, 8, 8, 9, 7, 8, 9, 9, 8, 9, 8, 9, 8, 7,13, 9, 9, 9, 9, 7,14, 8,12,10, 9,10, 9,
        12, 5,10,10,10,10,12, 6,10,11, 9,13, 9,13,11,10,12, 8,11,11,11,11,12, 9,11,12,11,12,13, 8,10,15,
        12,11,12,11,11,14,12,12,12,12,13,11,12,13,13,12,13,12,13,12,11,17,13,13,13,13,11,18,12,16,14,13,
        14,13,16, 9,14,14,14,14,16,10,14,15,13,17,13,17,15,14,16,12,15,15,15,15,16,13,15,16,15,16,17,12,
        14,19,16,15,16,15,15,18,16,16,16,16,17,15,16,17,17,16,17,16,17,16,15,21,17,17,17,17,15,22,16,20,
        18,17,18,17,20,13,18,18,18,18,20,14,18,19,17,21,17,21,19,18,20,16,19,19,19,19,20,17,19,20,19,20,
        21,16,18,23,20,19,20,19,19,22,20,20,20,20,21,19,20,21,21,20,21,20,21,20,19,25,21,21,21,21,19,26,
        20,24,22,21,22,21,24,17,22,22,22,22,24,18,22,23,21,25,21,25,23,22,24,20,23,23,23,23,24,21,23,24,
        23,24,25,20,22,27,24,23,24,23,23,26,24,24,24,24,25,23,24,25,25,24,25,24,25,24,23,29,25,25,25,25,
        23,30,24,28,26,25,26,25,28,21,26,26,26,26,28,22,26,27,25,29,25,29,27,26,28,24,27,27,27,27,28,25,
        27,28,27,28,29,24,26,31,28,27,28,27,27,30,28,28,28,28,29,27,28,29,29,28,29,28,29,28,30,27,29,29,
        29,29,31,26,29,30,30,29,30,29,30,29,30,30,30,30,30,30,30,31,30,31,30,31,31,30,31,30,31,31,31,31,
};

// sum_all_err=[('ref', 84), ('nv', 36), ('amd', 84), ('intel', 84)]
const U8 BC1_Opt6Tab_3C[512] =
{
         0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4,
         4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8,
         8, 8, 0,16, 8, 9, 1,16, 9, 9, 2,16, 9,10, 3,16,10,10, 4,16,10,11, 5,16,11,11, 6,16,11,12, 7,16,
        12,12, 8,16,12,13, 9,16,13,13,10,16,13,14,11,16,14,14,12,16,14,15,13,16,15,15,14,16,16,15,15,16,
        17,15,16,16,18,15,16,17,19,15,17,17,20,15,17,18,21,15,18,18,22,15,18,19,23,15,19,19,24,15,19,20,
        25,15,20,20,26,15,20,21,27,15,21,21,28,15,21,22,29,15,22,22,30,15,22,23,31,15,23,23,23,23,23,24,
        24,24,24,24,16,32,24,25,17,32,25,25,18,32,25,26,19,32,26,26,20,32,26,27,21,32,27,27,22,32,27,28,
        23,32,28,28,24,32,28,29,25,32,29,29,26,32,29,30,27,32,30,30,28,32,30,31,29,32,31,31,30,32,32,31,
        31,32,33,31,32,32,34,31,32,33,35,31,33,33,36,31,33,34,37,31,34,34,38,31,34,35,39,31,35,35,40,31,
        35,36,41,31,36,36,42,31,36,37,43,31,37,37,44,31,37,38,45,31,38,38,46,31,38,39,47,31,39,39,39,39,
        39,40,40,40,40,40,32,48,40,41,33,48,41,41,34,48,41,42,35,48,42,42,36,48,42,43,37,48,43,43,38,48,
        43,44,39,48,44,44,40,48,44,45,41,48,45,45,42,48,45,46,43,48,46,46,44,48,46,47,45,48,47,47,46,48,
        48,47,47,48,49,47,48,48,50,47,48,49,51,47,49,49,52,47,49,50,53,47,50,50,54,47,50,51,55,47,51,51,
        56,47,51,52,57,47,52,52,58,47,52,53,59,47,53,53,60,47,53,54,61,47,54,54,62,47,54,55,63,47,55,55,
        55,55,55,56,56,56,56,56,56,56,56,57,57,57,57,57,57,57,57,58,58,58,58,58,58,58,58,59,59,59,59,59,
        59,59,59,60,60,60,60,60,60,60,60,61,61,61,61,61,61,61,61,62,62,62,62,62,62,62,62,63,63,63,63,63,
};

// sum_all_err=[('ref', 49), ('nv', 21), ('amd', 18), ('intel', 36)]
const U8 BC1_Opt6Tab_4C[512] =
{
         0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 2, 0, 5, 2, 1, 2, 2, 2, 3, 1, 6, 3, 2, 3, 3, 3, 4, 0,11, 4, 3,
         4, 4, 4, 5, 1,12, 5, 4, 5, 5, 5, 6, 2,13, 6, 5, 6, 6, 6, 7, 3,14, 7, 6, 7, 7, 7, 8, 4,15, 8, 7,
         8, 8, 8, 9, 7,12, 9, 8, 9, 9, 9,10, 8,13,10, 9,10,10,10,11, 9,14,11,10,11,11,11,12,16, 2,12,11,
        12,12,12,13,16, 5,11,16,13,13,13,14,16, 8,12,17,14,14,14,15,16,11,14,16,15,15,16,13,16,14,16,15,
        15,18,16,16,16,17,15,20,15,21,17,17,17,18,19,15,15,24,18,18,18,19,21,14,19,18,19,19,19,20,22,15,
        20,19,20,20,20,21,17,28,21,20,21,21,21,22,18,29,22,21,22,22,22,23,19,30,23,22,23,23,23,24,20,31,
        24,23,24,24,24,25,23,28,25,24,25,25,25,26,24,29,26,25,26,26,26,27,25,30,27,26,27,27,27,28,32,18,
        28,27,28,28,28,29,32,21,27,32,29,29,29,30,32,24,28,33,30,30,30,31,32,27,30,32,31,31,32,29,32,30,
        32,31,31,34,32,32,32,33,31,36,31,37,33,33,33,34,35,31,31,40,34,34,34,35,37,30,35,34,35,35,35,36,
        38,31,36,35,36,36,36,37,33,44,37,36,37,37,37,38,34,45,38,37,38,38,38,39,35,46,39,38,39,39,39,40,
        36,47,40,39,40,40,40,41,39,44,41,40,41,41,41,42,40,45,42,41,42,42,42,43,41,46,43,42,43,43,43,44,
        48,34,44,43,44,44,44,45,48,37,43,48,45,45,45,46,48,40,44,49,46,46,46,47,48,43,46,48,47,47,48,45,
        48,46,48,47,47,50,48,48,48,49,47,52,47,53,49,49,49,50,51,47,47,56,50,50,50,51,53,46,51,50,51,51,
        51,52,54,47,52,51,52,52,52,53,49,60,53,52,53,53,53,54,50,61,54,53,54,54,54,55,51,62,55,54,55,55,
        55,56,52,63,56,55,56,56,56,57,55,60,57,56,57,57,57,58,56,61,58,57,58,58,58,59,57,62,59,58,59,59,
        59,60,58,63,60,59,60,60,60,61,63,55,61,60,61,61,61,62,62,61,62,61,62,62,62,63,63,62,63,62,63,63,
};

RR_NAMESPACE_END
