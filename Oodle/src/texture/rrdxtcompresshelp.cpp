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

#include "rrdxtcompresshelp.inl"

RR_NAMESPACE_START

#if 0
// "compact" optimal :
U8 COTable5_4C[512];
U8 COTable6_4C[512];
U8 COTable5_3C[512];
U8 COTable6_3C[512];
static void iTable_init(void);
#endif

/*

Compress_SingleColor_Compact restricts quantized endpoints to be within +-1 of the original color

*/

void Compress_SingleColor_Compact_4C(rrDXT1Block * dxtBlock,
							const rrColor32BGRA & c)
{
	rrDXT1Block & bco = *dxtBlock;
	bco.c0.u.r = COTable5_4C[c.u.r*2+0];
	bco.c0.u.g = COTable6_4C[c.u.g*2+0];
	bco.c0.u.b = COTable5_4C[c.u.b*2+0];
	bco.c1.u.r = COTable5_4C[c.u.r*2+1];
	bco.c1.u.g = COTable6_4C[c.u.g*2+1];
	bco.c1.u.b = COTable5_4C[c.u.b*2+1];
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
	bco.c0.u.r = COTable5_3C[c.u.r*2+0];
	bco.c0.u.g = COTable6_3C[c.u.g*2+0];
	bco.c0.u.b = COTable5_3C[c.u.b*2+0];
	bco.c1.u.r = COTable5_3C[c.u.r*2+1];
	bco.c1.u.g = COTable6_3C[c.u.g*2+1];
	bco.c1.u.b = COTable5_3C[c.u.b*2+1];
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

#if 0

static inline int Lerp13_16bit(int fm,int to)
{
	int t = fm * (2 * 0xAAAB) + to * 0xAAAB;

	return t>>17;
}

// make iTable for Compress_SingleColor_Compact

// size is 32 or 64
// expand [size] -> 256
static void PrepareOptTable(U8 Table[512], const U8 * expand, int size, bool fourc)
{
	int sum_err = 0;
	int max_err = 0;

	for (int i = 0; i < 256; i++)
	{
		int bestErr = 256;

		//int iq = rrMul8Bit(i,size-1);
		// map i from 256  -> size, rounding down and up :
		int dn = i*size/256;
		int up = (i*size+255)/256;
		
		// also allowed to step off by some additional amount :
		int step_off = 1;
		//int step_off = 2;
		
		int lo = RR_MAX(0,dn-step_off);
		int hi = RR_MIN(size-1,up+step_off);

		/*

		step_off 1 gets you most of the win

		no step off :
		sum_err : 414
		max_err : 7
		sum_err : 177
		max_err : 3

		max step off 1 :
		sum_err : 149
		max_err : 1
		sum_err : 59
		max_err : 1

		max step off 2 :
		sum_err : 125
		max_err : 1
		sum_err : 52
		max_err : 1
		
		*/

		for (int min = lo; min <= hi; min++)
		{
			for (int max = lo; max <= hi; max++)
			{
				// not really min/max , max < min is needed too
				//	that way we consider the 1/3 and 2/3 interp of each min/max pair
				int mine = expand[min];
				int maxe = expand[max];		
				
				int interp;
				
				if ( fourc )
					interp = Lerp13_16bit(mine,maxe);
				else
					interp = (mine + maxe)/2;
				
				int err = abs(interp - i);

				if (err < bestErr)
				{
					Table[i*2+0] = (U8)min;
					Table[i*2+1] = (U8)max;
					bestErr = err;
				}
				else if ( err == bestErr )
				{
					// break ties in favor of closer to original color i
					//	also favor more compact ranges
					int prev_mine = expand[Table[i*2+0]];
					int prev_maxe = expand[Table[i*2+1]];
					// L1 error is the same as (max - min) unless they're both on the same side of i :
					//int prev_d = abs(prev_mine - i) + abs(prev_maxe - i);	
					//int d = abs(mine - i) + abs(maxe - i);
					// meh? L2 or L1 ?
					int prev_d = Square(prev_mine - i) + Square(prev_maxe - i);	
					int d = Square(mine - i) + Square(maxe - i);
					if ( d < prev_d )
					{
						Table[i*2+0] = (U8)min;
						Table[i*2+1] = (U8)max;
						bestErr = err;
					}
					else if ( d == prev_d )
					{
						d = d;
					}
				}
			}
		}
		
		sum_err += bestErr;
		max_err = RR_MAX(max_err,bestErr);
	}
	
	rrprintfvar(sum_err);
	rrprintfvar(max_err);
}

static void iTable_init(void)
{
	static bool once = false;
	if ( once ) return;
	once = true;

	U8 Expand5[32];
	U8 Expand6[64];

	for(int i=0;i<32;i++)
		Expand5[i] = (U8)((i<<3)|(i>>2));

	for(int i=0;i<64;i++)
		Expand6[i] = (U8)((i<<2)|(i>>4));

	PrepareOptTable((U8 *)COTable5_4C, Expand5, 32 , true);
	PrepareOptTable((U8 *)COTable6_4C, Expand6, 64 , true);
	
	PrepareOptTable((U8 *)COTable5_3C, Expand5, 32 , false);
	PrepareOptTable((U8 *)COTable6_3C, Expand6, 64 , false);
	
	rrPrintfBin2C(COTable5_4C, 512, "COTable5_4C" );
	rrPrintfBin2C(COTable6_4C, 512, "COTable6_4C" );
	
	rrPrintfBin2C(COTable5_3C, 512, "COTable5_3C" );
	rrPrintfBin2C(COTable6_3C, 512, "COTable6_3C" );
};

#endif

RR_NAMESPACE_END
