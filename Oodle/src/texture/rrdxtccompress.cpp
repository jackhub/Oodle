// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#include "rrdxtccompress.h"
#include "rrdxtcompresshelp.h"
#include "rrdxtcblock.h"
#include "rrcolorvecc.h"
#include "rrrand.h"
#include <float.h>
#include "rrdxtccompress.inl"
#include "rrmat3.h"
#include "rrlogutil.h"

#include "templates/rralgorithm.h"
#include "templates/rrvector_s.h"

#include "rrsimpleprof.h"
//#include "rrsimpleprofstub.h"

//===============================================================================

RR_NAMESPACE_START

//================================================

struct rrCompressDXT1_Startup_Data
{
	rrVec3i avg;
	rrVec3i diagonal;
	//rrColor32BGRA avgC;
	rrColor32BGRA loC;
	rrColor32BGRA hiC;
	rrbool has_any_alpha; // has_any_alpha can only be true when mode == rrDXT1PaletteMode_Alpha
	// if has_any_alpha is on, you cannot use 4c mode, must use 3c mode
};

void DXT1_GreedyOptimizeBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool do_joint_optimization);

void DXT1_AnnealBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, 
	rrDXT1PaletteMode mode);

bool rrCompressDXT1_Degen_3C(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colorblock, rrDXT1PaletteMode mode,
	const rrColor32BGRA & avg_color);
	
// for palette mode,
//  is this color a 3rd-index special color? (transparent/black)
bool rrDXT1_IsTransparentOrBlack(rrDXT1PaletteMode mode,const rrColor32BGRA &color)
{
	if ( mode == rrDXT1PaletteMode_FourColor ) return false;
	else if ( mode == rrDXT1PaletteMode_Alpha ) return rrColor32BGRA_IsOneBitTransparent(color);
	else
	{
		// mode == NoAlpha ; is it black ?
		#define BLACKNESS_DISTANCE	12 // @@ blackness threshold
		return ( color.u.b < BLACKNESS_DISTANCE &&
			color.u.g < BLACKNESS_DISTANCE &&
			color.u.r < BLACKNESS_DISTANCE );
	}
}

bool Compress_TryAllPairs(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	vector_s<Color565,16> uniques;
	uniques.resize(16);
	for(int i=0;i<16;i++)
	{
		uniques[i] = Quantize( colors.colors[i] ).w;
	}
	RR_NAMESPACE::stdsort(uniques.begin(),uniques.end());
	vector_s<Color565,16>::iterator it = RR_NAMESPACE::unique(uniques.begin(),uniques.end());
	uniques.erase( it, uniques.end()  );
	
	int count = uniques.size32();
	
	if ( count == 1 )
	{
		// @@ special case; single color
		return false;
	}
	
	bool ret = false;
	for(int i=0;i<count;i++)
	{
		for(int j=i+1;j<count;j++)
		{
			Color565 c0 = uniques[j];
			Color565 c1 = uniques[i];
			RR_ASSERT( c0 > c1 );
			
			rrColor32BGRA palette[4];
			DXT1_ComputePalette(ToUnion(c0),ToUnion(c1),palette,mode);
			
			U32 err;
			U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
			if ( err < *pError )
			{
				ret = true;
				*pError = err;
				pBlock->c0.w = c0;
				pBlock->c1.w = c1;
				pBlock->indices = indices;
			}
		}
	}
	
	return ret;
}

bool Compress_TryAllPairs_Heavy(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	// Color565 is U16
	vector_s<Color565,16> uniques;
	uniques.resize(16);
	for(int i=0;i<16;i++)
	{
		uniques[i] = Quantize( colors.colors[i] ).w;
	}
	RR_NAMESPACE::stdsort(uniques.begin(),uniques.end());
	vector_s<Color565,16>::iterator it = RR_NAMESPACE::unique(uniques.begin(),uniques.end());
	uniques.erase( it, uniques.end()  );
	
	int count = uniques.size32();
	
	if ( count == 1 )
	{
		// @@ special case; single color
		return false;
	}
	
	bool ret = false;
	for(int i=0;i<count;i++)
	{
		for(int j=i+1;j<count;j++)
		{
			Color565 c0 = uniques[i];
			Color565 c1 = uniques[j];
			
			rrDXT1Block trial;
			
			trial.c0 = ToUnion(c0);
			trial.c1 = ToUnion(c1);	
			
			rrColor32BGRA palette[4];
			U32 err;

			{
				DXT1_ComputePalette(trial.c0,trial.c1,palette,mode);

				trial.indices = DXT1_FindIndices(colors,palette,mode,&err);
				
				DXT1_OptimizeEndPointsFromIndicesIterative(&trial,&err,colors,mode);
				
				if ( err < *pError )
				{
					ret = true;
					*pError = err;
					*pBlock = trial;
				}
			}
			
			// reverse colors and try again :
			// no point trying this in force-four-color mode, it doesn't give us any new options
			if ( mode != rrDXT1PaletteMode_FourColor )
			{
				RR_NAMESPACE::swap( trial.c0, trial.c1 );

				DXT1_ComputePalette(trial.c0,trial.c1,palette,mode);

				trial.indices = DXT1_FindIndices(colors,palette,mode,&err);
				
				DXT1_OptimizeEndPointsFromIndicesIterative(&trial,&err,colors,mode);
				
				if ( err < *pError )
				{
					ret = true;
					*pError = err;
					*pBlock = trial;
				}
			}
		}
	}
	
	return ret;
}

static inline const rrColor32BGRA Vec3iToColor_div_f_round(const rrVec3i & vec, int divider)
{
	// @@ clamp?
	rrColor32BGRA c;
	//*
	F32 recip = 1.f / divider;
	c.u.b = U8_check( rr_froundint( vec.x * recip ) );
	c.u.g = U8_check( rr_froundint( vec.y * recip ) );
	c.u.r = U8_check( rr_froundint( vec.z * recip ) );
	/*/
	c.u.b = (U8) ( vec.x / divider );
	c.u.g = (U8) ( vec.y / divider );
	c.u.r = (U8) ( vec.z / divider );
	/**/
	c.u.a = 0xFF;
	return c;
}

// rrCompressDXT1_Startup returns false for degenerate blocks that should not continue
//	fills pData if true is returned
bool rrCompressDXT1_Startup(rrCompressDXT1_Startup_Data * pData, rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	SIMPLEPROFILE_SCOPE(BC1_Startup);
	rrVec3i avg(0,0,0);
	
	rrColor32BGRA loC;
	loC.dw = 0xFFFFFFFF;
	loC.u.a = 0;
	rrColor32BGRA hiC;
	hiC.dw = 0;
	
	int num_colors = 0;
	
	rrColor32BGRA loC_colors;
	loC_colors.dw = 0xFFFFFFFF;
	loC_colors.u.a = 0;
	rrColor32BGRA hiC_colors;
	hiC_colors.dw = 0;
	
	int num_transparent = 0;

	for(int i=0;i<16;i++)
	{
		const rrColor32BGRA & c = colors.colors[i];

		RR_ASSERT( rrColor32BGRA_IsOneBitTransparentCanonical(c) );

		if ( c.dw == 0 )
		{
			num_transparent++;
		}

		avg += ColorToVec3i( c );
		
		hiC.u.b = RR_MAX(hiC.u.b,c.u.b);
		hiC.u.g = RR_MAX(hiC.u.g,c.u.g);
		hiC.u.r = RR_MAX(hiC.u.r,c.u.r);
		loC.u.b = RR_MIN(loC.u.b,c.u.b);
		loC.u.g = RR_MIN(loC.u.g,c.u.g);
		loC.u.r = RR_MIN(loC.u.r,c.u.r);
		
		if ( ! rrDXT1_IsTransparentOrBlack(mode,c) )
		{
			// if pal_mode == alpha,
			//	then blacks come in here and count as "colors"
			num_colors++;
			
			hiC_colors.u.b = RR_MAX(hiC_colors.u.b,c.u.b);
			hiC_colors.u.g = RR_MAX(hiC_colors.u.g,c.u.g);
			hiC_colors.u.r = RR_MAX(hiC_colors.u.r,c.u.r);
			loC_colors.u.b = RR_MIN(loC_colors.u.b,c.u.b);
			loC_colors.u.g = RR_MIN(loC_colors.u.g,c.u.g);
			loC_colors.u.r = RR_MIN(loC_colors.u.r,c.u.r);
		}
	}
	
	// loC/hiC alphas are all zero

	// hiC includes all colors, degen and non
	if ( hiC.dw == 0 )
	{
		// there can be a mix of opaque-black & transparent here
		// still need to code indexes

		// be careful about rrDXT1PaletteMode_FourColor

		if ( num_transparent == 16 )
		{
			// all transparent
			pBlock->endpoints = 0xFFFFFFFF; // can put anything here (if its 3c mode)
			pBlock->indices = 0xFFFFFFFF; // 3rd index
		}
		else if ( num_transparent == 0 )
		{
			// all black-opaque
			pBlock->endpoints = 0;
			pBlock->indices = 0;
		}
		else
		{
			// some mix of black-opaque and transparent
			U32 check_err;
			pBlock->endpoints = 0; // black, 3c mode
			pBlock->indices = DXT1_FindIndices(colors,pBlock->endpoints,mode,&check_err);
			RR_ASSERT( check_err == 0 );
		}

		*pError = 0;

		RR_DURING_ASSERT( U32 check_err = DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );
		RR_ASSERT( check_err == 0 );
		return false;
	}

	RR_ASSERT( num_transparent != 16 ); // should have been caught above
	// num_colors == 0 is possible here

	pData->has_any_alpha = num_transparent > 0;

	// "avg" includes all colors, including degens
	avg.x = (avg.x + 8)>>4;
	avg.y = (avg.y + 8)>>4;
	avg.z = (avg.z + 8)>>4;
	
	rrColor32BGRA avgC;
	avgC = Vec3iToColor(avg);
	
	if ( ! pData->has_any_alpha )
	{
		// try single color block to get started :
		rrDXT1Block block;
		Compress_SingleColor_Compact(&block,avgC);
		U32 err = DXT1_ComputeSSD_RGBA(colors,block,mode);
		if ( err < *pError )
		{
			*pBlock = block;
			*pError = err;
		}
	}

	if ( num_colors < 16 )
	{
		if ( num_colors == 0 )
		{
			// degenerate, no colors
			
			// we already checked hiC.dw == 0  above
			//  so it's not a true pure black degenerate (nor all transparent)

			// we still might have not quite true blacks that were classified as "black"
			//	eg. (4,4,4) would fall in the "blackness threshold"
			// we can do better by trying to code those
			// so don't just bail here
			
			// if we don't explicitly detect all-transparent
			//  the drop-through code might use the RGB values to code something funny
			//	(if input was not canonical, but it IS canonical, so that's not true)
			// -> because of canonicalization hiC.dw will be == 0 in either case
										
			// all were in "blackness threshold"
			//  but not true black
			// use the full color bbox						
			loC_colors = loC;
			hiC_colors = hiC;
		}
			
		// use the loC/hiC only of the non-transparent colors
		//	see rrDXT1_IsTransparentOrBlack

		rrColor32BGRA midC_colors = Average(loC_colors,hiC_colors);
		rrCompressDXT1_Degen_3C(pBlock,pError,colors,mode,midC_colors);

		if ( loC_colors.dw == hiC_colors.dw )
		{
			// degenerate, only one color (that's not transparent or black)
			//	 (this is a little fuzzy because of blackness threshold)
			//	 (there could be other shades of black that we just didn't count)
			// -> no don't return
			// helps to fall through here!
			//return false;
		}
		else
		{	
			Compress_EndPoints_Force3C(pBlock,pError,colors,mode,loC_colors,hiC_colors);
		}
		
		/*
		rrVec3i diagonal_colors = ColorToVec3i(hiC_colors) - ColorToVec3i(loC_colors);
		S32 lensqr = LengthSqr(diagonal_colors);
		if ( lensqr <= 12 )
		{
			return false;
		}
		*/
		
		// @@ fill pData with full color info or reduced ?
		
		/*
		// this seems like a good idea but seems to hurt a bit in practice
		// bc1 -a1 -l3 --w1 r:\rdotestset2\p7_zm_zod_crab_cage_net_c_BC7_UNORM_sRGB_A.tga
		//per-pixel rmse : 5.1956
		//per-pixel rmse : 5.1977
		if ( mode == rrDXT1PaletteMode_Alpha )
		{
			// because of canonicalization
			// transparent pels are black
			// loC will always have dw == 0
			RR_ASSERT( loC.dw == 0 );
			loC = loC_colors;
			avg = ColorToVec3i(midC_colors);
		}
		/**/
		
	}
	
	if ( loC.dw == hiC.dw )
	{
		// degenerate, only one color
		//	already did SingleColor, get out
		return false;
	}	

	rrVec3i diagonal = ColorToVec3i(hiC) - ColorToVec3i(loC);

	if ( LengthSqr(diagonal) <= 12 )
	{
		// very tiny color bbox
		//Compress_TwoColorBest(pBlock,pError,colors,mode,loC,hiC);
		Compress_EndPoints(pBlock,pError,colors,mode,loC,hiC);
		return false;
	}
	
	// fill rrCompressDXT1_Startup_Data	
	pData->avg = avg;
	pData->diagonal = diagonal;
	pData->loC = loC;
	pData->hiC = hiC;	
	
	return true;
}

bool rrCompressDXT1_2Means(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	rrVec3i avg = data.avg;		
				
	// don't use pca, just diagonal :
	rrVec3f pca = Vec3i_to_Vec3f( data.diagonal );
	pca = Normalize(pca);

	// dot the colors in the PCA linear fit direction & seed 2-means
	rrVec3i sumNeg(0,0,0); int countNeg = 0;
	rrVec3i sumPos(0,0,0); int countPos = 0;
	for(int i=0;i<16;i++)
	{
		rrVec3i vec = ColorToVec3i( colors.colors[i] );
		rrVec3i delta = vec - avg;
		rrVec3f d = Vec3i_to_Vec3f(delta); 
		F64 dot = d * pca;
		
		if ( dot > 0.f )
		{
			sumPos += vec;
			countPos ++;
		}
		else
		{
			sumNeg += vec;
			countNeg ++;
		}
	}
	
	// make initial 2 means :
	//RR_ASSERT( countPos > 0 && countNeg > 0 );
	if ( countPos == 0 || countNeg == 0 )
	{
		// some kind of bad degeneracy
		// already handled okay by above 1 & 2 color modes
		// @@ actually since we use bbox diagonal, not true pca
		//	this just tells us the bbox diagonal was not a good splitting axis
		//Compress_TwoColorBest(pBlock,pError,colors,mode,loC,hiC);
		//rmse_total = 437.230
		//rmse_total = 437.238
		return false;
	}
	
	rrColor32BGRA colorPos = Vec3iToColor_div_f_round(sumPos,countPos);
	rrColor32BGRA colorNeg = Vec3iToColor_div_f_round(sumNeg,countNeg);
	
	for(;;)
	{
		// refine 2-means :
		
		sumNeg = rrVec3i(0,0,0); countNeg = 0;
		sumPos = rrVec3i(0,0,0); countPos = 0;
		
		for(int i=0;i<16;i++)
		{
			const rrColor32BGRA & c = colors.colors[i];
			U32 dPos = DeltaSqrRGB( c , colorPos );
			U32 dNeg = DeltaSqrRGB( c , colorNeg );
			rrVec3i vec = ColorToVec3i( c );
		
			if ( dPos < dNeg )
			{
				sumPos += vec; countPos ++;
			}
			else
			{
				sumNeg += vec; countNeg ++;
			}			
		}
		
		if ( countPos == 0 || countNeg == 0 )
		{
			break;
		}
			
		rrColor32BGRA newColorPos = Vec3iToColor_div_f_round(sumPos,countPos);
		rrColor32BGRA newColorNeg = Vec3iToColor_div_f_round(sumNeg,countNeg);
		
		if ( newColorPos.dw == colorPos.dw &&
			 newColorNeg.dw == colorNeg.dw )
			break;
		
		colorPos = newColorPos;
		colorNeg = newColorNeg;
	}
	
	// hit the two means as well as possible :
	Compress_TwoColorBest(pBlock,pError,colors,mode,colorPos,colorNeg);
	
	#if 1
	// -> moved inside TwoColorBest
	// $$$$ moved back out
	
	sumPos = ColorToVec3i(colorPos);
	sumNeg = ColorToVec3i(colorNeg);
	
	// step off so that the 1/3 and 2/3 straddle the two-means exactly :
	//	(this puts the two means centers at the 1/6 and 5/6 points
	//	 eg. half-way between the ends and the 1/3,2/3 points)
	//  as opposed to Compress_TwoColorBest which puts them on the 1/3 points
	//	does this help? -> yes quite a bit
	rrVec3i twoMeanDelta = sumPos - sumNeg;
	// (-1>>2) == -1 , meh
	twoMeanDelta.x >>= 2; twoMeanDelta.y >>= 2; twoMeanDelta.z >>= 2;
	//twoMeanDelta.x /= 4; twoMeanDelta.y /= 4; twoMeanDelta.z /= 4;
	rrVec3i endPos = sumPos + twoMeanDelta;
	rrVec3i endNeg = sumNeg - twoMeanDelta;
	rrColor32BGRA colorEndPos = Vec3iToColorClamp(endPos);
	rrColor32BGRA colorEndNeg = Vec3iToColorClamp(endNeg);
	
	// TwoColorBest here doesn't help much :
	// rmse_total = 437.238
	// rmse_total = 433.388
	Compress_EndPoints(pBlock,pError,colors,mode,colorEndPos,colorEndNeg);
	//Compress_TwoColorBest(pBlock,pError,colors,mode,colorEndPos,colorEndNeg);
	
	#endif
	
	return true;
}

