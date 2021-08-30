// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#include "rrsurfacebc67.h"
#include "rrsurfacerowcache.h"
#include "rrsurfacedxtc.h"
#include "rrcolor.h"
#include "rrdxtcblock.h"
#include "bc67format.h"
#include "bc6compress.h"
#include "bc7compress.h"
#include "bc7decode_fast.h"
#include "blocksurface.h"

#ifdef __RADX86__
#include "cpux86.h"
#endif

RR_NAMESPACE_START

void BC6H_DecodeBlock(U16 * pTo, const void * from, bool isSigned)
{
	bc6h_decode_block(pTo, 4 * 4 * sizeof(U16), isSigned, from);
}

void BC7_DecodeBlock(rrColorBlock4x4RGBA * pTo,const void * from)
{
	bc7_decode_block_fast((U8 *)pTo,from);
}

rrbool rrSurfaceDXTC_DecompressBC6H(BlockSurface * to, const BlockSurface * from )
{
	RR_ASSERT_ALWAYS( from->pixelFormat == rrPixelFormat_BC6U || from->pixelFormat == rrPixelFormat_BC6S );
	bool is_signed = from->pixelFormat == rrPixelFormat_BC6S;

	BlockSurface_Alloc(to,from->count,rrPixelFormat_4_F16);
		
	// for each block :
	for LOOP(blocki,from->count)
	{
		const U8 * inPtr = BlockSurface_SeekC(from,blocki);
		U8 * outPtr = BlockSurface_Seek(to,blocki);
		
		//U16 colors[4*4*4];
		U16 * colors = (U16 *) outPtr;
		BC6H_DecodeBlock(colors,inPtr,is_signed);
	}

	return true;
}

rrbool rrSurfaceDXTC_DecompressBC7(BlockSurface * to, const BlockSurface * from )
{
	RR_ASSERT_ALWAYS( from->pixelFormat == rrPixelFormat_BC7 );
	
	BlockSurface_Alloc(to,from->count,rrPixelFormat_R8G8B8A8);
		
	// for each block :
	for LOOP(blocki,from->count)
	{
		const U8 * inPtr = BlockSurface_SeekC(from,blocki);
		U8 * outPtr = BlockSurface_Seek(to,blocki);
		
		rrColorBlock4x4RGBA * pcolors = (rrColorBlock4x4RGBA *)outPtr;
		BC7_DecodeBlock(pcolors,inPtr);
		//RR_NOP();
	}

	return true;
}

rrbool rrSurfaceDXTC_CompressBC6H(BlockSurface * to, const BlockSurface * from, bool isSigned, rrDXTCLevel level, rrDXTCOptions options)
{
#if defined(__RADX86__) && defined(DO_BUILD_SSE4)
	// BC6 encode on x86 requires SSE4.1 now.
	if ( ! rrCPUx86_feature_present(RRX86_CPU_SSE41) )
		return false;
#endif

	rrPixelFormat to_fmt = isSigned ? rrPixelFormat_BC6S : rrPixelFormat_BC6U;
	RR_ASSERT_ALWAYS( to->pixelFormat == to_fmt );

	BlockSurfaceObj from_F16;
	BlockSurface_AllocCopyOrSetViewIfFormatMatches_Normalized(&from_F16,from,rrPixelFormat_4_F16);

	BC6EncOptions opts;
	bc6enc_options_init(&opts,level,options,isSigned);

	// for each block :
	for LOOP(blocki,from->count)
	{
		const U8 * inPtr = BlockSurface_SeekC(&from_F16,blocki);
		U8 * outPtr = BlockSurface_Seek(to,blocki);
		
		const U16 * pixels = (const U16 *)inPtr;
		
		bc6enc_compress_block(outPtr, pixels, opts);
	}

	return true;
}

rrbool rrSurfaceDXTC_CompressBC7(BlockSurface * to, const BlockSurface * from, rrDXTCLevel level, rrDXTCOptions options)
{
#if defined(__RADX86__) && defined(DO_BUILD_SSE4)
	// BC7 encode on x86 requires SSE4.1 now.
	if ( ! rrCPUx86_feature_present(RRX86_CPU_SSE41) )
		return false;
#endif

	RR_ASSERT( from->count == to->count );
	RR_ASSERT( to->pixelFormat == rrPixelFormat_BC7 );

	// rrColor32RGBA not rrColor32BGRA
	BlockSurfaceObj from_converted;
	bool rgbx_ok = (options & rrDXTCOptions_BC7_IgnoreAlpha);
	bool is_rgba = BlockSurface_SetView_to_RGBA8_or_BGRA8(&from_converted,from,rgbx_ok,rrPixelFormat_R8G8B8A8);
			
	// for each block :
	
	if ( ! is_rgba )
	{
		for LOOP(blocki,from->count)
		{
			const U8 * inPtr = BlockSurface_SeekC(&from_converted,blocki);
			U8 * outPtr = BlockSurface_Seek(to,blocki);
		
			rrColorBlock4x4 colors = *((const rrColorBlock4x4 *)inPtr);
			SwapRB(&colors);
			BC7_CompressBlock(outPtr, (const U8 *)&colors, level, options);
		}
	}
	else
	{
		for LOOP(blocki,from->count)
		{
			const U8 * inPtr = BlockSurface_SeekC(&from_converted,blocki);
			U8 * outPtr = BlockSurface_Seek(to,blocki);
		
			BC7_CompressBlock(outPtr, inPtr, level, options);
		}
	}

	return true;
}

RR_NAMESPACE_END
