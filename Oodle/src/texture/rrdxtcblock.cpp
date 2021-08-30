// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4

#include "rrdxtcblock.h"
#include "templates/rrstl.h"
#include "vec128.inl"
#include "rrdxt1vqhelp.inl" // the faster SAD/SSD are here

RR_NAMESPACE_START

// DXTC formats :
// http://msdn.microsoft.com/en-us/library/bb694531(VS.85).aspx

/**

BC1 is : 

4color when end0 > end1 (end0 == end1 is 3color)

4color : end0, end1, 1/3, 2/3
3color : end0, end1, 1/2, black+transparent

BC1 4color all 1s = 0x55555555  (end1)
BC1 4color all 2s = 0xAAAAAAAA  (1/3)

BC1 4color indices flip for endpoint exchange is ^= 0x55555555  (all 1s)
				
**/

//-----------------------------------------------------

void Make4ColorOrder(rrColor565Bits & c0,rrColor565Bits & c1)
{
	if ( c0.w < c1.w ) RR_NAMESPACE::swap(c0.w,c1.w);
}

// this exactly matches the (fm*2 + to)/3
static inline int Lerp13_16bit(int fm,int to)
{
	int t = fm * (2 * 0xAAAB) + to * 0xAAAB;

	return t>>17;
}

void DXT1_ComputePalette(U32 endpoints, rrColor32BGRA palette[4], rrDXT1PaletteMode mode)
{
	rrDXT1EndPoints ep;
	ep.dw = endpoints;
	DXT1_ComputePalette(ep.u.c0,ep.u.c1,palette,mode);
}

void DXT1_ComputePalette(rrColor565Bits c0,rrColor565Bits c1, rrColor32BGRA palette[4], rrDXT1PaletteMode mode)
{
	rrColor32BGRA c0b = rrColor565Bits_UnQuantize(c0);
	rrColor32BGRA c1b = rrColor565Bits_UnQuantize(c1);

	palette[0] = c0b;
	palette[1] = c1b;

	if ( mode == rrDXT1PaletteMode_FourColor || c0.w > c1.w )
	{
		rrColor32BGRA c2;
		rrColor32BGRA c3;
				
		c2.u.r = (U8) Lerp13_16bit(c0b.u.r,c1b.u.r);
		c2.u.g = (U8) Lerp13_16bit(c0b.u.g,c1b.u.g);
		c2.u.b = (U8) Lerp13_16bit(c0b.u.b,c1b.u.b);
		c2.u.a = 0xFF;
		
		c3.u.r = (U8) Lerp13_16bit(c1b.u.r,c0b.u.r);
		c3.u.g = (U8) Lerp13_16bit(c1b.u.g,c0b.u.g);
		c3.u.b = (U8) Lerp13_16bit(c1b.u.b,c0b.u.b);
		c3.u.a = 0xFF;
		
		palette[2] = c2;
		palette[3] = c3;
	}
	else
	{
		rrColor32BGRA c2;

		c2.u.r = (c0b.u.r + c1b.u.r)>>1;
		c2.u.g = (c0b.u.g + c1b.u.g)>>1;
		c2.u.b = (c0b.u.b + c1b.u.b)>>1;
		c2.u.a = 0xFF;

		palette[2] = c2;
		
		if ( mode == rrDXT1PaletteMode_Alpha )
		{
			palette[3].dw = 0;
		}
		else
		{
			// must stuff A = 255 so we can do 4-channel RGBA diffs
			palette[3].u.r = 0;
			palette[3].u.g = 0;
			palette[3].u.b = 0;
			palette[3].u.a = 0xFF;
		}
	}
}