bool rrCompressDXT1_4Means(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool use_pca)
{
	SIMPLEPROFILE_SCOPE(BC1_4Means);
		
	rrVec3i avg = data.avg;	
	rrVec3f pca;
	
	if ( use_pca )
	{
		rrVec3f avgF = Vec3i_to_Vec3f(avg);	
		rrMat3 cov;
		rrMat3_SetZero(&cov);
		for(int i=0;i<16;i++)
		{
			rrVec3f d = ColorToVec3f( colors.colors[i] ) - avgF;
			cov[0].x += d.x * d.x;
			cov[0].y += d.x * d.y;
			cov[0].z += d.x * d.z;
			cov[1].y += d.y * d.y;
			cov[1].z += d.y * d.z;
			cov[2].z += d.z * d.z;
		}
		cov[1].x = cov[0].y;
		cov[2].x = cov[0].z;
		cov[2].y = cov[1].z;

		// power iteration for eigenvector :
		pca = Vec3i_to_Vec3f(data.diagonal);
		
		for(int iter=0;iter<4;iter++)
		{
			pca = cov * pca;
			pca = cov * pca;
			// technically I could just normalize at the end, but the floats get blown out of range
			if ( ! NormalizeSafe(&pca) )
			{
				// NOTE(fg): this is bad:
				// loC and hiC are bbox corners which can be very far from all colors in the block
				// and it's certainly no reason to bail out of this pass just because we got unlucky with the seed vec
				//Compress_TwoColorBest(pBlock,pError,colors,mode,data.loC,data.hiC);
				//return true;

				// Instead, pick the longest diagonal between pixels in the block the average
				// color; this is guaranteed to at least be something that makes sense for the block
				// (we handled single-color blocks during init) and is symmetric.
				//
				// This doesn't need to be particularly fast, it's rare, just try to not face-plant
				pca = rrVec3f(1.f,1.f,1.f);
				F32 longest_len2 = 0.0;

				for(int i=1;i<16;i++)
				{
					rrVec3f colori = ColorToVec3f(colors.colors[i]);
					rrVec3f axis = colori - avgF;
					F32 len2 = (F32)LengthSqr(axis);
					if (len2 > longest_len2)
					{
						pca = axis;
						longest_len2 = len2;
					}
				}

				pca = Normalize(pca);
			}
		}
	}
	else
	{
		// just diagonal	
		pca = Vec3i_to_Vec3f(data.diagonal);
		pca = Normalize(pca);
	}

	// dot the colors in the PCA linear fit direction & seed 4-means
	F32 minDot =  999999.f;
	F32 maxDot = -999999.f;
	//F64 max_perpSqr = 0;
	//int max_perpSqr_i = 0;
	for(int i=0;i<16;i++)
	{
		rrVec3i vec = ColorToVec3i( colors.colors[i] );
		rrVec3i delta = vec - avg;
		rrVec3f d = Vec3i_to_Vec3f(delta);
		F32 dot =(F32)( d * pca );
		minDot = RR_MIN(minDot,dot);
		maxDot = RR_MAX(maxDot,dot);

		/*
		// find the point farthest off the PCA axis :
		F64 perpSqr = LengthSqr(d) - (dot*dot);
		if ( perpSqr > max_perpSqr )
		{
			max_perpSqr = perpSqr;
			max_perpSqr_i = i;
		}
		*/
	}
	
	// this is true with the real PCA axis, but not with the bbox diagonal :
	//RR_ASSERT( minDot <= 0.f && maxDot >= 0.f );
	
	// make initial 4 means :

	// means are indexed like 0,1,2,3 in order, not the DXT1 order of 0,2,3,1

	rrColor32BGRA means[4];
	
	{
//	SIMPLEPROFILE_SCOPE(FindMeans);
	
	#if 1
	// make 4 points staggered along the pca line :
	{
	rrVec3f meansf[4];
	meansf[0] = Vec3i_to_Vec3f(avg) + (0.75f * minDot) * pca;
	meansf[3] = Vec3i_to_Vec3f(avg) + (0.75f * maxDot) * pca;	
	meansf[1] = meansf[0] + (meansf[3] - meansf[0]) * (1.f/3.f);
	meansf[2] = meansf[0] + (meansf[3] - meansf[0]) * (2.f/3.f);
		
	for(int i=0;i<4;i++) means[i] = Vec3fToColorClamp(meansf[i]);
	}
	#else
	{
	// alternate seed strategy :
	//	take two seed endpoints
	//	3rd point= furthest from pca axis
	//	3th = furthest from first 3
	// -> worse
	
	//rrVec3f end0 = Vec3i_to_Vec3f(avg) + (0.75f * minDot) * pca;
	//rrVec3f end1 = Vec3i_to_Vec3f(avg) + (0.75f * maxDot) * pca;	
	rrVec3f end0 = Vec3i_to_Vec3f(avg) + (minDot) * pca;
	rrVec3f end1 = Vec3i_to_Vec3f(avg) + (maxDot) * pca;	
	
	means[0] = Vec3fToColorClamp(end0);
	means[3] = Vec3fToColorClamp(end1);
	means[1] = colors.colors[max_perpSqr_i];
	
	// set means[2] to farthest from the first 3 :
	
	U32 worst_d = 0;

	// this is essentially FindIndices but with "means" as the palette
	for(int i=0;i<16;i++)
	{
		const rrColor32BGRA & c = colors.colors[i];
		U32 d0 = DeltaSqrRGB( c , means[0] );
		U32 d1 = DeltaSqrRGB( c , means[1] );
		U32 d3 = DeltaSqrRGB( c , means[3] );
		
		U32 min_d = RR_MIN3(d0,d1,d3);
		min_d = (min_d<<4) + i;
		worst_d = RR_MAX(worst_d,min_d);
	}

	int worst_i = worst_d & 0xF;
		
	means[2] = colors.colors[worst_i];	
	
	}	
	#endif
	
	for(;;)
	{
		// refine 4-means :
		rrVec3i sums[4];
		memset(sums,0,4*sizeof(rrVec3i));
		int	counts[4] = { 0 };
		
		//rmse_total = 386.710

		U32 worst_d = 0;
		int worst_d_i = 0;

		// this is essentially FindIndices but with "means" as the palette
		// @@ SIMD me
		for(int i=0;i<16;i++)
		{
			const rrColor32BGRA & c = colors.colors[i];
			U32 d0 = DeltaSqrRGB( c , means[0] );
			U32 d1 = DeltaSqrRGB( c , means[1] );
			U32 d2 = DeltaSqrRGB( c , means[2] );
			U32 d3 = DeltaSqrRGB( c , means[3] );
			
			/*
			int index = 0;
			if ( d0 <= d1 && d0 <= d2 && d0 <= d3 ) index = 0;
			else if ( d1 <= d2 && d1 <= d3 ) index = 1;
			else if ( d2 <= d3 ) index = 2;
			else index = 3;
			*/
			
			d0 <<= 2;
			d1 = (d1<<2) + 1;
			d2 = (d2<<2) + 2;
			d3 = (d3<<2) + 3;
			U32 min_d = RR_MIN4(d0,d1,d2,d3);
			int index = min_d & 3;
			
			sums[index] += ColorToVec3i(c);
			counts[index] ++;
			
			// also track which point is furthest from any mean :
			if ( min_d > worst_d )
			{
				worst_d = min_d;
				worst_d_i = i;
			}
		}
		
		bool anyChanged = false;

		for(int i=0;i<4;i++)
		{
			// if count == 0 , nobody mapped there,
			int count = counts[i];
			if ( count == 0 )
			{
				//just leave means alone
				// rmse_total = 386.710
				//change this mean to the point that was furthest from any mean :
				// rmse_total = 385.078
				means[i] = colors.colors[worst_d_i];
				// infinite loop :
				//anyChanged = true;
				
				// WARNING : settings means[] to a source color directly like this
				//	can take the alpha value from the source
				//	that should be benign but let's just stomp it to be sure
				means[i].u.a = 255;
				continue;
			}
					
			rrColor32BGRA color = Vec3iToColor_div_f_round(sums[i],count);
			
			if ( color.dw != means[i].dw )
			{
				anyChanged = true;
				means[i] = color;
			}
		}	
		
		if ( ! anyChanged )
			break;
	}
	
	}
	
	/*
	// enumerate all pairs
	//	this is the 6 trials explicitly written below
	for(int i=0;i<3;i++)
	{
		for(int j=i+1;j<4;j++)
		{
			Compress_TwoColorBest(pBlock,pError,colors,mode,means[i],means[j]);
		}
	}
	*/
	
	#if 0
	// use TwoColorBest which tries the points as the various interpolants
	
	// hit the four means as well as possible :
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[0],means[3]);
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[1],means[2]);
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[0],means[2]);
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[1],means[3]);
	
	//*
	
	// [0,1] and [2,3] are the less important 2 of the full enumeration of pairs
	// helps a micro bit 
	//with :  rmse_total = 386.710
	//without:rmse_total = 388.998
	
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[0],means[1]);
	Compress_TwoColorBest(pBlock,pError,colors,mode,means[2],means[3]);
	
	Compress_TwoColorBest(pBlock,pError,colors,mode, Average(means[0],means[1]), Average(means[2],means[3]) );
	/**/

	#else
	
	// experiment :
	// add all endpoint pairs we want to try
	// uniqueify them
	// then only try the unique ones
	// when the color bbox is small, we get lots of 565 collisions
	// -> this appears to be slightly faster without the uniquing
	
	// Compress_TwoColorBest_AddEndPoints can add 8  (was 10)
	//	8*7 = 56
	
	U32 endpoints[10*7];
	U32 * endptr = endpoints;
		
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[0],means[3]);
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[1],means[2]);
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[0],means[2]);
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[1],means[3]);
	
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[0],means[1]);
	endptr = Compress_TwoColorBest_AddEndPoints(endptr,means[2],means[3]);
	endptr = Compress_TwoColorBest_AddEndPoints(endptr, Average(means[0],means[1]), Average(means[2],means[3]) );
	
	RR_ASSERT( (SINTa)(endptr - endpoints) <= (SINTa)RR_ARRAY_SIZE(endpoints) );
	
	/*
	// time to sort and unique is more than the time it saves
	RR_NAMESPACE::stdsort(endpoints,endptr);
	endptr = RR_NAMESPACE::unique(endpoints,endptr);
	/**/
	
	int count = (int)(endptr - endpoints);
	
	// with no degenerates, count is 56
	// in practice I usually see count around 50
	//	some collisions removed but not a ton
	{
//	SIMPLEPROFILE_SCOPE(FindIndices);
	
	for LOOP(i,count)
	{
		if ( data.has_any_alpha && DXT1_Is4Color(endpoints[i],rrDXT1PaletteMode_Alpha) )
			continue;

		rrColor32BGRA palette[4];
		DXT1_ComputePalette(endpoints[i],palette,mode);
		
		U32 err;
		U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
		if ( err < *pError )
		{
			*pError = err;
			pBlock->endpoints = endpoints[i];
			pBlock->indices = indices;
		}
	}
	}

	#endif	
	
	return true;
}

/**

8Means from bbox corners
only a tiny bit better than 4menas
(8 choose 2) = 8*7/2 = 28 trials instead of (4 choose 2) = 4*3/2 = 6 for 4means

**/
bool rrCompressDXT1_8Means(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	rrColor32BGRA loC = data.loC;
	rrColor32BGRA hiC = data.hiC;
	
	rrColor32BGRA means[8];
		
	// idea : in the initial seeding if any count is zero
	//	make them be interpolants in the middle, like 4 means
	//	when your colors actually are colinear, you want points along that line
	//	
	// another idea : could use PCA and the full covariance matrix
	//	make 4 points along the PCA axis
	//	and then 2 points off each of the minor axes
	//		= 8 total points
	//	it's a way to favor giving more points along the primary axis
	//	  this would also make it more closely related to 4 means and more progressive
	
	// as is, 8means is sometimes *worse* than 4means
	//	 because it's not a strict superset
	//	 (it's bbox corners whereas 4means is 4 points along a line)
	
	//bool once = true;
	
	int	counts[8];
	rrVec3i sums[8];

	#if 0
	
	// make the bbox corners :
	
	for LOOP(i,8)
	{
		means[i].u.r = (i&1) ? hiC.u.r : loC.u.r;
		means[i].u.g = (i&2) ? hiC.u.g : loC.u.g;
		means[i].u.b = (i&4) ? hiC.u.b : loC.u.b;
		means[i].u.a = 255;
	}
	
	#else

//per-pixel rmse : 15.5549
//per-pixel rmse : 15.5598

	// initial means are just bbox corners

	// @@ to do the initial seeding, could just take the colors in the bbox
	//	and divide by which octant they fall in
	//	that's a lot faster than 8 distances
	
	{
		// refine means :
		RR_ZERO(sums);
		RR_ZERO(counts);
		
		int midR = (loC.u.r + hiC.u.r + 1)/2;
		int midG = (loC.u.g + hiC.u.g + 1)/2;
		int midB = (loC.u.b + hiC.u.b + 1)/2;
		
		for(int i=0;i<16;i++)
		{
			const rrColor32BGRA & c = colors.colors[i];
		
			int ir = c.u.r >= midR;	
			int ig = c.u.g >= midG;	
			int ib = c.u.b >= midB;
			
			int index = ir + (ig<<1) + (ib<<2);
				
			sums[index] += ColorToVec3i(c);
			counts[index] ++;
		}
		
		for(int i=0;i<8;i++)
		{
			// if count == 0 , nobody mapped there,
			int count = counts[i];
			if ( count == 0 )
			{
				// must write all the means
				means[i].u.r = (U8) midR;
				means[i].u.g = (U8) midG;
				means[i].u.b = (U8) midB;
				means[i].u.a = 255;
				continue;
			}
		
			rrColor32BGRA color = Vec3iToColor_div_f_round(sums[i],count);
			
			means[i] = color;
		}	
	}
	#endif
		
	for(;;)
	{
		// refine means :
		RR_ZERO(sums);
		RR_ZERO(counts);
		
		U32 worst_d = 0;
		int worst_d_i = -1;
		
		// this is essentially FindIndices but with "means" as the palette
		//	@@ SIMD me!
		for(int i=0;i<16;i++)
		{
			const rrColor32BGRA & c = colors.colors[i];
			U32 d0 = (DeltaSqrRGB( c , means[0] )<<3) + 0;
			U32 d1 = (DeltaSqrRGB( c , means[1] )<<3) + 1;
			U32 d2 = (DeltaSqrRGB( c , means[2] )<<3) + 2;
			U32 d3 = (DeltaSqrRGB( c , means[3] )<<3) + 3;
			U32 d4 = (DeltaSqrRGB( c , means[4] )<<3) + 4;
			U32 d5 = (DeltaSqrRGB( c , means[5] )<<3) + 5;
			U32 d6 = (DeltaSqrRGB( c , means[6] )<<3) + 6;
			U32 d7 = (DeltaSqrRGB( c , means[7] )<<3) + 7;
						
			U32 min_d = RR_MIN(RR_MIN4(d0,d1,d2,d3),RR_MIN4(d4,d5,d6,d7));
			int index = min_d & 7;
			
			sums[index] += ColorToVec3i(c);
			counts[index] ++;
			
			// also track which point is furthest from any mean :
			if ( min_d > worst_d )
			{
				worst_d = min_d;
				worst_d_i = i;
			}
		}
		
		bool anyChanged = false;

		for(int i=0;i<8;i++)
		{
			// if count == 0 , nobody mapped there,
			int count = counts[i];
			if ( count == 0 )
			{
				// @@ may take this point more than once?
				//	do something else after first one ?
				if ( worst_d_i >= 0 )
				{
					means[i] = colors.colors[worst_d_i];
					// WARNING : settings means[] to a source color directly like this
					//	can take the alpha value from the source
					//	that should be benign but let's just stomp it to be sure
					means[i].u.a = 255;
				
					counts[i] = 1;
					worst_d_i = -1; // don't take it again
					// future means with count == 0 will be left
				}
				//retry once? doesn't help
				//anyChanged = once;
				//once = false;
				continue;
			}
		
			rrColor32BGRA color = Vec3iToColor_div_f_round(sums[i],count);
			
			if ( color.dw != means[i].dw )
			{
				anyChanged = true;
				means[i] = color;
			}
		}	
		
		if ( ! anyChanged )
			break;
	}
	
	// enumerate all pairs
	for(int i=0;i<7;i++)
	{
		// for speed, skip means with no points mapped
		if ( counts[i] == 0 ) continue;
		for(int j=i+1;j<8;j++)
		{
			if ( counts[j] == 0 ) continue;
			Compress_TwoColorBest(pBlock,pError,colors,mode,means[i],means[j]);
		}
	}	
	
	#if 0
	// do bbox edges and faces too?
	
	// these help only a tiny bit
	//	easy, difficult
	//without: 306.382, 119.087
	//with :   306.185, 118.906

	// 12 edges :
	rrColor32BGRA edge01 = Average(means[0],means[1]);
	rrColor32BGRA edge02 = Average(means[0],means[2]);
	rrColor32BGRA edge04 = Average(means[0],means[4]);
	rrColor32BGRA edge13 = Average(means[1],means[3]);
	rrColor32BGRA edge15 = Average(means[1],means[5]);
	rrColor32BGRA edge23 = Average(means[2],means[3]);
	rrColor32BGRA edge26 = Average(means[2],means[6]);
	rrColor32BGRA edge45 = Average(means[4],means[5]);
	rrColor32BGRA edge46 = Average(means[4],means[6]);
	rrColor32BGRA edge37 = Average(means[3],means[7]);
	rrColor32BGRA edge57 = Average(means[5],means[7]);
	rrColor32BGRA edge67 = Average(means[6],means[7]);
	
	// 6 edge-edge through the middle :
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge01,edge67);
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge02,edge57);
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge04,edge37);
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge13,edge46);
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge15,edge26);
	Compress_TwoColorBest(pBlock,pError,colors,mode,edge23,edge45);
	
	// 3 face-face through the middle :
	
	rrColor32BGRA face0123 = Average(edge01,edge23);
	rrColor32BGRA face4567 = Average(edge46,edge57);
	Compress_TwoColorBest(pBlock,pError,colors,mode,face0123,face4567);
	
	rrColor32BGRA face0246 = Average(edge02,edge46);
	rrColor32BGRA face1357 = Average(edge13,edge57);
	Compress_TwoColorBest(pBlock,pError,colors,mode,face0246,face1357);
	
	rrColor32BGRA face0145 = Average(edge01,edge45);
	rrColor32BGRA face2367 = Average(edge23,edge67);
	Compress_TwoColorBest(pBlock,pError,colors,mode,face0145,face2367);
	#endif
	
	return true;
}

#if 0
/***

rrCompressDXT1_PCA_Axis_Stretches
find the pca axis
make endpoints at the extrema of the pca axis
then try various stretches of the sdev for alternate endpoint locations

-> no good

****/
bool rrCompressDXT1_PCA_Axis_Stretches(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	rrVec3f avgF = Vec3i_to_Vec3f(data.avg);
	
	rrMat3 cov;
	rrMat3_SetZero(&cov);
	for(int i=0;i<16;i++)
	{
		rrVec3f d = ColorToVec3f( colors.colors[i] ) - avgF;
		cov[0].x += d.x * d.x;
		cov[0].y += d.x * d.y;
		cov[0].z += d.x * d.z;
		cov[1].y += d.y * d.y;
		cov[1].z += d.y * d.z;
		cov[2].z += d.z * d.z;
	}
	cov[1].x = cov[0].y;
	cov[2].x = cov[0].z;
	cov[2].y = cov[1].z;

	// power iteration for eigenvector :
	rrVec3f pca = Vec3i_to_Vec3f(data.diagonal);
	
	for(int iter=0;iter<4;iter++)
	{
		pca = cov * pca;
		pca = cov * pca;
		// technically I could just normalize at the end, but the floats get blown out of range
		if ( ! NormalizeSafe(&pca) )
		{
			Compress_TwoColorBest(pBlock,pError,colors,mode,data.loC,data.hiC);
			return false;
		}
	}
	
	// dot the colors in the PCA linear fit direction
	// pca vector is normalized, so dots are in units of pixel levels (0-255)
	F64 sumSqr = 0;
	F64 minDot =  999999.f;
	F64 maxDot = -999999.f;
	for(int i=0;i<16;i++)
	{
		rrVec3f vec = ColorToVec3f( colors.colors[i] );
		rrVec3f dv = vec - avgF;
		F64 dot = dv * pca;
		// sum of all dots is zero by definition of "avg"
		sumSqr += dot*dot;
		minDot = RR_MIN(minDot,dot);
		maxDot = RR_MAX(maxDot,dot);
	}
	
	RR_ASSERT( minDot <= 0 && maxDot >= 0 );
		
	// first try the end points :
	{
		rrVec3f end_lo = avgF + (float)minDot * pca;
		rrVec3f end_hi = avgF + (float)maxDot * pca;
		
		rrColor32BGRA color_lo = Vec3fToColorClamp(end_lo);
		rrColor32BGRA color_hi = Vec3fToColorClamp(end_hi);
		
		//Compress_EndPoints(pBlock,pError,colors,mode,color_lo,color_hi);
		Compress_TwoColorBest(pBlock,pError,colors,mode,color_lo,color_hi);
		// @@ or Compress_TwoColorBest ?

		// (if you also do it below) :
		// Compress_TwoColorBest :
		// rmse = 2.656259, sad = 1.788358
		// Compress_EndPoints :
		// rmse = 2.586839, sad = 1.743666
	}
	
	F64 variance = sumSqr/16.0;
	RR_ASSERT( variance >= 0 );
	
	F64 sdev = sqrt( variance );
	RR_ASSERT( sdev < 256 );
	
//	const int num_passes = 8; // 4 is almost as good as 8
	const int num_passes = 4; // 4 is almost as good as 8
//rmse = 2.569575, sad = 1.735196  num_passes = 4
//rmse = 2.558271, sad = 1.730160  num_passes = 8
	for(int pass=0;pass<num_passes;pass++)
	{
		// try 3/4 * sdev to 6/4 * sdev
		//F64 spread = (sdev * (num_passes + pass - (num_passes/4))) * (1.0 / num_passes);
		F64 spread = (sdev * 0.75) + sdev * pass / (F64)num_passes;
		
		if ( spread < 4 ) continue; // < 4 is pointless
		
		rrVec3f delta = (float)spread * pca;
		
		rrVec3f end_lo = avgF - delta;
		rrVec3f end_hi = avgF + delta;
		
		rrColor32BGRA color_lo = Vec3fToColorClamp(end_lo);
		rrColor32BGRA color_hi = Vec3fToColorClamp(end_hi);
		
		//Compress_EndPoints(pBlock,pError,colors,mode,color_lo,color_hi);
		Compress_TwoColorBest(pBlock,pError,colors,mode,color_lo,color_hi);
		// @@ or Compress_TwoColorBest ?
		//	note that TwoColorBest is also doing axis stretches so there's a lot of redundancy here
		//	makes more sense to just use _EndPoints here and increase num_passes
	}
	
	return true;
}
#endif

#if 1
/**

rrCompressDXT1_PCA_Squish_All_Clusters

full squish
strict complexity limit by forcing cluster reduction

while squish is a full enum of indexing possibilities along the PCA axis
it fundamentally assumes that colinear along the PCA axis is a good fit for the data
so the exhaustiveness of its search is a bit of a mirage
it is in fact only exhaustive when your color is one dimensional

(for example when two colors are way off the pca line, but have roughly the same dot product
on the pca line, squish will only try clusters with them in one order, but they might want to
cluster the opposite way)

**/

F64 DXT1_OptimizeEndPointsFromIndices_FourC_Error(const U32 indices, const rrColorBlock4x4 & colors);

struct ColorAccum
{
	rrVec3f	color_sum;
	F32	sqrcolor_sum;
	F32	count;
};

ColorAccum operator - (const ColorAccum &lhs, const ColorAccum &rhs)
{
	ColorAccum ret;
	ret.color_sum = lhs.color_sum - rhs.color_sum;
	ret.sqrcolor_sum = lhs.sqrcolor_sum - rhs.sqrcolor_sum;
	ret.count = lhs.count - rhs.count;
	return ret;
}

F64 DXT1_OptimizeEndPointsFromIndices_FourC_Error(
		const ColorAccum & group_0,const ColorAccum & group_13,const ColorAccum & group_23,const ColorAccum & group_1,
		U32 * pEndPoints);
		
F64 DXT1_OptimizeEndPointsFromIndices_3C_Error(
		const ColorAccum & group_0,const ColorAccum & group_12,const ColorAccum & group_1,
		U32 * pEndPoints);

// for 4C , num_colors == 16
// for 3C , num_colors is with blacks excluded
int make_accum_clusters(ColorAccum color_sum_to_end[17], const rrVec3f * colors, int num_colors,
						const rrVec3f & avgF)
{
	RR_ASSERT( num_colors >= 2 && num_colors <= 16 );

	//rrVec3f pca(1,1,1);
	F32 biggest_lensqr = 0.f;

	//cov of just the on-blacks :
	rrMat3 cov;
	rrMat3_SetZero(&cov);
	for(int i=0;i<num_colors;i++)
	{
		rrVec3f d = colors[i] - avgF;
		F32 lensqr = (F32) LengthSqr(d);
		if ( lensqr > biggest_lensqr )
		{
			biggest_lensqr = lensqr;
			//pca = d; // pca seed @@ this is better on FRYMIRE
		}
		cov[0].x += d.x * d.x;
		cov[0].y += d.x * d.y;
		cov[0].z += d.x * d.z;
		cov[1].y += d.y * d.y;
		cov[1].z += d.y * d.z;
		cov[2].z += d.z * d.z;
	}
	cov[1].x = cov[0].y;
	cov[2].x = cov[0].z;
	cov[2].y = cov[1].z;

	if ( biggest_lensqr < 9.f )
	{
		// @@ degenerate
		// all non-black colors are equal
		//	do a 3c single color fit
		return 0;
	}

	// power iteration for eigenvector :
	// @@ choice of seed *does* seem to matter about 0.005 rmse
	// most of all we just need something that is not perpendicular to the true principle axis
	//	eg doing (1,1,1) is very bad because the true axis can be (0,1,-1)
	//	that actually happens quite often any time you have 2 colors in a block
	//	happens a lot on FRYMIRE ; breakpoint on the return 0 below and see how often that is hit
	F64 l0 = LengthSqr(cov[0]);
	F64 l1 = LengthSqr(cov[1]);
	F64 l2 = LengthSqr(cov[2]);
	rrVec3f pca;
	if ( l0 >= l1 && l0 >= l2 ) pca = cov[0];
	else if ( l1 >= l2 ) pca = cov[1];
	else pca = cov[2];
	
	for(int iter=0;iter<4;iter++)
	{
		pca = cov * pca;
		pca = cov * pca;
		// technically I could just normalize at the end, but the floats get blown out of range
		if ( ! NormalizeSafe(&pca) )
		{
			return 0;
		}
	}
	
	// dot the colors in the PCA linear fit direction
	// pca vector is normalized, so dots are in units of pixel levels (0-255)
	U32 dot_and_index[17];
	dot_and_index[num_colors] = (1<<29) | 16; // past end
	F64 minDot=0,maxDot=0;
	for LOOP(i,num_colors)
	{
		rrVec3f vec = colors[i];
		rrVec3f dv = vec - avgF;
		F64 dot = dv * pca;
		minDot = RR_MIN(minDot,dot);
		maxDot = RR_MAX(maxDot,dot);
		// maximum dot is 128*3
		// pack dot to int with index at the bottom :
		RR_ASSERT( dot > -1024 && dot < 1024 );
		#define PCA_AXIS_TO_INT_SCALE	16	// how much fractional precision along axis; this is 1 pixel value step
		U32 doti = (U32)((dot+1024.0)*PCA_AXIS_TO_INT_SCALE);
		dot_and_index[i] = (doti<<8) | i;
	}

	RR_ASSERT( minDot <= 0 && maxDot >= 0 );
	F64 dot_range = maxDot - minDot;

	// sort dots low to high :
	RR_NAMESPACE::stdsort(dot_and_index,dot_and_index+num_colors);
	RR_ASSERT( dot_and_index[0] <= dot_and_index[1] );
	
	// color_sum_to_end[i] contains the sum of all colors from [i] to end	
	// so make the tail-open interval [i,j) you do
	//	(mask_to_end[i] ^ mask_to_end[j])
	//  = color_sum_to_end[i] - color_sum_to_end[j];
	// color_sum_to_end xyz is rgb , w is color count
	ColorAccum accum;
	RR_ZERO(accum);
	color_sum_to_end[num_colors] = accum;
	for LOOPBACK(i,num_colors)
	{
		int from = dot_and_index[i] & 0xFF; // sorted order index
		rrVec3f cur = colors[from];
		accum.color_sum += cur;
		accum.sqrcolor_sum += (F32)(cur*cur);
		accum.count += 1;
		color_sum_to_end[i] = accum;
	}
	RR_ASSERT( color_sum_to_end[0].count == num_colors );
	
	// reduce the cluster count
	//	by forcing degenerate points to go in the same cluster
	//	if adjacent dots are within epsilon, don't split at that point
	// you can do this neatly by just sliding down mask_to_end[]

	// mask_to_end[i] is the set from [i,16)
	int num_clusters = num_colors;
	
	U32 tiny_dot = (U32)((dot_range*PCA_AXIS_TO_INT_SCALE)/32); // (1/32) of the pca axis extents
	tiny_dot = RR_MAX(tiny_dot,PCA_AXIS_TO_INT_SCALE); // at least 1 pel
	// @@@@ you can definitely make tiny_dot larger and get fewer clusters
	//		to trade off speed vs quality
		
	for(int t=num_colors-2;t>=0;t--)
	{
		U32 d1 = dot_and_index[t];
		U32 d2 = dot_and_index[t+1];
		RR_ASSERT( d2 >= d1 );
		U32 delta = (d2 - d1)>>8;
		if ( delta < tiny_dot )
		{
			RR_ASSERT( num_clusters > 0 );
			
			// eliminate the option of dividing t from t+1
			// keep mask_to_end[t] the same
			//	get rid of mask_to_end[t+1]
			// slide down everything that follows
			memmove(color_sum_to_end+t+1,color_sum_to_end+t+2,(num_clusters-1-t)*sizeof(color_sum_to_end[0]));
			memmove(dot_and_index+t+1,dot_and_index+t+2,(num_clusters-2-t)*sizeof(U32));
			num_clusters--;
		}
	}
	
	#if 1
	// optional :
	//	reduce even more than tiny_dot gots us -
	//  rmse = 9.380450, sad = 6.554615 vs rmse = 9.371775, sad = 6.549979
	// forcing reducing to 8 has almost no loss
	//	@@ further reduction may be worth it for space-speed
	// number of indices to try is O(num_clusters^3) so keeping it small helps a lot
	while( num_clusters > 8 ) 
	{
		// find smallest delta :
		U32 smallest_delta = (U32)-1;
		
		for(int t=num_clusters-2;t>=0;t--)
		{
			U32 d1 = dot_and_index[t];
			U32 d2 = dot_and_index[t+1];
			U32 delta = d2-d1;
			smallest_delta = RR_MIN(delta,smallest_delta);
		}
		
		RR_ASSERT( smallest_delta >= tiny_dot );
		
		for(int t=num_clusters-2;t>=0;t--)
		{
			U32 d1 = dot_and_index[t];
			U32 d2 = dot_and_index[t+1];
			RR_ASSERT( (d2 - d1) >= smallest_delta );
			if ( (d2 - d1) == smallest_delta )
			{
				memmove(color_sum_to_end+t+1,color_sum_to_end+t+2,(num_clusters-1-t)*sizeof(color_sum_to_end[0]));
				memmove(dot_and_index+t+1,dot_and_index+t+2,(num_clusters-2-t)*sizeof(U32));
				num_clusters--;
			}
		}
	}
	#endif
	
	RR_ASSERT( color_sum_to_end[0].count == num_colors ); // all
	RR_ASSERT( color_sum_to_end[num_clusters].count == 0 ); // none

	return num_clusters;
}
		