void DXT1_ComputePalette(rrColor32BGRA c0b,rrColor32BGRA c1b,rrColor32BGRA palette[4], rrDXT1PaletteMode mode)
{
	//RR_ASSERT( rrColor32BGRA_EqualsRGBA(c0b, rrColor565Bits_UnQuantize(c0_16b)) );
	//RR_ASSERT( rrColor32BGRA_EqualsRGBA(c1b, rrColor565Bits_UnQuantize(c1_16b)) );
	
	palette[0] = c0b;
	palette[1] = c1b;
	
	// have to pack to 565 to get the word compare for 3 color mode BLECK :
	// no you don't! 32-bit unquantized dword compare order is the same as the quantized 16-bit
	//	RR_ASSERT( (c0_16b.w > c1_16b.w) == (c0b.dw > c1b.dw) );

	if ( mode == rrDXT1PaletteMode_FourColor || c0b.dw > c1b.dw )
	{
		rrColor32BGRA c2;
		rrColor32BGRA c3;
				
		c2.u.r = (U8) Lerp13_16bit(c0b.u.r,c1b.u.r);
		c2.u.g = (U8) Lerp13_16bit(c0b.u.g,c1b.u.g);
		c2.u.b = (U8) Lerp13_16bit(c0b.u.b,c1b.u.b);
		c2.u.a = 0xFF;
		
		c3.u.r = (U8) Lerp13_16bit(c1b.u.r,c0b.u.r);
		c3.u.g = (U8) Lerp13_16bit(c1b.u.g,c0b.u.g);
		c3.u.b = (U8) Lerp13_16bit(c1b.u.b,c0b.u.b);
		c3.u.a = 0xFF;
		
		palette[2] = c2;
		palette[3] = c3;
	}
	else
	{
		rrColor32BGRA c2;

		c2.u.r = (c0b.u.r + c1b.u.r)>>1;
		c2.u.g = (c0b.u.g + c1b.u.g)>>1;
		c2.u.b = (c0b.u.b + c1b.u.b)>>1;
		c2.u.a = 0xFF;

		palette[2] = c2;
		
		if ( mode == rrDXT1PaletteMode_Alpha )
		{
			palette[3].dw = 0;
		}
		else
		{
			// must stuff A = 255 so we can do 4-channel RGBA diffs
			palette[3].u.r = 0;
			palette[3].u.g = 0;
			palette[3].u.b = 0;
			palette[3].u.a = 0xFF;
		}
	}
}

void DXT1_ComputePalette_NV5x(rrColor565Bits c0,rrColor565Bits c1, rrColor32BGRA palette[4], rrDXT1PaletteMode mode)
{
	palette[0].u.b = (U8) ( (3 * c0.u.b * 22) / 8 );
	palette[0].u.g = (U8) ( (c0.u.g << 2) | (c0.u.g >> 4) );
	palette[0].u.r = (U8) ( (3 * c0.u.r * 22) / 8 );
	palette[0].u.a = 0xFF;

	palette[1].u.r = (U8) ( (3 * c1.u.r * 22) / 8 );
	palette[1].u.g = (U8) ( (c1.u.g << 2) | (c1.u.g >> 4) );
	palette[1].u.b = (U8) ( (3 * c1.u.b * 22) / 8 );
	palette[1].u.a = 0xFF;

	int gdiff = palette[1].u.g - palette[0].u.g;

	if ( mode == rrDXT1PaletteMode_FourColor || c0.w > c1.w )
	{
		palette[2].u.r = (U8) ( ((2 * c0.u.r + c1.u.r) * 22) / 8 );
		palette[2].u.g = (U8) ( (256 * palette[0].u.g + gdiff/4 + 128 + gdiff * 80) / 256 );
		palette[2].u.b = (U8) ( ((2 * c0.u.b + c1.u.b) * 22) / 8 );
		palette[2].u.a = 0xFF;

		palette[3].u.r = (U8) ( ((2 * c1.u.r + c0.u.r) * 22) / 8 );
		palette[3].u.g = (U8) ( (256 * palette[1].u.g - gdiff/4 + 128 - gdiff * 80) / 256 );
		palette[3].u.b = (U8) ( ((2 * c1.u.b + c0.u.b) * 22) / 8 );
		palette[3].u.a = 0xFF;
	}
	else 
	{
		palette[2].u.r = (U8) ( ((c0.u.r + c1.u.r) * 33) / 8 );
		palette[2].u.g = (U8) ( (256 * palette[0].u.g + gdiff/4 + 128 + gdiff * 128) / 256 );
		palette[2].u.b = (U8) ( ((c0.u.b + c1.u.b) * 33) / 8 );
		palette[2].u.a = 0xFF;

		if ( mode == rrDXT1PaletteMode_Alpha )
		{
			palette[3].dw = 0;
		}
		else
		{
			palette[3].u.r = 0;
			palette[3].u.g = 0;
			palette[3].u.b = 0;
			palette[3].u.a = 0xFF;
		}
	}
}