bool rrCompressDXT1_PCA_Squish_All_Clusters_4C(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	rrVec3f avgF = Vec3i_to_Vec3f(data.avg);
	
	rrVec3f colorsF[16];
	for(int i=0;i<16;i++)
	{
		colorsF[i] = ColorToVec3f( colors.colors[i] );
	}
	
	ColorAccum color_sum_to_end[17];
	int num_clusters = make_accum_clusters(color_sum_to_end,colorsF,16,avgF);
	
	if ( num_clusters <= 1 )
	{
		Compress_TwoColorBest(pBlock,pError,colors,mode,data.loC,data.hiC);
		return false;
	}
	
	RR_ASSERT( color_sum_to_end[0].count == 16.f ); // all
	RR_ASSERT( color_sum_to_end[num_clusters].count == 0 ); // none

	F64 best_err = RR_F32_MAX;
	U32 best_endpoints = 0;

	// [0,i) -> 0
	// [i,j) -> 1/3 (index 2)
	// [j,k) -> 2/3 (index 3)
	// [k,16)-> 1.0 (index 1)
		
	// if you did the enumeration of each set from 0 to == num_clusters
	//	you would consider some degenerate cases
	//  eg. flat blocks with all the indices the same for all 4 index options

	// I force the degeneracy broken
	//	the 0 and 1/3 points cannot go all the way to the last index
	//  and the 2/3 and 1 point cannot be the first index
	// NOTE therefore we do not consider flat blocks here at all
	//	you should seed pError with flat block before calling this
	//	(rrCompressDXT1_Startup does so)

	for(int i=0;i<num_clusters;i++)	// not == num_clusters, no need to look at all 0's
	{		
	for(int j=RR_MAX(i,1);j<num_clusters;j++) // j != 0 and != num_clusters ; this prevents flat blocks
	{		
	for(int k=j;k<=num_clusters;k++) // no k = 0, forces first index to be a 0 or 1/3
	{			
		// [0,i) -> 0
		// [i,j) -> 1/3 (index 2)
		// [j,k) -> 2/3 (index 3)
		// [k,16) -> 1
		
		ColorAccum group_0  = color_sum_to_end[0] - color_sum_to_end[i];
		ColorAccum group_13 = color_sum_to_end[i] - color_sum_to_end[j];
		ColorAccum group_23 = color_sum_to_end[j] - color_sum_to_end[k];
		ColorAccum group_1  = color_sum_to_end[k];
		
		U32 endpoints = 0;
		F64 err = DXT1_OptimizeEndPointsFromIndices_FourC_Error(group_0,group_13,group_23,group_1,&endpoints);

		#if 0
		{
		// verify err calculation
		// won't be exactly the same because this does the true interpolants
		//	but should be close
		
		U32 err32 = 0;
		DXT1_FindIndices(colors, endpoints,rrDXT1PaletteMode_NoAlpha,&err32);

		F64 derr = sqrt(err/16.0) - sqrt(err32/16.0);
		derr = RR_ABS(derr);
		RR_ASSERT( derr < 8 || err32 < err ); // sometimes true err32 is much less
		}
		#endif
	
		if ( err < best_err )
		{
			best_err = err;
			best_endpoints = endpoints;
			// could save this indexing,
			//	but we'll just save the endpoints and remake it
			//	re-finding indexes must be strictly better than saving them
		}
	}
	}
	}
	
	// saved endpoints from the best guessed err; re-find indices on them
	U32 err = RR_DXTC_ERROR_BIG;
	U32 best_indices = DXT1_FindIndices(colors,best_endpoints,mode,&err);
	if ( err < *pError)
	{
		*pError = err;
		pBlock->endpoints = best_endpoints;
		pBlock->indices = best_indices;
	}
	// one step of re-find endpoints now from these indices
	DXT1_OptimizeEndPointsFromIndices_Fourc_Reindex(pBlock,pError,best_indices,colors,mode);
		
	return true;
}

/**
rrCompressDXT1_Degen_3C :
block is degenerate (single color)
AND also has blacks or transparents
so do 3c single color for the color part
and find the 3rd index special assignments too
**/
bool rrCompressDXT1_Degen_3C(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colorblock, rrDXT1PaletteMode mode,
	const rrColor32BGRA & avg_color)
{	
	// make 3c single color :
	rrDXT1Block scBlock;
	Compress_SingleColor_Compact_3C(&scBlock,avg_color);
	
	RR_ASSERT( mode != rrDXT1PaletteMode_FourColor );
	RR_ASSERT( ! DXT1_Is4Color(scBlock,mode) );
	
	// FindInd to either get the single color or 3rd (transparent or black depending on mode)
	U32 err;
	U32 ind = DXT1_FindIndices(colorblock,scBlock.endpoints,mode,&err);
	if ( err < *pError )
	{
		*pError = err;
		pBlock->endpoints = scBlock.endpoints;
		pBlock->indices = ind;
		return true;
	}
	return false;
}
			
bool rrCompressDXT1_PCA_Squish_All_Clusters_3C(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colorblock, rrDXT1PaletteMode mode)
{
	if ( mode == rrDXT1PaletteMode_FourColor ) return false; // BC3
	
	if ( mode != rrDXT1PaletteMode_NoAlpha )
	{
		// don't use in forced 4C BC3
		// @@ should support rrDXT1PaletteMode_Alpha too, but in that case rather than factor out the blacks
		//	you factor out the transparent
		return false;
	}
	
	// no Startup
	//	assume 4C has already been done to try solid color and such

	// gather non-black colors:
	//	(in Alpha mode, gather non-transparent colors)
	rrVec3f colors[16];
	int num_colors = 0;
	rrVec3f sum(0,0,0);
	
	for(int i=0;i<16;i++)
	{
		rrVec3f cur = ColorToVec3f( colorblock.colors[i] );
		
		if ( mode == rrDXT1PaletteMode_NoAlpha )
		{
			if ( LengthSqr(cur) < 3*(BLACKNESS_DISTANCE*BLACKNESS_DISTANCE) )
				continue; // is black, don't add to active colors
		}
		else
		{
			RR_ASSERT( mode == rrDXT1PaletteMode_Alpha );
			if ( rrColor32BGRA_IsOneBitTransparent(colorblock.colors[i]) )
				continue; // is transparent, don't add to active colors
		}
		
		colors[num_colors] = cur;
		num_colors++;

		sum += cur;
	}
	
	// black & transparent are handled below
	// we find endpoints on the active set of colors (non-black/trans)
	// then after we do a FindIndices using those endpoints
	//	that will assign the 3rd index to the special case colors
	
	if ( num_colors == 16 )
	{
		// no blacks
		// @@ test if it helps to still go ahead and do 3c fit here at max quality
		//	I mean yes, it definitely does help quality
		//	the question is how much
		//	 in this case, all the make_accum_clusters is identical to the 4c case
		//	 the only difference is the iteration at the end
		//	 so we'd rather return here and do the 3c iteration in the 4c clusters
		// BUT if there are blacks, don't do it there and also here
		//	need to unify that decision
		return false;
	}
	if ( num_colors < 2 )
	{
		// if num_colors == 0 , sum = 0 = transparent black, go ahead and run that
		return rrCompressDXT1_Degen_3C(pBlock,pError,colorblock,mode,Vec3fToColor(sum));
			
		/*
		if ( num_colors == 1 )
		{
			return rrCompressDXT1_Degen_3C(pBlock,pError,colorblock,mode,sum);
		}
		// num_colors == 0 is all black!
		//   assume it was handled by Startup degenerate cases in 4C trial
		// @@ this happens a decent amount on FRYMIRE with blocks that aren't actually all black
		//	the issue is our "blackness threshold" is quite big
		//	you could do something like repeat with a smaller blackness threshold
		return false;	
		*/	
	}	

	rrVec3f avgF = sum * (1.f/num_colors);
	
	ColorAccum color_sum_to_end[17];
	int num_clusters = make_accum_clusters(color_sum_to_end,colors,num_colors,avgF);

	if ( num_clusters <= 1 ) // also pca degeneracies
	{
		// handle num_colors == 1
		//	do a 3c single color fit
		return rrCompressDXT1_Degen_3C(pBlock,pError,colorblock,mode,Vec3fToColor(avgF));
	}

	F64 best_err = RR_F32_MAX;
	U32 best_endpoints = 0;

	// [0,i) -> 0
	// [i,j) -> 1/2 (index 2)
	// [j,num_clusters)-> 1.0 (index 1)

	// @@ not sure about the degeneracy breaking rules here;
	//	compare to doing them all (j =i and i == num_clusters)
	//	this does enumerate i==0 and j==num_clusters which is flat with all at 1/2
	for(int i=0;i<num_clusters;i++)	// not == num_clusters, no need to look at all 0's
	{		
	for(int j=RR_MAX(i,1);j<=num_clusters;j++) // j != 0 ; this prevents flat blocks
	{
		
		ColorAccum group_0  = color_sum_to_end[0] - color_sum_to_end[i];
		ColorAccum group_12 = color_sum_to_end[i] - color_sum_to_end[j];
		ColorAccum group_1  = color_sum_to_end[j];
		
		U32 endpoints = 0;
		F64 err = DXT1_OptimizeEndPointsFromIndices_3C_Error(group_0,group_12,group_1,&endpoints);

		#if 0
		{
		// verify err calculation
		// won't be exactly the same because this does the true interpolants
		//	but should be close
		
		U32 err32 = 0;
		DXT1_FindIndices(colorblock, endpoints,rrDXT1PaletteMode_NoAlpha,&err32);

		F64 derr = sqrt(err/16.0) - sqrt(err32/16.0);
		derr = RR_ABS(derr);
		RR_ASSERT( derr < 8 || err32 < err ); // sometimes true err32 is much less
		}
		#endif
	
		if ( err < best_err )
		{
			best_err = err;
			best_endpoints = endpoints;
			// could save this indexing,
			//	but we'll just save the endpoints and remake it
			//	re-finding indexes must be strictly better than saving them
		}
	}
	}
	
	// saved endpoints from the best guessed err; re-find indices on them
	U32 err = RR_DXTC_ERROR_BIG;
	U32 best_indices = DXT1_FindIndices(colorblock,best_endpoints,mode,&err);
	if ( err < *pError)
	{
		*pError = err;
		pBlock->endpoints = best_endpoints;
		pBlock->indices = best_indices;
	}
	// one step of re-find endpoints now from these indices
	DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(pBlock,pError,colorblock,mode);
		
	return true;
}

bool rrCompressDXT1_PCA_Squish_All_Clusters(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	SIMPLEPROFILE_SCOPE(BC1_PCA_Squish_All_Clusters);

	if ( ! rrCompressDXT1_PCA_Squish_All_Clusters_4C(data,pBlock,pError,colors,mode) )
		return false;
	
	rrCompressDXT1_PCA_Squish_All_Clusters_3C(pBlock,pError,colors,mode);
	
	return true;
}

#endif

#if 0
bool rrCompressDXT1_PCA_Squish_All_Clusters_Old(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	//SIMPLEPROFILE_SCOPE(rrCompressDXT1_PCA_Squish_All_Clusters);
		
	rrVec3i diagonal = data.diagonal;
	rrVec3f avgF = Vec3i_to_Vec3f(data.avg);
	
	//rrVec3i cov[3] = { 0 };
	rrMat3 cov;
	rrMat3_SetZero(&cov);
	for(int i=0;i<16;i++)
	{
		rrVec3f d = ColorToVec3f( colors.colors[i] ) - avgF;
		cov[0].x += d.x * d.x;
		cov[0].y += d.x * d.y;
		cov[0].z += d.x * d.z;
		cov[1].y += d.y * d.y;
		cov[1].z += d.y * d.z;
		cov[2].z += d.z * d.z;
	}
	cov[1].x = cov[0].y;
	cov[2].x = cov[0].z;
	cov[2].y = cov[1].z;

	// power iteration for eigenvector :
	rrVec3f pca = Vec3i_to_Vec3f(diagonal);
	
	for(int iter=0;iter<4;iter++)
	{
		pca = cov * pca;
		pca = cov * pca;
		// technically I could just normalize at the end, but the floats get blown out of range
		if ( ! NormalizeSafe(&pca) )
		{
			Compress_TwoColorBest(pBlock,pError,colors,mode,data.loC,data.hiC);
			return false;
		}
	}
	
	#if 0
	// idea :
	// use covariance to get the off-pca-axis dimensions
	// tells us how well the colors fit the "linear along pca axis" model
	
	/*
	{	
	// slow way to verify :
	
	rrVec3f pca_mul = cov * pca;
	F32 eigen1;
	if ( RR_ABS(pca.x) >= RR_ABS(pca.y) && RR_ABS(pca.x) >= RR_ABS(pca.z) )
		eigen1 = pca_mul.x / pca.x;
	else if ( RR_ABS(pca.y) >= RR_ABS(pca.z) )
		eigen1 = pca_mul.y / pca.y;
	else
		eigen1 = pca_mul.z / pca.z;
		
	rrVec3f eigenvalues;
	rrMat3 eigenvectors;
	if ( ! rrMat3_EigenSolveSymmetric(cov,&eigenvalues,&eigenvectors) )
		return false;
	
	// eigen1 == eigenvalues.x
	// pca == eigenvectors[0]
	
	rrMat3 cov_reduced;
	F64 cov_reduced_sum_sqr = 0;
	for LOOP(i,3)
	{
		for LOOP(j,3)
		{
			cov_reduced[i][j] = cov[i][j] - eigen1 * pca[i] * pca[j];
			cov_reduced_sum_sqr += cov_reduced[i][j] * cov_reduced[i][j];
		}
	}
	F32 cov_reduced_trace = cov_reduced[0][0] + cov_reduced[1][1] + cov_reduced[2][2];
	F32 eigen_yz_l1 = eigenvalues.y + eigenvalues.z;
	// cov_reduced_trace == eigen_yz_l1
	
	F32 cov_reduced_sum = sqrt(cov_reduced_sum_sqr);
	F32 eigen_yz_l2 = sqrt( eigenvalues.y*eigenvalues.y + eigenvalues.z*eigenvalues.z );
	// cov_reduced_sum == eigen_yz_l2
	eigen_yz_l2 = eigen_yz_l2;
	}	
	/**/
	
	// do pca * cov to get primary eigenvalue
	// only need one component of the matrix multiply
	//  (pca * cov).x/pca.x
	
	F64 eigen1;
	if ( RR_ABS(pca.x) >= RR_ABS(pca.y) && RR_ABS(pca.x) >= RR_ABS(pca.z) )
		eigen1 = (cov[0] * pca) / pca.x;
	else if ( RR_ABS(pca.y) >= RR_ABS(pca.z) )
		eigen1 = (cov[1] * pca) / pca.y;
	else
		eigen1 = (cov[2] * pca) / pca.z;
	
	// trace of covariance is the sum of eigenvalues
	F64 cov_trace = cov[0][0] + cov[1][1] + cov[2][2];
	F64 cov_reduced_trace = cov_trace - eigen1;
	// cov_reduced_trace == eigen2 + eigen3
	
	// L2 eigen ratio :
	//F64 eigen_ratio = cov_reduced_sum_sqr / (eigen1*eigen1);
	// L1 eigen ratio :
	F64 eigen_ratio = cov_reduced_trace / eigen1;
	// eigen_ratio is in [0,2]
	// when eigen_ratio is near zero, colors are very colinear
	// ?? use eigen_ratio threshold to do squish or 8means ??
	// -> no this doesn't work
	//	8means is just always better , the optimal threshold is 0
	if ( eigen_ratio > 0.1 )
	//if ( 1 ) // 383.748
	{
		// wide covariance
		// don't do Squish on PCA axis
		// instead do 8means
	
		rrCompressDXT1_8Means(pBlock,pError,colors,mode);
		
		DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
		
		return true;
	}
	#endif
	
	// dot the colors in the PCA linear fit direction
	// pca vector is normalized, so dots are in units of pixel levels (0-255)
	U32 dot_and_index[17];
	dot_and_index[16] = (U32)-1; // to make sure this isn't touched
	F64 minDot=0,maxDot=0;
	for LOOP(i,16)
	{
		rrVec3f vec = ColorToVec3f( colors.colors[i] );
		rrVec3f dv = vec - avgF;
		F64 dot = dv * pca;
		minDot = RR_MIN(minDot,dot);
		maxDot = RR_MAX(maxDot,dot);
		// maximum dot is 128*3
		// pack dot to int with index at the bottom :
		RR_ASSERT( dot > -400 && dot < 400 );
		#define PCA_AXIS_TO_INT_SCALE	16	// how much fractional precision along axis; this is 1 pixel value step
		U32 doti = (U32)((dot+512.0)*PCA_AXIS_TO_INT_SCALE);
		dot_and_index[i] = (doti<<8) | i;
	}

	RR_ASSERT( minDot <= 0 && maxDot >= 0 );
	F64 dot_range = maxDot - minDot;

	// sort dots low to high :
	RR_NAMESPACE::stdsort(dot_and_index,dot_and_index+16);
	RR_ASSERT( dot_and_index[0] <= dot_and_index[1] );
	
	// change dot_and_index to shift :
	//	shift tells you where your 2 bit index goes in dot-sorted order
	// can make the clusters with masks of runs to the end
	// mask_to_end[i] has 3 (two bits) set for all indices in [i,16] (in dot-sorted order)
	// so make the tail-open interval [i,j) you do
	//	(mask_to_end[i] ^ mask_to_end[j])
	//	or mask_to_end[i] & ~mask_to_end[j] 
	// to make the interval be 1 or 2, just & with all_1s or all_2s
	U32 mask_to_end[18];
	U32 mask = 0xFFFFFFFF;
	for LOOP(i,16) 
	{
		int shift = (dot_and_index[i] & 0xF)*2;
		//dot_and_index[i] = shift; // store the shift for slow_indices (old)
		dot_and_index[i] >>= 8; // change it back to just the dot for cluster merging
		mask_to_end[i] = mask;
		mask ^= 3 << shift;
	}
	RR_ASSERT( mask == 0 );
	mask_to_end[16] = 0;
	mask_to_end[17] = (U32)-1; // to make sure this isn't touched

	const U32 all_2s = 0xAAAAAAAA;
	const U32 all_1s = 0x55555555;
			
	// now try all clusters :
	
	// remember indices are not in order :
	// 4color : end0, end1, 1/3, 2/3

	// reduce the cluster count
	//	by forcing degenerate points to go in the same cluster
	//	if adjacent dots are within epsilon, don't split at that point
	// you can do this neatly by just sliding down mask_to_end[]

	// mask_to_end[i] is the set from [i,16)
	int num_clusters = 16;
	
	//U32 tiny_dot = 32; // rmse = 9.391170, sad = 6.611921
	//U32 tiny_dot = 16; // rmse = 9.368147, sad = 6.547235
	// 16 = one pel in our int scaling
	// rmse = 9.371775, sad = 6.549979
	U32 tiny_dot = (U32)((dot_range*PCA_AXIS_TO_INT_SCALE)/32); // (1/32) of the pca axis extents
	tiny_dot = RR_MAX(tiny_dot,PCA_AXIS_TO_INT_SCALE); // at least 1 pel
	// @@@@ you can definitely make tiny_dot larger and get fewer clusters
	//		to trade off speed vs quality
		
	for(int t=14;t>=0;t--)
	{
		U32 d1 = dot_and_index[t];
		U32 d2 = dot_and_index[t+1];
		RR_ASSERT( d2 >= d1 );
		if ( (d2 - d1) < tiny_dot )
		{
			// eliminate the option of dividing t from t+1
			// keep mask_to_end[t] the same
			//	get rid of mask_to_end[t+1]
			// slide down everything that follows
			memmove(mask_to_end+t+1,mask_to_end+t+2,(num_clusters-1-t)*sizeof(U32));
			memmove(dot_and_index+t+1,dot_and_index+t+2,(num_clusters-2-t)*sizeof(U32));
			num_clusters--;
		}
	}
	RR_ASSERT( mask_to_end[num_clusters] == 0 );
	
	// optional :
	//	reduce even more than tiny_dot gots us -
	//  rmse = 9.380450, sad = 6.554615 vs rmse = 9.371775, sad = 6.549979
	// forcing reducing to 8 has almost no loss
	//	@@ further reduction may be worth it for space-speed
	// number of indices to try is O(num_clusters^3) so keeping it small helps a lot
	//while( num_clusters > 12 ) // 10.89
	while( num_clusters > 8 ) // 10.90
	//while( num_clusters > 6 ) // 10.95
	// vs 4-means : 11.02
	// num_clusters = 4 would just make this a crappy version of 4-means
	{
		// find smallest delta :
		U32 smallest_delta = 9999999;
		
		for(int t=num_clusters-2;t>=0;t--)
		{
			U32 d1 = dot_and_index[t];
			U32 d2 = dot_and_index[t+1];
			U32 delta = d2-d1;
			smallest_delta = RR_MIN(delta,smallest_delta);
		}
		
		RR_ASSERT( smallest_delta >= tiny_dot );
		
		for(int t=num_clusters-2;t>=0;t--)
		{
			U32 d1 = dot_and_index[t];
			U32 d2 = dot_and_index[t+1];
			RR_ASSERT( (d2 - d1) >= smallest_delta );
			if ( (d2 - d1) == smallest_delta )
			{
				memmove(mask_to_end+t+1,mask_to_end+t+2,(num_clusters-1-t)*sizeof(U32));
				memmove(dot_and_index+t+1,dot_and_index+t+2,(num_clusters-2-t)*sizeof(U32));
				num_clusters--;
			}
		}
	}
	
	RR_ASSERT( mask_to_end[0] == 0xFFFFFFFF ); // all
	RR_ASSERT( mask_to_end[num_clusters] == 0 ); // none

	F64 best_err = RR_F32_MAX;
	U32 best_indices = 0;

	// [0,i) -> 0
	// [i,j) -> 1/3 (index 2)
	// [j,k) -> 2/3 (index 3)
	// [k,16)-> 1.0 (index 1)
		
	// if you did the enumeration of each set from 0 to == num_clusters
	//	you would consider some degenerate cases
	//  eg. flat blocks with all the indices the same for all 4 index options

	// I force the degeneracy broken
	//	the 0 and 1/3 points cannot go all the way to the last index
	//  and the 2/3 and 1 point cannot be the first index
	// NOTE therefore we do not consider flat blocks here at all
	//	you should seed pError with flat block before calling this
	//	(rrCompressDXT1_Startup does so)

	// test7 full enumeration : 9.3804 , degeneracy broken : 9.3812

	for(int i=0;i<num_clusters;i++)	// not == num_clusters, no need to look at all 0's
	{		
	for(int j=RR_MAX(i,1);j<num_clusters;j++) // j != 0 and != num_clusters ; this prevents flat blocks
	{		
	for(int k=j;k<=num_clusters;k++) // no k = 0, forces first index to be a 0 or 1/3
	{			
		/*
		U32 slow_indices = 0;
		
		// [0,i) -> 0
		for(int t=i;t<j;t++)
		{
			slow_indices |= 2 << dot_and_index[t];
		}		
		for(int t=j;t<k;t++)
		{
			slow_indices |= 3 << dot_and_index[t];
		}
		for(int t=k;t<16;t++)
		{
			slow_indices |= 1 << dot_and_index[t];
		}
		*/
		
		U32 indices = all_2s & (mask_to_end[i] ^ mask_to_end[j]);
		indices |= (mask_to_end[j] ^ mask_to_end[k]);
		indices |= all_1s & mask_to_end[k];
		//RR_ASSERT( indices == slow_indices );
				
		//DXT1_OptimizeEndPointsFromIndices_Fourc_Reindex(pBlock,pError,indices,colors,mode);
		//DXT1_OptimizeEndPointsFromIndices_Fourc_NoReindex(pBlock,pError,indices,colors,mode,true);
		
		// Is full reindexing really necessary here?
		//	shouldn't our cluster enum cover all reindexes anyway?
		//	(note: if you don't reindex then you do have to at least do flip(indices)
		//	 when the endpoints get swapped to maintain fourc order)
		
		//Reindex:
		//rmse_total = 396.241

		//NoReindex :
		//rmse_total = 399.187
		//NoReindex : with non-degeneracy-breaking i,j,k loop iter
		//rmse_total = 398.853
		
		// conclusion: Reindex does help a little
		//	 it sometimes chooses non-consecutive-cluster indexings
		//	 that have small wins in the int quantization

		
		//*
		// experiment :
		// can we just compute the error of the lsqr without actually making endpoints?
		//   just take the best of these and reindex at the end
		// -> YES this looks totally fine now if you quantize endpoints
		//  at level 4 this hurts about 0.001 rmse
		F64 err = DXT1_OptimizeEndPointsFromIndices_FourC_Error(indices,colors);
		if ( err < best_err )
		{
			best_err = err;
			best_indices = indices;
		}
		/**/
	}
	}
	}
	
	DXT1_OptimizeEndPointsFromIndices_Fourc_Reindex(pBlock,pError,best_indices,colors,mode);
	
	return true;
}
#endif

#if 0
// rrCompressDXT1_PCA_Squish_Approx
//	NOT cluster fit just heuristic
// -> this was not a useful space-speed tradeoff
bool rrCompressDXT1_PCA_Squish_Approx(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{	
	rrVec3i diagonal = data.diagonal;
	rrVec3f avgF = Vec3i_to_Vec3f(data.avg);
	
	//rrVec3i cov[3] = { 0 };
	rrMat3 cov;
	rrMat3_SetZero(&cov);
	for(int i=0;i<16;i++)
	{
		rrVec3f d = ColorToVec3f( colors.colors[i] ) - avgF;
		cov[0].x += d.x * d.x;
		cov[0].y += d.x * d.y;
		cov[0].z += d.x * d.z;
		cov[1].y += d.y * d.y;
		cov[1].z += d.y * d.z;
		cov[2].z += d.z * d.z;
	}
	cov[1].x = cov[0].y;
	cov[2].x = cov[0].z;
	cov[2].y = cov[1].z;

	// power iteration for eigenvector :
	rrVec3f pca = Vec3i_to_Vec3f(diagonal);
	
	for(int iter=0;iter<4;iter++)
	{
		pca = cov * pca;
		pca = cov * pca;
		// technically I could just normalize at the end, but the floats get blown out of range
		if ( ! NormalizeSafe(&pca) )
		{
			Compress_TwoColorBest(pBlock,pError,colors,mode,data.loC,data.hiC);
			return false;
		}
	}
		
	// dot the colors in the PCA linear fit direction
	// pca vector is normalized, so dots are in units of pixel levels (0-255)
	U32 dot_and_index[16];
	for LOOP(i,16)
	{
		rrVec3f vec = ColorToVec3f( colors.colors[i] );
		rrVec3f dv = vec - avgF;
		F64 dot = dv * pca;
		// maximum dot is 128*3
		// pack dot to int with index at the bottom :
		RR_ASSERT( dot > -400 && dot < 400 );
		#define PCA_AXIS_TO_INT_SCALE	16	// how much fractional precision along axis; this is 1 pixel value step
		U32 doti = (U32)((dot+512.0)*PCA_AXIS_TO_INT_SCALE);
		dot_and_index[i] = (doti<<8) | i;
	}

	// sort dots low to high :
	RR_NAMESPACE::stdsort(dot_and_index,dot_and_index+16);
	RR_ASSERT( dot_and_index[0] <= dot_and_index[1] );
	
	// the idea here is just to come up with some possible indexings of the points
	// then do endpoints-from-indeces on those
	// and see which is best
	// to make the possible indexings, rather than do a "squish" and try to enumerate all ways
	// we just take pairs of points {i,j} along the PCA axis
	//	and generate and indexing as if those were the endpoints
	// not because we think they are good candidate endpoints
	//	but because it generates an indexing along the PCA axis where i&j are bottom/top index
	
	U32 last_indices = 0;
	for(int i=0;i<8;i++) // first endpoint is in the lower half of the PCA dots
	{		
		// second endpoint must be at least a large step away along the PCA axis
		// we're trying to rule out all the options like {1,2} as candidates
		//for(int j=i+8;j<16;j++)
		for(int j=i+9;j<16;j++)
		{
			int index1 = dot_and_index[i]&0xFF;
			int index2 = dot_and_index[j]&0xFF;
			
			// TwoColorBest just to generate indices :
			rrDXT1Block trial_block;
			U32 trial_err = RR_DXTC_ERROR_BIG;
			Compress_TwoColorBest(&trial_block,&trial_err,colors,mode,colors.colors[index1],colors.colors[index2]);
			//Compress_EndPoints(&trial_block,&trial_err,colors,mode,colors.colors[index1],colors.colors[index2]);
			if ( trial_err < *pError )
			{
				*pError = trial_err;
				*pBlock = trial_block;
			}

			if ( trial_block.indices == last_indices ) continue;
			last_indices = trial_block.indices;
			
			// then EP-from-Ind for the actual work :
			DXT1_OptimizeEndPointsFromIndices_Fourc_Reindex(pBlock,pError,trial_block.indices,colors,mode);
		}
	}
	
	return true;
}
#endif

//===================================================================

// 0 = VeryFast
void rrCompressDXT1_0(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	// @@ : note : this is "VeryFast"
	//	this is really a place-holder
	//	should replace with a good fast version
	//	using SSE2 and the simple divide method or whatever
	//	also doing block-at-a-time is obviously not ideal for VeryFast
	
	*pError = RR_DXTC_INIT_ERROR;

	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode) )
		return;

	if ( ! rrCompressDXT1_2Means(data,pBlock,pError,colors,mode) )
		return;
	
	// added 06-01-2019 :
	// @@ this should be skipped on flat blocks
	//	 and probably other cases where it's unlikely to help
	DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(pBlock,pError,colors,mode);
}