void DXT5_ComputePalette(U32 alpha_0,U32 alpha_1, U8 palette[8])
{
	palette[0] = (U8) alpha_0;
	palette[1] = (U8) alpha_1;
	if( alpha_0 > alpha_1 )
	{
		// 6 interpolated alpha values.
		palette[2] = (U8) ((6*alpha_0 + 1*alpha_1) / 7);
		palette[3] = (U8) ((5*alpha_0 + 2*alpha_1) / 7);
		palette[4] = (U8) ((4*alpha_0 + 3*alpha_1) / 7);
		palette[5] = (U8) ((3*alpha_0 + 4*alpha_1) / 7);
		palette[6] = (U8) ((2*alpha_0 + 5*alpha_1) / 7);
		palette[7] = (U8) ((1*alpha_0 + 6*alpha_1) / 7);
	}
	else
	{
		// 4 interpolated alpha values.
		palette[2] = (U8) ((4*alpha_0 + 1*alpha_1) / 5);
		palette[3] = (U8) ((3*alpha_0 + 2*alpha_1) / 5);
		palette[4] = (U8) ((2*alpha_0 + 3*alpha_1) / 5);
		palette[5] = (U8) ((1*alpha_0 + 4*alpha_1) / 5);
		palette[6] = 0;                        
		palette[7] = 255;                      
	}
}

void DXT1_Decompress(rrColorBlock4x4 * pTo,const rrDXT1Block & from, rrDXT1PaletteMode mode)
{
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(from.c0,from.c1,palette,mode);
	
	U32 indices = from.indices;
	for(int i=0;i<16;i++)
	{
		pTo->colors[i] = palette[ indices&3 ];
		indices >>= 2;
	}
}

void DXT3_CompressAlpha(rrDXT3AlphaBlock * pTo,const rrSingleChannelBlock4x4 & from)
{
	const U8 * inPtr = from.values;
	for(int y=0;y<4;y++)
	{
		U32 row = 0;
		
		for(int x=0;x<4;x++) // could be UNROLL_4
		{
			U32 val = *inPtr++;
			
			// correct quantization for restoration by bit replication :
			U32 out = rrMul8Bit(val,15);
			RR_ASSERT( out == (out&0xF) );
			
			//row |= (out)<<(x*4); // unroll to make this a constant shift
			row >>= 4;
			row |= out<<12;
		}
	
		RR_ASSERT( row == (row&0xFFFF) );

		pTo->alphas[y] = (U16) row;
	}
}

void DXT3_DecompressAlpha(rrSingleChannelBlock4x4 * pTo,const rrDXT3AlphaBlock & from)
{
	U8 * outPtr = pTo->values;
	for(int y=0;y<4;y++)
	{
		U32 row = from.alphas[y];
		
		for(int x=0;x<4;x++) // could be UNROLL_4
		{
			U32 val = (row&0xF);
			
			// bit replicate : 0->0, 0xF -> 0xFF
			U32 out = (val<<4) | val;
			
			*outPtr++ = (U8) out;
			
			row >>= 4;
		}
	}
}

void DXT5_DecompressAlpha(rrSingleChannelBlock4x4 * pTo,const rrDXT5AlphaBlock & from)
{
	U8 palette[8];
	DXT5_ComputePalette(from.a0,from.a1,palette);
	
	U8 * outPtr = pTo->values;
	for(int y=0;y<2;y++)
	{
		const U8 * three = from.indices[y];
		U32 word = three[0] + (three[1]<<8) + (three[2]<<16); // little endian 24 bit word
		
		for(int x=0;x<8;x++) // could be UNROLL_8
		{
			U32 index = (word&0x7);
						
			*outPtr++ = palette[index];
			
			word >>= 3;
		}
	}
}

// 1bit transparent mask :
//	turn a bit on, every 2nd bit, if the pel is transparent
// so all opaque -> mask = 0
// all transparent -> mask = 55555
U32 DXT1_OneBitTransparent_Mask(const rrColorBlock4x4 & block)
{
	// could be an SSE2 movmsk ?
	U32 out = 0;
	for LOOP(i,16)
	{
		if ( block.colors[i].dw == 0 )
			out |= 1U<<(i*2);
	}
	return out;
}

bool DXT1_OneBitTransparent_Mask_Same(U32 mask, U32 endpoints, U32 indices)
{
	if ( DXT1_Is4Color(endpoints,rrDXT1PaletteMode_Alpha) )
	{
		// endpoints are 4c
		// mask must be 0 (none transparent)
		return mask == 0;
	}
	else
	{
		// transparent pels must be at indices == 3
		U32 indices3c = indices & (indices>>1) & 0x55555555;
		return mask == indices3c;
	}
}