// 1 = Fast
void rrCompressDXT1_1(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode) )
		return;

	if ( ! rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,false) )
		return;
		
	DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
}

// 2 = Slow
void rrCompressDXT1_2(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode)
{
	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode) )
		return;

	#if 0	// TEST SQUISH :
		
	//*
		
	if ( ! rrCompressDXT1_PCA_Squish_All_Clusters(data,pBlock,pError,colors,mode) )
		return;
		
	//DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
	/*/
	
	// 7.3907
	if ( ! rrCompressDXT1_PCA_Squish_All_Clusters_Old(pBlock,pError,colors,mode) )
		return;
		
	/**/
	
	#else

	if ( ! rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,false) )
		return;
		
	// 8 means here is not worth it, a lot slower and no big gains :
	//rrCompressDXT1_8Means(pBlock,pError,colors,mode);

	DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
	#endif
	
	//DXT1_AnnealBlock(pBlock,pError,colors,mode);

	DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,false);
	
	/**/
	
	// verify *pError :
	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );
}

// 3 = VerySlow + Reference
void rrCompressDXT1_3(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXTCOptions options, rrDXT1PaletteMode mode, rrDXTCLevel level)
{
	SIMPLEPROFILE_SCOPE(BC1_Level3);

	*pError = RR_DXTC_INIT_ERROR;
	
	rrCompressDXT1_Startup_Data data;
	if ( ! rrCompressDXT1_Startup(&data,pBlock,pError,colors,mode) )
	{
		// linear_ramp1.BMP still wants Optimize
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
		return;
	}

	// two approaches here, seem to come out quite similar
	//	in terms of overall rmse and run time
	// rmse is similar on all files
	// run time is NOT , some times one approach is much faster, but it's not monotonic
	// overall 8means+squish seems to be slightly better rmse & run time (than 4means + all pairs)

	#if 0
	
	// 4means + all pairs
	
	bool non_degenerate = rrCompressDXT1_4Means(data,pBlock,pError,colors,mode);

	if ( ! non_degenerate )
	{
		// linear_ramp1.BMP still wants Optimize
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(pBlock,pError,colors,mode);
		return;
	}
	
	DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
	if ( level >= rrDXTCLevel_Reference ) 
	{
		// Compress_TryAllPairs_Heavy does its own DXT1_OptimizeEndPointsFromIndicesIterative
		//  this is pretty slow and rarely helps much
		//	 it helps most on the rare weirdo images (frymire/serrano)	
		Compress_TryAllPairs_Heavy(pBlock,pError,colors,mode);
	}
	
	//rmse_total = 382.464

	#elif 1

	// alternate approach : 8means + squish
	//bool non_degenerate = rrCompressDXT1_8Means(pBlock,pError,colors,mode);
	
	// 04-13-2020 : changed to 4Means +PCA
	bool non_degenerate = rrCompressDXT1_4Means(data,pBlock,pError,colors,mode,true);
	
	if ( ! non_degenerate )
	{
		// linear_ramp1.BMP still wants Optimize for degenerate blocks
		//	nobody else cares
		DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
		return;
	}
		
	DXT1_OptimizeEndPointsFromIndicesIterative(pBlock,pError,colors,mode);
	
	if ( level >= rrDXTCLevel_Reference )
	{
		// bc1difficult :
		// with neither :
		// rmse_total = 119.040
		// rmse_total = 118.205 without rrCompressDXT1_PCA_Squish_All_Clusters (yes Compress_TryAllPairs_Heavy)
		// rmse_total = 118.398 without Compress_TryAllPairs_Heavy (yes rrCompressDXT1_PCA_Squish_All_Clusters)
		// rmse_total = 118.166 with Compress_TryAllPairs_Heavy (and rrCompressDXT1_PCA_Squish_All_Clusters)
	
		// can try squish here too to see if it finds something different
		// @@ helps only a little and very slow -> I think this should go
		//	 leaving for now as "ground truth"
		rrCompressDXT1_PCA_Squish_All_Clusters(data,pBlock,pError,colors,mode);
		
		// Compress_TryAllPairs_Heavy does its own DXT1_OptimizeEndPointsFromIndicesIterative
		//  this is pretty slow and rarely helps much
		//	 it helps most on the rare weirdo images (frymire/serrano)	
		Compress_TryAllPairs_Heavy(pBlock,pError,colors,mode);
	}
	
	#else
			
	bool non_degenerate = rrCompressDXT1_PCA_Squish_All_Clusters(data,pBlock,pError,colors,mode);
	// non_degenerate = actually did squishing
	
	if ( ! non_degenerate )
	{
		// degenerate, could not squish
		// early out in this case (skip AnnealBlock)
		//
		// linear_ramp1.BMP still wants Optimize
		//	nobody else cares
		//
		// that is, on most texture you can skip the Optimize here
		//	but the Optimize helps a lot on linear_ramp
		
		DXT1_GreedyOptimizeBlock(pBlock,pError,colors,mode,true);
		return;
	}
		
	#endif

	if ( *pError == 0 ) return; // pretty rare but may as well
	
	if ( 1 ) // level >= rrDXTCLevel_Reference )
	{
		// @@ alternative to Anneal that should be considered
		//	 is just to do a greedy optimize but with randomized larger steps
		//	(you would have to consider joint endpoint moves like dilations & contractions)

		/*
		// Anneal in VerySlow ?
		// yes I guess so
		
		rmse_total = 307.686 Slow
		rmse_total = 307.024 VerySlow
		rmse_total = 306.321 VerySlow with Anneal
		rmse_total = 305.705 Reference
		*/

		DXT1_AnnealBlock(data,pBlock,pError,colors,mode);
	}
	
	DXT1_GreedyOptimizeBlock(data,pBlock,pError,colors,mode,true);
}

//================================================


#define NUM_WIGGLES	(6)	// number of non-null wiggles

// rrColor32BGRA is ARGB in shifts
static const S32 c_wiggledw_delta[8] = { 1<<16,-(1<<16), 1<<8,-(1<<8), 1,-1, 0,0 };

static RADFORCEINLINE rrColor32BGRA Wiggle(const rrColor32BGRA & color,int how)
{
	U32 dw;
	rrColor32BGRA ret;
	
	dw = color.dw;
	RR_ASSERT( (dw & 0xFF1F3F1F) == dw );
	dw += (U32)c_wiggledw_delta[how];
	// if we went out of allowed range on this color,
	// some bits outside of 0x1F3F1F are on; instead
	// of clamping, we can just return the original
	// value (which works out to the same thing)
	ret.dw = (dw & (~0xFF1F3F1F)) ? color.dw : dw;
	
	return ret;
}

void DXT1_GreedyOptimizeBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode, bool do_joint_optimization)
{
	// If the error is already 0, nothing to do
	if ( *pError == 0 )
		return;

	SIMPLEPROFILE_SCOPE(BC1_GreedyOpt);
	
	// Greedy optimization - do after Annealing

	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );

	// these are unpacked to bytes but NOT unquantized :	
	rrColor32BGRA best0_32 = rrColor565Bits_UnPack(pBlock->c0);
	rrColor32BGRA best1_32 = rrColor565Bits_UnPack(pBlock->c1);
	
	for(;;)
	{
		bool didAnything = false;
	
		// these are unpacked to bytes but NOT unquantized :
		rrColor32BGRA start0_32 = best0_32;
		rrColor32BGRA start1_32 = best1_32;
		
		// do_joint_optimization : 
		//	N*N pair wiggles, like end0 +1 in B and end1 -1 in R 
		//	or N+N independent endpoint wiggles (like BC7 does)
		
		// it's a pretty big speed difference
		//	but there is some decent quality available from do_joint_optimization
		// -> for now I'm turning off joint_optimization at level 2 (Slow)
		//	 leaving it on at level >= 3 (VerySlow)
		//	level 3 is annealing too so the speed impact of changing this isn't enormous
		
		if ( do_joint_optimization )
		{
			
			// try all wiggles :
			// 7*7 == 49 trials (actually 48, the both null is skipped)
			for(int w1=0;w1<=NUM_WIGGLES;w1++)
			{
				rrColor32BGRA c0_32 = Wiggle(start0_32,w1);
				
				rrColor32BGRA c0_32_unq = rrColor32BGRA_565_UnQuantize(c0_32);
							
				for(int w2=0;w2<=NUM_WIGGLES;w2++)
				{
					rrColor32BGRA c1_32 = Wiggle(start1_32,w2);
					
					if ( c0_32.dw == start0_32.dw && c1_32.dw == start1_32.dw )
						continue;
									
					rrColor32BGRA c1_32_unq = rrColor32BGRA_565_UnQuantize(c1_32);
					
					// if you have alpha, reject 4c mode :
					if ( data.has_any_alpha && c0_32_unq.dw > c1_32_unq.dw )
						continue;

					rrColor32BGRA palette[4];
					DXT1_ComputePalette(c0_32_unq,c1_32_unq,palette,mode);
					
					U32 err;
					U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
									
					if ( err < *pError )
					{
						// remember best wiggle :
						didAnything = true;
						*pError = err;
						best0_32 = c0_32;
						best1_32 = c1_32;
						pBlock->c0 = rrColor565Bits_Pack(c0_32);
						pBlock->c1 = rrColor565Bits_Pack(c1_32);
						pBlock->indices = indices;
						if ( err == 0 ) return;
					}				
				}
			}

		}
		else
		{
		
			rrColor32BGRA start0_32_unq = rrColor32BGRA_565_UnQuantize(start0_32);
			rrColor32BGRA start1_32_unq = rrColor32BGRA_565_UnQuantize(start1_32);
				
			// N+N instead of N*N
			for(int w1=0;w1<NUM_WIGGLES;w1++)
			{
				rrColor32BGRA c0_32 = Wiggle(start0_32,w1);
				
				if ( c0_32.dw == start0_32.dw )
					continue;
						
				rrColor32BGRA c0_32_unq = rrColor32BGRA_565_UnQuantize(c0_32);
							
				// if you have alpha, reject 4c mode :
				if ( data.has_any_alpha && c0_32_unq.dw > start1_32_unq.dw )
					continue;

				rrColor32BGRA palette[4];
				DXT1_ComputePalette(c0_32_unq,start1_32_unq,palette,mode);
				
				U32 err;
				U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
								
				if ( err < *pError )
				{
					// remember best wiggle :
					didAnything = true;
					*pError = err;
					best0_32 = c0_32;
					best1_32 = start1_32;
					pBlock->c0 = rrColor565Bits_Pack(c0_32);
					pBlock->c1 = rrColor565Bits_Pack(start1_32);
					pBlock->indices = indices;
					if ( err == 0 ) return;
				}				
			}
			
			for(int w2=0;w2<NUM_WIGGLES;w2++)
			{
				rrColor32BGRA c1_32 = Wiggle(start1_32,w2);
				
				if ( c1_32.dw == start1_32.dw )
					continue;
								
				rrColor32BGRA c1_32_unq = rrColor32BGRA_565_UnQuantize(c1_32);
				
				// if you have alpha, reject 4c mode :
				if ( data.has_any_alpha && start0_32_unq.dw > c1_32_unq.dw )
					continue;

				rrColor32BGRA palette[4];
				DXT1_ComputePalette(start0_32_unq,c1_32_unq,palette,mode);
				
				U32 err;
				U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
								
				if ( err < *pError )
				{
					// remember best wiggle :
					didAnything = true;
					*pError = err;
					best0_32 = start0_32;
					best1_32 = c1_32;
					pBlock->c0 = rrColor565Bits_Pack(start0_32);
					pBlock->c1 = rrColor565Bits_Pack(c1_32);
					pBlock->indices = indices;
					if ( err == 0 ) return;
				}				
			}
		
		}
				
		// if none found, terminate greedy descent
		if ( ! didAnything )
			break;
	}
	
}


#define ANNEAL_MAX_TEMPERATURE	(1024)
#define ANNEAL_TIME				(256)
//#define NUM_ANNEAL_CYCLES		(4)
#define NUM_ANNEAL_CYCLES		(6)
//#define NUM_ANNEAL_CYCLES		(3)

static int GetAnnealingTemperature(int time)
{
	// "unit" goes 0->1 over whole anneal time
	float unit = time * (1.f / ANNEAL_TIME);
	
	// "t" goes to integers over each cycle
	float t = unit * NUM_ANNEAL_CYCLES;
	
	/*
	float periodic =  0.5f - 0.5f * cosf( t * RR_PIf * 2.f );
	
	float temp = ANNEAL_MAX_TEMPERATURE * (1.f - unit) * periodic;
	*/
	
	// sawtooth :
	int it = (int)t;
	t = t - it; // fractional part;
	
	// triangle :
	if ( it > 0 )
	{
		t += t;
		if ( t > 1.f ) t = 2 - t;
	}
	else
	{
		t = 1 - t;
	}
	
	// sawtooth with decreasing amplitude
	float temp = ANNEAL_MAX_TEMPERATURE * (1.f - unit) * t;	
	
	int ret = rr_froundint(temp);
	ret = RR_MAX(ret,1);
	//rrprintfvar(ret);
	return ret;
}

#if 0
// using simple sawtooth now instead of cos
//  fast enough to just call GetAnnealingTemperature
//  don't bother caching it

static int s_anneal_temp[ANNEAL_TIME] = { 0 };
static bool s_anneal_temp_init_done = false;

static void SetupAnnealTemp()
{
	// beware thread safety
	// if multiple threads run in here
	//	let them all redundantly fill s_anneal_temp
	//	they will all fill it the same way
	if ( ! s_anneal_temp_init_done )
	{
		for(int time = 0;time<ANNEAL_TIME;time++)
		{
			s_anneal_temp[time] = GetAnnealingTemperature(time);
		}
		//s_anneal_temp[0] = RR_MAX(s_anneal_temp[0],1);
		RR_ASSERT( s_anneal_temp[0] > 0 );
		// s_anneal_temp_init_done set at end, not start
		s_anneal_temp_init_done = true;
	}
}
#endif