bool DXT1_OneBitTransparent_Same(const rrColorBlock4x4 & b1,const rrColorBlock4x4 & b2)
{
	for LOOP(i,16)
	{
		bool t1 = ( b1.colors[i].dw == 0 );
		bool t2 = ( b2.colors[i].dw == 0 );
		if ( t1 != t2 )
			return false;
	}
	return true;
}

bool DXT1_OneBitTransparent_Same(const rrColorBlock4x4 & block,U32 endpoints, U32 indices)
{
	U32 mask = DXT1_OneBitTransparent_Mask(block);
	return DXT1_OneBitTransparent_Mask_Same(mask,endpoints,indices);
}


#ifdef RR_DO_ASSERTS
// DXT1_ComputeSSD_OneBitTransparent
// returns same error as DXT1_FindIndices_Alpha
//	this absolutely forbids transparency changes (returns RR_DXTC_ERROR_BIG)
U32 DXT1_ComputeSSD_OneBitTransparent(const rrColorBlock4x4 & block,const rrDXT1Block & dxtb,rrDXT1PaletteMode mode)
{
	// this is now used only in asserts
	// differs from DXT1_ComputeSSD_RGBA if the 1bt mask changes
	//	1bt mask is not allowed to change in any valid coding
	//	so this should == DXT1_ComputeSSD_RGBA for all valid codings

	if ( mode != rrDXT1PaletteMode_Alpha )
	{
		// don't care about alpha, just do RGB SSD
		return DXT1_ComputeSSD_RGBA(block,dxtb,mode);
	}
	
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(dxtb.c0,dxtb.c1,palette,rrDXT1PaletteMode_Alpha);
	
	U32 err = 0;
	U32 indices = dxtb.indices;
	
	RR_ASSERT( mode == rrDXT1PaletteMode_Alpha );
	bool block_has_transparent = rrColorBlock4x4_HasAnyOneBitTransparent(&block);
	bool palette_has_transparent = ( palette[3].u.a == 0 );
	
	if ( block_has_transparent != palette_has_transparent )
	{
		// mismatch!
		if ( block_has_transparent )
		{
			// block has transparent but I can't map it to any palette entry
			// I consider "transparent or not" to be a bit I'm not allowed to change
			// just big error
			return RR_DXTC_ERROR_BIG;
		}
		else
		{
			// palette has transparent but block does not
			// must not use index 3
			
			for(int i=0;i<16;i++)
			{
				int index = indices&3; indices >>= 2;
				if ( index == 3 )
					return RR_DXTC_ERROR_BIG;				
				err += rrColor32BGRA_DeltaSqrRGB( block.colors[i] , palette[index] );
			}
		}
	}
	else
	{
		if ( block_has_transparent )
		{
			// both have alpha
			
			for(int i=0;i<16;i++)
			{
				int index = indices&3; indices >>= 2;
				bool c_t = rrColor32BGRA_IsOneBitTransparent(block.colors[i]);
				bool p_t = (index == 3);
				if ( c_t != p_t )
				{
					// transparency bit	mismatch
					return RR_DXTC_ERROR_BIG;			
				}
				else
				{
					// transparent bit is the same		
					// if transparent, RGB does not contribute
					// (no point counting the RGB error when transparent because DXT1 transparent is always black)
					if ( ! c_t )
					{
						err += rrColor32BGRA_DeltaSqrRGB( block.colors[i] , palette[index] );
					}
				}
			}
		}
		else	
		{
			// neither have alpha
		
			for(int i=0;i<16;i++)
			{
				int index = indices&3; indices >>= 2;
				err += rrColor32BGRA_DeltaSqrRGB( block.colors[i] , palette[index] );
			}
		}
	}
		
	return err;
}
#endif // RR_DO_ASSERTS


bool rrColorBlock4x4_IsBC1Canonical(const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode)
{
	// at this point colors should already be canonical :
	// either A = 255
	// or canonicalized 1-bit-transparent alpha :

	if ( mode == rrDXT1PaletteMode_Alpha )
	{
		for LOOP(i,16)
		{
			if ( ! rrColor32BGRA_IsOneBitTransparentCanonical(colors.colors[i]) )
				return false;
		}
	}
	else
	{
		for LOOP(i,16)
		{
			// all a's should be 255
			if ( colors.colors[i].u.a != 255 )
				return false;
		}
	}
	
	return true;
}


// NOTE: this is actual RGBA SSD , no A bias
U32 DXT1_ComputeSSD_RGBA(const rrColorBlock4x4 & color,const rrDXT1Block & dxtb, rrDXT1PaletteMode mode)
{
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(color,mode) );

	rrColor32BGRA palette[4];
	DXT1_ComputePalette(dxtb.c0,dxtb.c1,palette,mode);
	
	// RGBA diff but we should be canonical
	U32 ret = BC1_Palette_SSD_RGBA(&color,palette,dxtb.indices);
		
	return ret;
}

U32 DXT1_ComputeSAD_RGBA(const rrColorBlock4x4 & color,const rrDXT1Block & dxtb, rrDXT1PaletteMode mode)
{
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(color,mode) );

	rrColor32BGRA palette[4];
	DXT1_ComputePalette(dxtb.c0,dxtb.c1,palette,mode);
	
	// RGBA diff but we should be canonical
	U32 ret = BC1_Palette_SAD_RGBA(&color,palette,dxtb.indices);
		
	return ret;
}

/*
U32 rrColorBlock4x4_ComputeSSD_RGB(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs)
{
	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += rrColor32BGRA_DeltaSqrRGB( lhs.colors[i] , rhs.colors[i] );
	}
	
	return err;
}

U32 rrColorBlock4x4_ComputeSAD_RGB(const rrColorBlock4x4 & lhs,const rrColorBlock4x4 & rhs)
{
	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += rrColor32BGRA_DeltaSADRGB( lhs.colors[i] , rhs.colors[i] );
	}
	
	return err;
}

U32 rrSingleChannelBlock4x4_ComputeSSD(const rrSingleChannelBlock4x4 & lhs,const rrSingleChannelBlock4x4 & rhs)
{
	U32 err = 0;
	
	for(int i=0;i<16;i++)
	{
		err += S32_Square( lhs.values[i] - rhs.values[i] );
	}
	
	return err;
}
*/

/*
U32 DXT3_ComputeSSD(const rrSingleChannelBlock4x4 & original,const rrDXT3AlphaBlock & dxtb)
{
	// easy/slow implemention :
	
	rrSingleChannelBlock4x4 dec;
	DXT3_DecompressAlpha(&dec,dxtb);
	return rrSingleChannelBlock4x4_ComputeSSD(original,dec);
}


U32 DXT5_ComputeSSD(const rrSingleChannelBlock4x4 & original,const rrDXT5AlphaBlock & dxtb)
{
	// easy/slow implemention :
	
	rrSingleChannelBlock4x4 dec;
	DXT5_DecompressAlpha(&dec,dxtb);
	return rrSingleChannelBlock4x4_ComputeSSD(original,dec);
}
*/

void SwapRB(rrColorBlock4x4 * pBlock)
{
	for(int i=0;i<16;i++)
	{
		RR_NAMESPACE::swap( pBlock->colors[i].u.r, pBlock->colors[i].u.b );
	}
}

// RemoveAlpha changes original - makes it opaque !
void RemoveAlpha(rrSingleChannelBlock4x4 * pTo,rrColorBlock4x4 & from)
{
	for(int i=0;i<16;i++)
	{
		pTo->values[i] = from.colors[i].u.a;
		from.colors[i].u.a = 255;
	}
}

void KillAlpha(rrColorBlock4x4 & from)
{
	for(int i=0;i<16;i++)
	{
		from.colors[i].u.a = 255;
	}
}

void ReplaceAlpha(rrColorBlock4x4 * pTo,const rrSingleChannelBlock4x4 & from)
{
	for(int i=0;i<16;i++)
	{
		pTo->colors[i].u.a = from.values[i];
	}
}

//===================================================================================================

void DXT3_Decompress(rrColorBlock4x4 * pTo,const rrDXT3Block & from)
{
	// easy/slow implemention :
	
	rrSingleChannelBlock4x4 alpha;
	DXT3_DecompressAlpha(&alpha,from.alpha);
	
	DXT1_Decompress(pTo,from.dxt1block,rrDXT1PaletteMode_FourColor);
	
	ReplaceAlpha(pTo,alpha);
}

void DXT5_Decompress(rrColorBlock4x4 * pTo,const rrDXT5Block & from)
{
	// easy/slow implemention :
	
	rrSingleChannelBlock4x4 alpha;
	DXT5_DecompressAlpha(&alpha,from.alpha);
	
	DXT1_Decompress(pTo,from.dxt1block,rrDXT1PaletteMode_FourColor);
	
	ReplaceAlpha(pTo,alpha);
}

//===================================================================================================