void DXT1_AnnealBlock(const rrCompressDXT1_Startup_Data & data,rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, 
	rrDXT1PaletteMode mode)
{
	SIMPLEPROFILE_SCOPE(BC1_Anneal);

	rrColor32BGRA cur0 = rrColor565Bits_UnPack(pBlock->c0);
	rrColor32BGRA cur1 = rrColor565Bits_UnPack(pBlock->c1);
	U32 curIndices = pBlock->indices;
	U32 curError = *pError;
	
	RR_ASSERT( *pError == DXT1_ComputeSSD_OneBitTransparent(colors,*pBlock,mode) );
	
	//SetupAnnealTemp();
		
	U64 rand_state = rrRand64Simple_SeedFromU64Array((const U64 *)colors.colors,8);
		
	for(int time = 0;time<ANNEAL_TIME;time++)
	{
		//int temperature = s_anneal_temp[time];
		int temperature = GetAnnealingTemperature(time);
	
		//U32 r = rrRandState32(&rand_state);
		U64 r = rrRand64Simple(&rand_state);
	
		int w0 = (int)( r & 7 );
		int w1 = (int)( (r>>8) & 7 );
		// 0-5 are real wiggles, 6&7 are nops (to give you trials where one endpoint is fixed)
		// could use some of the nop codes to do dual-endpoint stretches and contractions
	
		rrColor32BGRA c0 = Wiggle(cur0,w0);
		rrColor32BGRA c1 = Wiggle(cur1,w1);
		
		if ( c0.dw == cur0.dw && c1.dw == cur1.dw )
			continue;
		
		if ( data.has_any_alpha && c0.dw > c1.dw )
			continue;
		
		rrColor32BGRA palette[4];
		DXT1_ComputePalette( rrColor32BGRA_565_UnQuantize(c0), rrColor32BGRA_565_UnQuantize(c1),palette,mode);
				
		U32 err;
		U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
			
		int delta = (int)err - (int)curError;
		if ( delta >= temperature ) continue;
		
		//U32 rand_in_temperature = rrRandStateMod(&rand_state,temperature);
		// use the top 32 bits of "r" that we already made :
		U32 rand_in_temperature = (U32)(((U64)(r>>32) * (U32)temperature)>>32);
		
		if ( delta < 0 || (int)rand_in_temperature > delta )
		{
			// does it improve on the current global min?
			if ( err < *pError )
			{
				*pError = err;
				pBlock->c0 = rrColor565Bits_Pack(c0);
				pBlock->c1 = rrColor565Bits_Pack(c1);
				pBlock->indices = indices;
				if ( err == 0 ) return;
			}

			// note "cur" isn't necessarily the best because annealing
			//  allows positive-cost-delta steps
			curError = err;
			cur0 = c0;
			cur1 = c1;
			curIndices = indices;
		}
	}
	
	// after Anneal you want to further DXT1_GreedyOptimizeBlock
	//	to do greedy steps if any are available
	
}

//=========================================================================

// Quantize straight to packed RGB565; the previous approach where we
// first quantized results to 8-bit temps and from there to 565 rounds
// twice.
static inline rrColor565Bits Vec3fToQuantized565_RN(const rrVec3f & vec)
{
	const F32 scaleRB = 31.0f / 255.0f;
	const F32 scaleG = 63.0f / 255.0f;

	rrColor565Bits ret = {};
	ret.u.b = RR_CLAMP( rr_froundint( scaleRB * vec.x ), 0, 31 );
	ret.u.g = RR_CLAMP( rr_froundint( scaleG  * vec.y ), 0, 63 );
	ret.u.r = RR_CLAMP( rr_froundint( scaleRB * vec.z ), 0, 31 );
	return ret;
}

// Truncating and clampign at max-1 so we can bump values by +1
// without having to worry about overflows later.
static inline void Vec3fToQuantized565_RD_MaxMinus1(U8 dest[3], const rrVec3f & vec)
{
	const F32 scaleRB = 31.0f / 255.0f;
	const F32 scaleG = 63.0f / 255.0f;

	dest[0] = static_cast<U8>(RR_CLAMP( (int) ( scaleRB * vec.x ), 0, 30 ));
	dest[1] = static_cast<U8>(RR_CLAMP( (int) ( scaleG  * vec.y ), 0, 62 ));
	dest[2] = static_cast<U8>(RR_CLAMP( (int) ( scaleRB * vec.z ), 0, 30 ));
}

static rrColor565Bits PackColor(const U8 vals[3])
{
	RR_ASSERT(vals[0] < 32);
	RR_ASSERT(vals[1] < 64);
	RR_ASSERT(vals[2] < 32);

	rrColor565Bits col = {};
	col.u.b = vals[0];
	col.u.g = vals[1];
	col.u.r = vals[2];
	return col;
}

#define USE_NEW_QUANT 0

/*

OptimizeEndPointsFromIndices :

Simon's lsqr to optimize end points from indices

I'm not sure I'm doing it right for the 3 color case, can you just use a 0,0 weight like that ?
	
*/

bool DXT1_OptimizeEndPointsFromIndices_Raw(U32 * pEndPoints, U32 * pIndices, const U32 indices, bool fourc, const rrColorBlock4x4 & colors)
{
	// can just scale up weights to make them ints
	// this is meh for optimization but it is nice to get "det" in an int to be able to check == 0 exactly
	static const int c_fourc_weights[8]  = {3,0,2,1,  0,3,1,2};
	static const int c_threec_weights[8] = {2,0,1,0,  0,2,1,0};
	
	const int * pWeights = fourc ? c_fourc_weights : c_threec_weights;
	
	int AA = 0;
	int AB = 0;
	int BB = 0;
	rrVec3f AX(0,0,0);
	rrVec3f BX(0,0,0);
	
	U32 tindices = indices;
	for(int i=0;i<16;i++)
	{
		int index = tindices&3;
		tindices >>= 2;
		
		const int A = pWeights[index];
		const int B = pWeights[index+4];
		
		AA += A*A;
		BB += B*B;
		AB += A*B;
		
		rrVec3f X = ColorToVec3f( colors.colors[i] );
		AX += float(A)*X;
		BX += float(B)*X;
	}
	
	int det = AA*BB - AB*AB;
	if ( det == 0 )
	{
		return false;
	}
	
	// have to multiply invDet by the normalization factor that was used on weights :
	//double invDet = 1.0 / det;
	float invDet = pWeights[0] / (float) det;
	
	rrVec3f vA = float(  BB * invDet) * AX + float(- AB * invDet ) * BX;
	rrVec3f vB = float(- AB * invDet) * AX + float(  AA * invDet ) * BX;
	
#if USE_NEW_QUANT == 2
	U8 rdA[3], rdB[3];
	U8 bestA[3] = {}, bestB[3] = {};

	// rounded-down A, B, clamped to actual max minus 1 so we can safely add 1
	Vec3fToQuantized565_RD_MaxMinus1(rdA, vA);
	Vec3fToQuantized565_RD_MaxMinus1(rdB, vB);

	// For each of the 3 color channels
	for(int ch=0;ch<3;ch++)
	{
		int mult = (ch == 1) ? 65 : 132; // .4 fixed-point multipliers for the bit expand
		float best = 0.0f;

		// Try all 4 rounding combinations
		for (int i=0;i<4;i++)
		{
			// Actual endpoint values
			int x0i = rdA[ch] + (i&1);
			int x1i = rdB[ch] + (i>>1);

			// Expanded with bit-replicate, and converted to float
			float x0 = (float)((x0i * mult) >> 4);
			float x1 = (float)((x1i * mult) >> 4);

			// Residual term is
			//   x^T (A^T A) x - 2 x^T (A^T b) + b^T b
			// can drop the b^T b portion since it's a constant, doesn't change the min
			float res = AA * (x0*x0) + BB * (x1*x1) + 2.0f * (AB * x0*x1 - x0*AX[ch] - x1*BX[ch]);
			if (i == 0 || res < best)
			{
				best = res;
				bestA[ch] = static_cast<U8>(x0i);
				bestB[ch] = static_cast<U8>(x1i);
			}
		}
	}

	rrColor565Bits qA = PackColor(bestA);
	rrColor565Bits qB = PackColor(bestB);
#elif USE_NEW_QUANT == 1
	rrColor565Bits qA = Vec3fToQuantized565_RN(vA);
	rrColor565Bits qB = Vec3fToQuantized565_RN(vB);
#else // USE_NEW_QUANT
	rrColor32BGRA cA = Vec3fToColorClamp(vA);
	rrColor565Bits qA = Quantize(cA);
	
	#if 0 //$$$$
	// if end A went down when it was quantized,
	//	make end B go up
	// this tries to keep the mid point the same
	// -> yes this does seem to help a tiny bit
	//   the affect is very small though
	// l2 bc1difficult :
	// rmse_total = 119.278
	// rmse_total = 119.269
	// rmse_total = 307.686	
	rrColor32BGRA cAq = rrColor565Bits_UnQuantize(qA);
	
	// cA or vA ?
	// xyz -> bgr
	vB.x += cA.u.b - cAq.u.b;
	vB.y += cA.u.g - cAq.u.g;
	vB.z += cA.u.r - cAq.u.r;
	//vB.x += vA.x - cAq.u.b;
	//vB.y += vA.y - cAq.u.g;
	//vB.z += vA.z - cAq.u.r;
	#endif

	rrColor32BGRA cB = Vec3fToColorClamp(vB);
	rrColor565Bits qB = Quantize(cB);
	
	#if 0
	// then apply the B drift back to A :
	// tiny help
	//rmse_total = 307.632
	//rmse_total = 307.668
	rrColor32BGRA cBq = rrColor565Bits_UnQuantize(qB);
	vA.x += cB.u.b - cBq.u.b;
	vA.y += cB.u.g - cBq.u.g;
	vA.z += cB.u.r - cBq.u.r;
	cA = Vec3fToColorClamp(vA);
	qA = Quantize(cA);
	#endif
	
	// switch endpoints to maintain four-color state :
	//
	// NOTE : switching endpoints makes them no longer be the lsqr solve for these indices!
	//	they are now the lsqr solve for flip(indices)
	//	re-indexing fixes this but if you don't reindex, it may be much worse
	
	// note in degenerate cases if qA == qB , that's always 3-index
	//	no flip can save them and we're screwed
	
	//Make4ColorOrder(qA,qB);
#endif // USE_NEW_QUANT
	
	#if 1
	
	if ( qA.w < qB.w )
	{
		RR_NAMESPACE::swap(qA.w,qB.w);
		
		if ( pIndices )
			*pIndices = indices ^ 0x55555555;
	}
	else
	{
		if ( pIndices )
			*pIndices = indices;
	}	
	
	if ( ! fourc )
		RR_NAMESPACE::swap(qA,qB);
	
	rrDXT1EndPoints endpoints;
	endpoints.u.c0 = qA;
	endpoints.u.c1 = qB;
	*pEndPoints = endpoints.dw;
	
	#else
	
	// @@ TEMP TEST
	//	for reference
	//	
	// rmse_total = 119.260
	
	{
		// make the alternative quantization by biasing by the quantization drift :
		rrColor32BGRA cAq = rrColor565Bits_UnQuantize(qA);
		rrColor32BGRA cBq = rrColor565Bits_UnQuantize(qB);
		
		vA.x += vA.x - cAq.u.b;
		vA.y += vA.y - cAq.u.g;
		vA.z += vA.z - cAq.u.r;
		
		vB.x += vB.x - cBq.u.b;
		vB.y += vB.y - cBq.u.g;
		vB.z += vB.z - cBq.u.r;
		
		rrColor565Bits qA2 = Quantize( Vec3fToColorClamp(vA) );
		rrColor565Bits qB2 = Quantize( Vec3fToColorClamp(vB) );
		
		/*
		// take the lower endpoint on each axis
		//	make it choose the lower quantization
		//	higher endpoint takes the higher quantization
		//	so they both prefer to dilate
		// -> this doesn't work
		rrColor565Bits udA,udB;		
		if ( vA.x < vB.x ) { udA.u.b = RR_MIN(qA.u.b,qA2.u.b); udB.u.b = RR_MAX(qB.u.b,qB2.u.b); }
		else { udA.u.b = RR_MAX(qA.u.b,qA2.u.b); udB.u.b = RR_MIN(qB.u.b,qB2.u.b); }
		if ( vA.y < vB.y ) { udA.u.g = RR_MIN(qA.u.g,qA2.u.g); udB.u.g = RR_MAX(qB.u.g,qB2.u.g); }
		else { udA.u.g = RR_MAX(qA.u.g,qA2.u.g); udB.u.g = RR_MIN(qB.u.g,qB2.u.g); }		
		if ( vA.z < vB.z ) { udA.u.r = RR_MIN(qA.u.r,qA2.u.r); udB.u.r = RR_MAX(qB.u.r,qB2.u.r); }
		else { udA.u.r = RR_MAX(qA.u.r,qA2.u.r); udB.u.r = RR_MIN(qB.u.r,qB2.u.r); }
		*/
		
		Make4ColorOrder(qA,qB);
		Make4ColorOrder(qA2,qB2);
		//Make4ColorOrder(udA,udB);
		
		rrDXT1PaletteMode mode = rrDXT1PaletteMode_NoAlpha;
		// re-index for new endpoints :
		rrColor32BGRA palette[4];
		
		U32 best_err;
		U32 cur_err;
		
		// make best_err = the baseline we would have returned
		DXT1_ComputePalette(qA,qB,palette,mode);
		DXT1_FindIndices(colors,palette,mode,&best_err);
	
		rrDXT1EndPoints endpoints;
		endpoints.u.c0 = qA;
		endpoints.u.c1 = qB;
		Make4ColorOrder(endpoints.u.c0,endpoints.u.c1);
		*pEndPoints = endpoints.dw;

		#if 1		
		// see if the alternate quantization is better	
		DXT1_ComputePalette(qA2,qB2,palette,mode);
		DXT1_FindIndices(colors,palette,mode,&cur_err);
		
		if ( cur_err < best_err )
		{
			best_err = cur_err;
			endpoints.u.c0 = qA2;
			endpoints.u.c1 = qB2;
			Make4ColorOrder(endpoints.u.c0,endpoints.u.c1);
			*pEndPoints = endpoints.dw;
		}
		#endif
		
		#if 0
		DXT1_ComputePalette(udA,udA,palette,mode);
		DXT1_FindIndices(colors,palette,mode,&cur_err);
		
		if ( cur_err < best_err )
		{
			best_err = cur_err;
			endpoints.u.c0 = udA;
			endpoints.u.c1 = udA;
			Make4ColorOrder(endpoints.u.c0,endpoints.u.c1);
			*pEndPoints = endpoints.dw;
		}
		#endif
		
		#if 0
		DXT1_ComputePalette(qA,qB2,palette,mode);
		DXT1_FindIndices(colors,palette,mode,&cur_err);
		
		if ( cur_err < best_err )
		{
			best_err = cur_err;
			endpoints.u.c0 = qA;
			endpoints.u.c1 = qB2;
			Make4ColorOrder(endpoints.u.c0,endpoints.u.c1);
			*pEndPoints = endpoints.dw;
		}
		
		DXT1_ComputePalette(qA2,qB,palette,mode);
		DXT1_FindIndices(colors,palette,mode,&cur_err);
		
		if ( cur_err < best_err )
		{
			best_err = cur_err;
			endpoints.u.c0 = qA2;
			endpoints.u.c1 = qB;
			Make4ColorOrder(endpoints.u.c0,endpoints.u.c1);
			*pEndPoints = endpoints.dw;
		}
		#endif
	}
	#endif
	
	return true;
}