// returns (err << 2) | index
static inline U32 DXT1_FindIndexAndErr_Brute2_RGBA_1BT(rrColor32BGRA color, rrColor32BGRA palette[4])
{
	// picks color indices by these rules, in priority order:
	// 1. must match target alpha
	// 2. if alpha matches, pick color index by minimum error
	// 3. on ties, pick lowest numerical index
	//
	// color need to be canonicalized for the alpha handling to work,
	// but it's the same for all palette modes.
	U32 d0 = rrColor32BGRA_DeltaSqrRGBA_1BT(color,palette[0]);
	U32 d1 = rrColor32BGRA_DeltaSqrRGBA_1BT(color,palette[1]);
	U32 d2 = rrColor32BGRA_DeltaSqrRGBA_1BT(color,palette[2]);
	U32 d3 = rrColor32BGRA_DeltaSqrRGBA_1BT(color,palette[3]);
	
	U32 best = d0*4;
	best = RR_MIN(best, d1*4 + 1);
	best = RR_MIN(best, d2*4 + 2);
	best = RR_MIN(best, d3*4 + 3);
	
	return best;
}

//===============================================================================

U32 DXT1_FindIndices(const rrColorBlock4x4 & block, U32 endpoints,rrDXT1PaletteMode mode, U32 * pError)
{
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(endpoints,palette,mode);
	return DXT1_FindIndices(block,palette,mode,pError);
}


#ifdef __RADX86__ // FindIndices 

static RADFORCEINLINE Vec128 DeltaSqrRGBA_SSE2(
	const Vec128 & x_br16, const Vec128 & x_ga16, const Vec128 & y_br16, const Vec128 & y_ga16)
{
	Vec128 diff_br16 = _mm_sub_epi16(y_br16, x_br16); // db, dr
	Vec128 diff_ga16 = _mm_sub_epi16(y_ga16, x_ga16); // dg, da

	Vec128 sumsq_br32 = _mm_madd_epi16(diff_br16, diff_br16); // db*db + dr*dr
	Vec128 sumsq_ga32 = _mm_madd_epi16(diff_ga16, diff_ga16); // dg*dg + da*da

	return _mm_add_epi32(sumsq_br32, sumsq_ga32); // db*db + dg*dg + dr*dr + da*da
}

static RADFORCEINLINE Vec128 MinS32_SSE2(Vec128 a, Vec128 b)
{
#ifdef DO_BUILD_SSE4
	return _mm_min_epi32(a, b);
#else
	Vec128 a_greater = _mm_cmpgt_epi32(a, b);
	return _mm_or_si128(_mm_and_si128(b, a_greater), _mm_andnot_si128(a_greater, a));
#endif
}

// With SSE2, brute force works great.
U32 DXT1_FindIndices(const rrColorBlock4x4 & block, rrColor32BGRA palette[4],rrDXT1PaletteMode mode,U32 * pError)
{
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(block,mode) );

	// There's a bunch of assumptions and simplifications in here, enough so that it's hard to all keep straight,
	// so here's the full rationale for everything in here:
	//
	// The basic goal is to select, per pixel, the palette entry that minimizes the SSD (sum of squared
	// differences) in the color channels, but with a few wrinkles.
	//
	// Inputs are expected to be canonicalized for our given palette mode (checked above). In particular, this
	// means that in 4c mode, all alpha values in "block" _must_ be 255, and in 3c mode they must be either
	// 0 or 255. Note that our generated palette in 3c mode depends on the palette mode! In when palette mode
	// has alpha enabled, index 3 has alpha=0 (which is how the GPU decodes it), but in no-alpha mode, we
	// actually consider index 3 to have alpha=255, and likewise canonicalize all our inputs to have alpha=255
	// for every pixel. Thus the computed error in the alpha channel is always 0 for every pixel in NoAlpha
	// mode.
	//
	// With transparency enabled, we require exact match of the 1-bit transparency flag. The way we accomplish
	// this is by scaling up alpha differences so that a mismatch in the A channel is considered a larger
	// error than the largest possible RGB error. As noted above, the A channel values in both our palette and
	// the reference block only ever contain 0 or 255. Therefore, the SSD component for the alpha channel is
	// at most 255^2 = 65,025. However, we have 3 other color chanels that can also have an SSD contribution of
	// up to 65,025 each. To make sure that we always exactly match in the A channel, we scale up the diffs
	// in A by a factor of 2, which scales the A contribution in the SSD by a factor of 4, enough to be larger
	// than any error contribution from the remaining 3 channels. This is enough to guarantee that we always
	// match A exactly, meaning the A contribution from the palette index we return is always 0. So somewhat
	// counter-intuitively, even though what we compute is a weighted SSD, our return value is always a
	// pure unweighted RGB SSD with exact match in the 1-bit alpha channel (subject to the NoAlpha rules above).
	//
	// SSD calculation happens in fixed point. The SSE2 instruction PMADDWD (_mm_madd_epi16) happens to be
	// very convenient for this, so it's what we use. This works on 16-bit signed integer values. Instead of
	// unpacking channels to 16-bit, it ends up cheaper to use masking, which means we have one PMADDWD dot
	// product for the B/R channels diffs (the even-numbered channels), and another for the odd-numbered
	// channels.
	//
	// After computing the SSDs for the 4 palette entries, we need to select the index that results in the
	// smallest SSD. Individual errors per pixel fit in 18 bits comfortably, and we have 16 pixels, so this
	// sum fits in 22 bits. That means that instead of doing a comparison tree or similar, we can simply
	// multiply the SSD by 4 and stuff the index value in the bottom 2 bits. Then we get both the index for
	// the palette entry with the smallest error, and what that error is, by computing the min over all 4
	// values of (SSD[i]*4 + i). (Ties are broken towards smaller indices.)
	//
	// Instead of multiplying the final SSDs by 4, it happens to be slightly cheaper to scale the values
	// in all color channels by 2 before we compute the differences. Since we independently also scale
	// the A channel by 2, that means the R,G,B channel values get scaled by 2, whereas A gets scaled by
	// a total of 4 prior to the error calculation.
	//
	// In terms of value range, that means the contributions of the R,G,B channels are at most 510^2 each
	// per pixel (fits in 18 bits), and the A contribution (pre-min reduction) is at most 1020^2 (fits in
	// 20 bits). As noted above, we never pick pixels with an effective A error that is nonzero after
	// calculation the mins, and we'd be good on range and free from overflows either way.

	// Load all 16 pixels worth of data, and the palette
	Vec128 pal  = load128u(palette);
	Vec128 mask_br = _mm_set1_epi16(0xff);
	Vec128 const32_1 = _mm_set1_epi32(1);
	Vec128 const32_2 = _mm_set1_epi32(2);
	Vec128 const32_3 = _mm_set1_epi32(3);
	Vec128 scale_ga = _mm_set1_epi32(0x040002);
	Vec128 magic_mul = _mm_set1_epi16(0x4080);
	Vec128 error_sum = _mm_setzero_si128();
	U32 indices = 0;

	Vec128 pal_br16 = _mm_and_si128(pal, mask_br); // bytes: B,0,R,0 -> LE words: B,R
	Vec128 pal_ga16 = _mm_srli_epi16(pal, 8); // LE words: G,A

	// double the palette entries and also the pixels (below)
	// this scales the resulting squared deltas by 4 which lets us avoid
	// having to shift everything by 2 in the inner loop
	pal_br16 = _mm_add_epi16(pal_br16, pal_br16);
	// scale G up by 2 , but A up by 4 :
	// this makes an A difference 4*255^2
	//	vs a full RGBA black to white which is 3*255^2
	//	guarantees we preserve binary alpha
	pal_ga16 = _mm_mullo_epi16(pal_ga16, scale_ga);

	// process rows in reverse order since it makes index math slightly easier
	for (int i = 12; i >= 0; i -= 4)
	{
		// One row's worth of data
		Vec128 row = load128u(block.colors + i);
		Vec128 row_br16 = _mm_and_si128(row, mask_br);
		Vec128 row_ga16 = _mm_srli_epi16(row, 8);

		// again, scale row values
		row_br16 = _mm_add_epi16(row_br16, row_br16);
		row_ga16 = _mm_mullo_epi16(row_ga16, scale_ga);

		Vec128 d0 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0x00), _mm_shuffle_epi32(pal_ga16, 0x00));
		Vec128 d1 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0x55), _mm_shuffle_epi32(pal_ga16, 0x55));
		Vec128 d2 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0xaa), _mm_shuffle_epi32(pal_ga16, 0xaa));
		Vec128 d3 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0xff), _mm_shuffle_epi32(pal_ga16, 0xff));

		Vec128 best = d0; // d0 is multiplied by 4 and has 0 in low bits -> perfect
		best = MinS32_SSE2(best, _mm_add_epi32(d1, const32_1));
		best = MinS32_SSE2(best, _mm_add_epi32(d2, const32_2));
		best = MinS32_SSE2(best, _mm_add_epi32(d3, const32_3));

		// Now have 2b best color index in low bits of every lane
		// and squared error in top 30 bits; accumulate error first
		error_sum = _mm_add_epi32(error_sum, _mm_srli_epi32(best, 2));

		// Now extract index bits fancily (NOTE slightly wasteful, should do this for two rows at once)
		Vec128 best_inds = _mm_and_si128(best, const32_3);
		Vec128 packed = _mm_packs_epi32(best_inds, _mm_setzero_si128()); // now have 16-bit fields
		Vec128 magicked = _mm_mullo_epi16(packed, magic_mul); // move bit 0 of index into bit 7 of 16b lane, and bit 1 of index into bit 15
		U32 bits = _mm_movemask_epi8(magicked); // and poof, movemask does the rest

		indices = (indices << 8) | bits;
	}

	// Horizontal reduction for final error sum
	if (pError)
	{
		error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0xb1)); // add to one away
		error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0x4e)); // add to two away
		*pError = _mm_cvtsi128_si32(error_sum);
	}
	
	#ifdef RR_DO_ASSERTS
	if ( mode == rrDXT1PaletteMode_Alpha )
	{
		U32 mask = DXT1_OneBitTransparent_Mask(block);
		bool is3c = palette[3].dw == 0;
		// if block has any alpha, and we're not in 3c mode
		//	then it's uncodeable
		//	-> should catch that sooner
		RR_ASSERT( mask == 0 || is3c );
		U32 made = DXT1_OneBitTransparent_Mask_FromIndices(is3c,indices);
		// verify 1bit transparency is preserved :
		RR_ASSERT( made == mask );
		// -> this means there should be zero contribution from A to the output error
		//	so the different A scaling does not change the SSD error result
	}
	#endif

	return indices;
}