#if 1
/**

DXT1_OptimizeEndPointsFromIndices_FourC_Error :
experiment 05-25-2019
do lsqr to find endpoints
then just compute error directly
saves converting back to color & so on

this differs from the true error in the 565 quantization of endpoints and the
  interpolants not being floating point
	-> fixed now, endpoints ARE 565 quantized, that's trivial
	-> it is still different in that it assumes real number 1/3 perfect interpolation
		not the funny rounding per the spec

**/	
F64 DXT1_OptimizeEndPointsFromIndices_FourC_Error(const U32 indices, const rrColorBlock4x4 & colors)
{
	// can just scale up weights to make them ints
	// this is meh for optimization but it is nice to get "det" in an int to be able to check == 0 exactly
	//static const int c_fourc_weights[8]  = {3,0,2,1,  0,3,1,2};
	static const F32 c_fourc_weights[8]  = {1.f,0,2/3.f,1/3.f,  0,1.f,1/3.f,2/3.f};
	
	F64 AA = 0;
	F64 AB = 0;
	F64 BB = 0;
	rrVec3f AX(0,0,0);
	rrVec3f BX(0,0,0);
	F64 XX = 0;
	
	U32 tindices = indices;
	for(int i=0;i<16;i++)
	{
		int index = tindices&3;
		tindices >>= 2;
		
		const F32 A = c_fourc_weights[index];
		const F32 B = c_fourc_weights[index+4];
		
		AA += A*A;
		BB += B*B;
		AB += A*B;
		
		rrVec3f X = ColorToVec3f( colors.colors[i] );
		XX += X * X;
		AX += A*X;
		BX += B*X;
	}
	
	F64 det = AA*BB - AB*AB;
	if ( det == 0.0 ) // @@ eps ?
	{
		return RR_F32_MAX;
	}
	
	// have to multiply invDet by the normalization factor that was used on weights :
	float invDet = 1.f / (float) det;
	
	rrVec3f vA = float(  BB * invDet) * AX + float(- AB * invDet ) * BX;
	rrVec3f vB = float(- AB * invDet) * AX + float(  AA * invDet ) * BX;
	
	// !! quantize endpoints here !!
	// -> yes does help
	if ( 1 )
	{		
		rrColor565Bits qA = Quantize( Vec3fToColorClamp(vA) );
		rrColor565Bits qB = Quantize( Vec3fToColorClamp(vB) );
		
		rrColor32BGRA cA = rrColor565Bits_UnQuantize(qA);
		rrColor32BGRA cB = rrColor565Bits_UnQuantize(qB);
		
		vA = ColorToVec3f(cA);
		vB = ColorToVec3f(cB);
	}
	
	// just go back to the matrix we were trying to minimize err from in the lsqr
	//	and get the residual :
	//	(this is an approximation because it treats the interpolants as floating point 1/3)
	
	F64 err = AA * (vA * vA) + 2 * AB * (vA * vB) + BB * (vB * vB) + XX - 2 * (AX * vA) - 2 * (BX * vB);
	
	// @@ could return the endpoints chosen
	
	#if 0
	// verify err calculation
	F64 check_err = 0;
	tindices = indices;
	for(int i=0;i<16;i++)
	{
		int index = tindices&3;
		tindices >>= 2;
		
		const F32 A = c_fourc_weights[index];
		const F32 B = c_fourc_weights[index+4];
		
		rrVec3f X = ColorToVec3f( colors.colors[i] );
		rrVec3f P = A * vA + B * vB;
		rrVec3f PX = P - X;
		check_err += PX * PX;
	}
	
	F64 err_err = RR_ABS( err - check_err );
	RR_ASSERT( err_err < 1.0 );
	#endif
	
	return err;
}

F64 DXT1_OptimizeEndPointsFromIndices_FourC_Error(
		const ColorAccum & group_0,const ColorAccum & group_13,const ColorAccum & group_23,const ColorAccum & group_1,
		U32 * pEndPoints)
{	
	F64 AA = 0;
	F64 AB = 0;
	F64 BB = 0;
	rrVec3f AX(0,0,0);
	rrVec3f BX(0,0,0);
	F64 XX = 0;
	
	{
		const ColorAccum & group = group_0;
		const F32 A = 1.f;
		const F32 B = 0.f;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
	
	{
		const ColorAccum & group = group_1;
		const F32 A = 0.f;
		const F32 B = 1.f;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
	
	{
		const ColorAccum & group = group_13;
		const F32 A = 2.f/3;
		const F32 B = 1.f/3;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
	
	{
		const ColorAccum & group = group_23;
		const F32 A = 1.f/3;
		const F32 B = 2.f/3;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
		
	F32 det = (F32)( AA*BB - AB*AB );
	if ( det == 0.f ) // @@ eps ?
	{
		return RR_F32_MAX;
	}
	
	// have to multiply invDet by the normalization factor that was used on weights :
	float invDet = 1.f / det;
	
	rrVec3f vA = float(  BB * invDet) * AX + float(- AB * invDet ) * BX;
	rrVec3f vB = float(- AB * invDet) * AX + float(  AA * invDet ) * BX;
	
	// quantize the endpoints :
	rrColor565Bits qA = Quantize( Vec3fToColorClamp(vA) );
	rrColor565Bits qB = Quantize( Vec3fToColorClamp(vB) );
	
	// save them to record this solution :
	rrDXT1EndPoints ep;
	ep.u.c0 = qA;
	ep.u.c1 = qB;
	Make4ColorOrder(ep.u.c0,ep.u.c1); // do to ep, but not to AB
	*pEndPoints = ep.dw;
	
	// use the quantized endpoints for error calc
	rrColor32BGRA cA = rrColor565Bits_UnQuantize(qA);
	rrColor32BGRA cB = rrColor565Bits_UnQuantize(qB);
	
	vA = ColorToVec3f(cA);
	vB = ColorToVec3f(cB);
	
	// just go back to the matrix we were trying to minimize err from in the lsqr
	//	and get the residual :
	//	(this is an approximation because it treats the interpolants as floating point 1/3)
	
	F64 err = AA * (vA * vA) + BB * (vB * vB) + XX + 2.f * ( AB * (vA * vB) - (AX * vA) - (BX * vB) );
		
	return err;
}	

F64 DXT1_OptimizeEndPointsFromIndices_3C_Error(
		const ColorAccum & group_0,const ColorAccum & group_12,const ColorAccum & group_1,
		U32 * pEndPoints)
{
	F64 AA = 0;
	F64 AB = 0;
	F64 BB = 0;
	rrVec3f AX(0,0,0);
	rrVec3f BX(0,0,0);
	F64 XX = 0;
	
	{
		const ColorAccum & group = group_0;
		const F32 A = 1.f;
		const F32 B = 0.f;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
	
	{
		const ColorAccum & group = group_1;
		const F32 A = 0.f;
		const F32 B = 1.f;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
	
	{
		const ColorAccum & group = group_12;
		const F32 A = 0.5f;
		const F32 B = 0.5f;
	
		AA += (A*A) * group.count;
		BB += (B*B) * group.count;
		AB += (A*B) * group.count;
			
		XX += group.sqrcolor_sum;
		AX += A*group.color_sum;
		BX += B*group.color_sum;
	}
			
	F32 det = (F32)( AA*BB - AB*AB );
	if ( det == 0.f ) // @@ eps ?
	{
		return RR_F32_MAX;
	}
	
	// have to multiply invDet by the normalization factor that was used on weights :
	float invDet = 1.f / det;
	
	rrVec3f vA = float(  BB * invDet) * AX + float(- AB * invDet ) * BX;
	rrVec3f vB = float(- AB * invDet) * AX + float(  AA * invDet ) * BX;
	
	// quantize the endpoints :
	rrColor565Bits qA = Quantize( Vec3fToColorClamp(vA) );
	rrColor565Bits qB = Quantize( Vec3fToColorClamp(vB) );
	
	// save them to record this solution :
	rrDXT1EndPoints ep;
	ep.u.c0 = qA;
	ep.u.c1 = qB;
	// call in opposite order to make 3color :
	Make4ColorOrder(ep.u.c1,ep.u.c0); // do to ep, but not to AB
	*pEndPoints = ep.dw;
	
	// use the quantized endpoints for error calc
	rrColor32BGRA cA = rrColor565Bits_UnQuantize(qA);
	rrColor32BGRA cB = rrColor565Bits_UnQuantize(qB);
	
	vA = ColorToVec3f(cA);
	vB = ColorToVec3f(cB);
	
	// just go back to the matrix we were trying to minimize err from in the lsqr
	//	and get the residual :
	//	(this is an approximation because it treats the interpolants as floating point 1/3)
	
	F64 err = AA * (vA * vA) + BB * (vB * vB) + XX + 2.f * ( AB * (vA * vB) - (AX * vA) - (BX * vB) );
		
	return err;
}
#endif
	
bool DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode) //, rrDXTCOptions options)
{
	// keep previous fourc state :
	bool fourc = DXT1_Is4Color(*pBlock,mode);

	U32 endpoints = rrDXT1Block_EndPoints_AsU32(*pBlock);
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,NULL,pBlock->indices,fourc,colors) )
	{
		return false;
	}

	// if endpoints didn't change, bail :
	if ( endpoints == rrDXT1Block_EndPoints_AsU32(*pBlock) )
	{
		return false;
	}
	
	// re-index for new endpoints :
	rrDXT1EndPoints ep; ep.dw = endpoints;
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(ep.u.c0,ep.u.c1,palette,mode);
	
	U32 err;
	U32 indices = DXT1_FindIndices(colors,palette,mode,&err);
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		rrDXT1Block_EndPoints_AsU32(*pBlock) = endpoints;
		pBlock->indices = indices;
		return true;
	}
	else
	{
		return false;
	}
}

bool DXT1_OptimizeEndPointsFromIndices_Fourc_Reindex(rrDXT1Block * pBlock,U32 * pError, U32 indices, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode) //, rrDXTCOptions options)
{
	// does not read *pBlock
	bool fourc = true;
	U32 endpoints = 0;
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,NULL,indices,fourc,colors) )
	{
		return false;
	}

	// re-index for new endpoints :
	rrDXT1EndPoints ep; ep.dw = endpoints;
	// degenerate ; equal endpoints is 3 color, bail
	if ( ep.u.c0.w == ep.u.c1.w )
		return false;
	
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(ep.u.c0,ep.u.c1,palette,mode);
	
	U32 err;
	indices = DXT1_FindIndices(colors,palette,mode,&err);
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		rrDXT1Block_EndPoints_AsU32(*pBlock) = endpoints;
		pBlock->indices = indices;
		return true;
	}
	else
	{
		return false;
	}
}

// if allow_flip is true, then flip(indices) will be used
//	if the end points need to be flipped to maintain four color order
bool DXT1_OptimizeEndPointsFromIndices_Fourc_NoReindex(rrDXT1Block * pBlock,U32 * pError, U32 old_indices, const rrColorBlock4x4 & colors, bool allow_flip) //, rrDXTCOptions options)
{
	// does not read *pBlock
	bool fourc = true;
	U32 endpoints = 0;
	U32 new_indices = 0;
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,&new_indices,old_indices,fourc,colors) )
	{
		return false;
	}
	
	if ( new_indices != old_indices )
	{
		if ( ! allow_flip )
		{
			// no point even checking error with old indices, they're F'ed
			return false;
		}
	}
		
	// evaluate error with new endpoints :
	rrDXT1Block block;
	block.endpoints = endpoints;
	block.indices = new_indices;
	
	// degenerate ; equal endpoints is 3 color, bail
	if ( block.c0.w == block.c1.w )
		return false;
	
	//RR_ASSERT( DXT1_Is4Color(block,mode) );
	
	U32 err = DXT1_ComputeSSD_RGBA(colors,block,rrDXT1PaletteMode_FourColor); // use NoAlpha, we're guaranteed to be in 4-color mode here
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		*pBlock = block;
		return true;
	}
	else
	{	
		return false;
	}
}

bool DXT1_OptimizeEndPointsFromIndices_Inherit_NoReindex(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors, rrDXT1PaletteMode mode)
{
	// keep previous fourc state :
	bool fourc = DXT1_Is4Color(*pBlock,mode);

	U32 oldep = pBlock->endpoints;
	U32 endpoints = oldep;
	
	if ( ! DXT1_OptimizeEndPointsFromIndices_Raw(&endpoints,NULL,pBlock->indices,fourc,colors) )
	{
		return false;
	}

	// if endpoints didn't change, bail :
	if ( endpoints == oldep )
	{
		return false;
	}
	
	// evaluate error with new endpoints :
	rrDXT1Block block;
	block.endpoints = endpoints;
	block.indices = pBlock->indices;
	
	// DXT1_OptimizeEndPointsFromIndices_Raw will try to preserve the fourc state
	//	except when it can't because endpoints were degenerate
	RR_ASSERT( DXT1_Is4Color(block,mode) == fourc || ( fourc && block.c0.w == block.c1.w ) );
	
	U32 err = DXT1_ComputeSSD_RGBA(colors,block,mode);
	if ( err < *pError )
	{
		// it got better, take it
		*pError = err;
		*pBlock = block;
		return true;
	}
	else
	{	
		return false;
	}
}

void DXT1_OptimizeEndPointsFromIndicesIterative(rrDXT1Block * pBlock,U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode) //, rrDXTCOptions options)
{
	SIMPLEPROFILE_SCOPE(BC1_EndpointsFromIndsIter);
	
	for(;;)
	{
		U32 oldIndices = pBlock->indices;
		if ( ! DXT1_OptimizeEndPointsFromIndices_Inherit_Reindex(pBlock,pError,colors,mode) )
			break;
		if ( oldIndices == pBlock->indices )
			break;
		// else indices changed so do it again
		
		// this almost never actually repeats
		//	it helps quality a tiny bit and doesn't hurt speed much
		
		// @@ while iterating does help a tiny bit, is it worth it speed-wise ?
	}
}

//=============================================================================================

// main external entry points :
			
void rrCompressDXT1Block(rrDXT1Block * pBlock,const rrColorBlock4x4 & colors, rrDXTCLevel level, rrDXTCOptions options, bool isBC23ColorBlock)
{
//	SIMPLEPROFILE_SCOPE(rrCompressDXT1Block);
	
	rrDXT1PaletteMode mode = (options & rrDXTCOptions_BC1_OneBitAlpha) ? rrDXT1PaletteMode_Alpha : rrDXT1PaletteMode_NoAlpha;
	if ( isBC23ColorBlock ) // BC2/3 (and also DXT3/5) color blocks don't support the 3-color mode and ignore endpoint ordering
		mode = rrDXT1PaletteMode_FourColor;
	
	// rrSurfaceDXTC_CompressBC1 does the canonicalize :
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(colors,mode) );
	
	U32 err = RR_DXTC_INIT_ERROR;
		
	if ( level >= rrDXTCLevel_VerySlow )
		rrCompressDXT1_3( pBlock, &err, colors, options, mode, level );
	else if ( level == rrDXTCLevel_Slow )
		rrCompressDXT1_2( pBlock, &err, colors, options, mode );
	else if ( level == rrDXTCLevel_Fast )
		rrCompressDXT1_1( pBlock, &err, colors, options, mode );
	else // VeryFast
		rrCompressDXT1_0( pBlock, &err, colors, options, mode );

	// In BC2/3, both endpoint orderings result in four-color mode. So we have some freedom
	// here and want to pick a canonical choice.
	if ( mode == rrDXT1PaletteMode_FourColor )
	{
		rrDXT1Block_BC3_Canonicalize(pBlock);
	}
	else if ( mode == rrDXT1PaletteMode_Alpha )
	{
		RR_ASSERT( DXT1_OneBitTransparent_Same(colors,pBlock->endpoints,pBlock->indices) );
	}
}

//=============================================================================================

RR_NAMESPACE_END