#else // not X86

//===============================================================================
// scalar fallbacks for FindIndices :

U32 DXT1_FindIndices(const rrColorBlock4x4 & block, rrColor32BGRA palette[4],rrDXT1PaletteMode mode,U32 * pError)
{
	RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(block,mode) );

	U32 indices = 0;
	U32 err = 0;

	for(int i=0;i<16;i++)
	{
		U32 index_err = DXT1_FindIndexAndErr_Brute2_RGBA_1BT(block.colors[i],palette);
	
		indices >>= 2;
		indices |= index_err << 30; // ends up implicitly masking out just the bottom 2 bits, great!

		err += index_err >> 2;
	}

	if ( pError )
		*pError = err;

	return indices;
}

#endif // processor for FindIndices

//===================================================================================================

void rrDXT1_PutBlock(U8 * outPtr,const rrDXT1Block & block)
{
	#ifdef __RADLITTLEENDIAN__
	*((rrDXT1Block *)outPtr) = block;
	#else
	#pragma error // need to put words as little endian
	#endif
}

void rrDXT1_GetBlock(const U8 * inPtr, rrDXT1Block * pBlock)
{
	#ifdef __RADLITTLEENDIAN__
	*pBlock = *((rrDXT1Block *)inPtr);
	#else
	#pragma error // need to put words as little endian
	#endif	
}

void rrDXT3Alpha_PutBlock(U8 * outPtr,const rrDXT3AlphaBlock & block)
{
	#ifdef __RADLITTLEENDIAN__
	*((rrDXT3AlphaBlock *)outPtr) = block;
	#else
	#pragma error // need to put words as little endian
	#endif
}

void rrDXT3Alpha_GetBlock(const U8 * inPtr, rrDXT3AlphaBlock * pBlock)
{
	#ifdef __RADLITTLEENDIAN__
	*pBlock = *((rrDXT3AlphaBlock *)inPtr);
	#else
	#pragma error // need to put words as little endian
	#endif	
}

void rrDXT5Alpha_GetBlock(const U8 * inPtr, rrDXT5AlphaBlock * pBlock)
{
	// DXT5 is okay to assign
	*pBlock = *((rrDXT5AlphaBlock *)inPtr);
}

//===================================================================================================

bool rrColorBlock4x4_HasAnyOneBitTransparent(const rrColorBlock4x4 * pBlock)
{
	for(int i=0;i<16;i++)
	{
		if ( rrColor32BGRA_IsOneBitTransparent(pBlock->colors[i]) )
			return true;
	}
	
	return false;
}


RR_NAMESPACE_END
