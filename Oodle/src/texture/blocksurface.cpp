// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#include "blocksurface.h"
#include "oodlemalloc.h"
#include "rrsurfaceblit.h"
#include "rrsurfacerowcache.h"
#include "layout.h"
#include "threadprofiler.h"

RR_NAMESPACE_START

void BlockSurface_SetView(BlockSurface * bs, const BlockSurface * from)
{	
	*bs = *from;
	bs->freeData = false;
}

void BlockSurface_SetView(BlockSurface * bs, const BlockSurface * from, int start, int count )
{
	BlockSurface_Init(bs);

	RR_ASSERT( start+count <= from->count );
	
	bs->blocks = from->blocks + from->blockSizeBytes * start;
	bs->count = count;
	bs->blockSizeBytes = from->blockSizeBytes;
	bs->freeData = false;
	bs->pixelFormat = from->pixelFormat;
}

void BlockSurface_SetView(BlockSurface * bs, void * blocks, int count, rrPixelFormat pf )
{	
	BlockSurface_Init(bs);
	
	bs->blocks = (U8 *)blocks;
	bs->freeData = false;
	bs->count = count;
	bs->pixelFormat = pf;
	bs->blockSizeBytes = rrPixelFormat_Get4x4BlockSizeBytes(pf);
}

void BlockSurface_Init(BlockSurface * bs)
{
	RR_ZERO(*bs);
	//bs->format = rrPixelFormat_Invalid; // == 0
}

void BlockSurface_Free(BlockSurface * bs)
{
	if ( bs->freeData )
	{
		OodleFree(bs->blocks);
	}
	
	RR_ZERO(*bs);
}

void BlockSurface_AllocCopy(BlockSurface * bs,const BlockSurface * from)
{
	BlockSurface_Alloc(bs, from->count, from->pixelFormat );
	
	SINTa size = BlockSurface_GetDataSizeBytes(bs);
	RR_ASSERT_ALWAYS( size == BlockSurface_GetDataSizeBytes(from) );
	
	memcpy(bs->blocks, from->blocks, size );
}

void BlockSurface_Copy(BlockSurface * to_blocks,const BlockSurface * from_blocks)
{
	RR_ASSERT( to_blocks->blocks != NULL );
	RR_ASSERT( to_blocks->count == from_blocks->count );
	RR_ASSERT( to_blocks->pixelFormat == from_blocks->pixelFormat );
	SINTa bytes = BlockSurface_GetDataSizeBytes(from_blocks);
	RR_ASSERT( bytes == BlockSurface_GetDataSizeBytes(to_blocks) );
	
	memcpy(to_blocks->blocks,from_blocks->blocks,bytes);
}

void BlockSurface_Alloc(BlockSurface * bs,int num_blocks, rrPixelFormat pixelFormat)
{
	if ( bs->count == num_blocks &&
		bs->pixelFormat == pixelFormat )
	{
		return;
	}
	
	BlockSurface_Free(bs);
	
	bs->pixelFormat = pixelFormat;
	bs->freeData = true;
	bs->count = num_blocks;
	bs->blockSizeBytes = rrPixelFormat_Get4x4BlockSizeBytes(pixelFormat);
	
	SINTa size = BlockSurface_GetDataSizeBytes(bs);
	bs->blocks = (U8 *) OodleMalloc(size);
}

rrbool BlockSurface_Blit_NonNormalized(BlockSurface * to_bs,const BlockSurface * fm_bs)
{
	RR_ASSERT_ALWAYS( to_bs != fm_bs );
	RR_ASSERT_ALWAYS( to_bs->count == fm_bs->count );

	// do a format changing blit :
	rrSurface rrs_to = { };
	rrSurface rrs_fm = { };
	BlockSurface_Set_RRS_View(&rrs_to,to_bs);
	BlockSurface_Set_RRS_View(&rrs_fm,fm_bs);
	
	rrbool ok = rrSurface_Blit_NonNormalized(&rrs_to,&rrs_fm);
	return ok;
}

rrbool BlockSurface_BlitNormalized(BlockSurface * to_bs,const BlockSurface * fm_bs,rrRangeRemap remap)
{
	RR_ASSERT_ALWAYS( to_bs != fm_bs );
	RR_ASSERT_ALWAYS( to_bs->count == fm_bs->count );

	// do a format changing blit :
	rrSurface rrs_to = { };
	rrSurface rrs_fm = { };
	BlockSurface_Set_RRS_View(&rrs_to,to_bs);
	BlockSurface_Set_RRS_View(&rrs_fm,fm_bs);

	rrbool ok = rrSurface_BlitNormalized(&rrs_to,&rrs_fm,remap);
	return ok;
}

void BlockSurface_AllocCopyOrSetViewIfFormatMatches_NonNormalized(BlockSurface * bs,const BlockSurface * from,rrPixelFormat format)
{
	RR_ASSERT( bs != from );
	if ( from->pixelFormat == format )
	{
		BlockSurface_SetView(bs,from);
		return;
	}
	
	// needs a pixel format change
	BlockSurface_Alloc(bs,from->count,format);
	
	// not for BCN :
	RR_ASSERT( ! rrPixelFormat_IsBlockCompressed(from->pixelFormat) );
	RR_ASSERT( ! rrPixelFormat_IsBlockCompressed(format) );
	
	// do a format changing blit :
	BlockSurface_Blit_NonNormalized(bs,from);
}

void BlockSurface_AllocCopyOrSetViewIfFormatMatches_Normalized(BlockSurface * bs,const BlockSurface * from,rrPixelFormat format,rrRangeRemap remap)
{
	RR_ASSERT( bs != from );
	if ( from->pixelFormat == format && remap == rrRangeRemap_None )
	{
		BlockSurface_SetView(bs,from);
		return;
	}

	// needs a pixel format change
	BlockSurface_Alloc(bs,from->count,format);

	// not for BCN :
	RR_ASSERT( ! rrPixelFormat_IsBlockCompressed(from->pixelFormat) );
	RR_ASSERT( ! rrPixelFormat_IsBlockCompressed(format) );

	// do a format changing blit :
	BlockSurface_BlitNormalized(bs,from,remap);
}

void BlockSurface_SetView_of_RRS_BCN(BlockSurface * bs, const rrSurface * from)
{
	BlockSurface_Init(bs);
	// make this BS a view of the BCN RRS

	// from must be BCN
	RR_ASSERT( rrPixelFormat_IsBlockCompressed(from->pixelFormat) );
	
	//BlockSurface_Free(bs);
	
	bs->blocks = from->data;
	bs->pixelFormat = from->pixelFormat;
	bs->blockSizeBytes = rrPixelFormat_Get4x4BlockSizeBytes(bs->pixelFormat);
	bs->freeData = false;
	
	int nbx = (from->width  + 3)/4;
	int nby = (from->height + 3)/4;
	bs->count = nbx*nby;
	
	// assumes from stride is tight packed :
	RR_ASSERT_ALWAYS( from->stride == nbx * bs->blockSizeBytes );
}

void BlockSurface_Set_RRS_View(rrSurface * surf,const BlockSurface * bs)
{
	rrSurface_Init(surf);
	// make RRS a view of this BS
	//	note the real layout, just a way to get the pixel

	//rrSurface_Free(surf);
	surf->data = bs->blocks;
	surf->freeData = false;
	surf->height = 4;
	surf->width = bs->count * 4;
	// or height = 1 and width = count * 16
	//	doesn't matter much
	surf->pixelFormat = bs->pixelFormat;
	surf->stride = rrPixelFormat_MakeStride_Minimum(surf->pixelFormat,surf->width);
}

// @@ this file does lots of call memcpy on pixels
//	bypp is in 1-16
//  should be templates with an outer dispatch
// it bothers me philosophically but isn't really practically relevant

static void copy_4x4_block_to_rows_advance(U8ptr to_rows[4],U8cptr & fm_ptr,int bypp)
{
	int bypr = bypp*4;
	
	for LOOP(r,4)
	{
		memcpy(to_rows[r],fm_ptr,bypr);
		to_rows[r] += bypr;
		fm_ptr += bypr;
	}
}

static void copy_4x4_rows_to_block_advance(U8ptr & to_ptr,U8cptr fm_rows[4],int bypp)
{
	int bypr = bypp*4;
	
	for LOOP(r,4)
	{
		memcpy(to_ptr,fm_rows[r],bypr);
		fm_rows[r] += bypr;
		to_ptr += bypr;
	}
}

void BlockSurface_Copy_to_RRS_SameFormat(rrSurface* to_array,int num_surfaces,const BlockSurface * from)
{
	THREADPROFILESCOPE("blksrfcpy");

	RR_ASSERT_ALWAYS( to_array[0].pixelFormat == from->pixelFormat );
	
	int to_block_count = TotalBlockCount(to_array,num_surfaces);
	RR_ASSERT_ALWAYS( from->count == to_block_count );
	
	const U8 * fromPtr = from->blocks;
	const U8 * fromEnd = fromPtr + BlockSurface_GetDataSizeBytes(from);
		
	if ( rrPixelFormat_IsBlockCompressed(from->pixelFormat) )
	{
		for LOOP(i,num_surfaces)
		{
			rrSurface * to = to_array+i;

			// assumes from stride is tight packed :
			//	@@ or copy row by row
			int nbx = (to->width  + 3)/4;
			RR_ASSERT_ALWAYS( to->stride == nbx * from->blockSizeBytes );
		
			SINTa bytes = from->blockSizeBytes * wh_to_num_blocks(to->width,to->height);
			memcpy(to->data,fromPtr,bytes);
			fromPtr += bytes;
		}
	}
	else
	{
		// do 4x4 -> rows
		
		rrSurfaceRowCache toRows;
		int bypp = rrPixelFormat_GetInfo(from->pixelFormat)->bytesPerPixel;
		RR_ASSERT( bypp >= 1 && bypp <= 16 );
		
		for LOOP(i,num_surfaces)
		{
			rrSurface * to = to_array+i;

			toRows.Start(to,from->pixelFormat,8,RR_SURFACE_ROW_CACHE_WRITE,4);
			
			// RowCache padding takes care of partial blocks for us
			
			int nbx = (to->width  + 3)/4;

			// work on rows of blocks :
			for(int y=0;y<to->height;y+=4)
			{			
				toRows.MoveCache(y,4);
			
				U8 * rows[4];
				for(int r=0;r<4;r++)
					rows[r] = (U8 *) toRows.GetRow(y + r);
			
				// @@ todo template switch on bypp
				// bypp is in [1,16]
			
				// for each block :
				for LOOP(bx,nbx)
				{
					copy_4x4_block_to_rows_advance(rows,fromPtr,bypp);
					// fromPtr is advanced
				}
			}

			toRows.FlushWrite();
		}
	}

	RR_ASSERT_ALWAYS( fromPtr == fromEnd );	
}

void BlockSurface_Copy_to_RRS_SameFormat_Layout(rrSurface* to_surfaces,int num_surfaces,const BlockSurface * from,const OodleTex_Layout * layout)
{
	if ( !layout )
	{
		BlockSurface_Copy_to_RRS_SameFormat(to_surfaces,num_surfaces,from);
		return;
	}
	
	THREADPROFILESCOPE("blksrfcpy");

	RR_ASSERT( num_surfaces == layout->m_surfaces.size32() );
	RR_ASSERT_ALWAYS( to_surfaces->pixelFormat == from->pixelFormat );

	if ( ! rrPixelFormat_IsBlockCompressed(from->pixelFormat) )
	{
		// do 4x4 -> rows

		const U8 * blockPtr = from->blocks;
		
		int bypp = rrPixelFormat_GetInfo(from->pixelFormat)->bytesPerPixel;
		RR_ASSERT( bypp >= 1 && bypp <= 16 );

		RR_ASSERT( from->blockSizeBytes == bypp*16 );

		for (SINTa bi = 0; bi < layout->m_nblocks; ++bi)
		{
			const BlockLocation8 * loc = layout->m_block_ids + bi;
			if ( loc->surface_id_plus_1 == 0 )
			{
				// intentional null block
				blockPtr += from->blockSizeBytes;
				continue;
			}

			rrSurface * to = to_surfaces + loc->surface_id_plus_1 - 1;

			// just assert not assert_always since layout creation does validate this
			int to_x = loc->x;
			int to_y = loc->y;

			if ( to_x < 0 )
			{
				// intentional null block
				blockPtr += from->blockSizeBytes;
				continue;
			}

			RR_ASSERT( to_x >= 0 && to_x < to->width );
			RR_ASSERT( to_y >= 0 && to_y < to->height );

			// @@ todo template switch on bypp
			// bypp is in [1,16]

			// need to handle partial blocks here, don't have RowCache to help
			if ( to_x+4 > to->width || to_y+4 > to->height )
			{
				for LOOP(by,4)
				{
					for LOOP(bx,4)
					{
						// clamp coordinate, repeat boundary:
						int x = RR_MIN(to_x+bx,to->width-1);
						int y = RR_MIN(to_y+by,to->height-1);

						U8 * toPtr = rrSurface_Seek(to,x,y);

						memcpy(toPtr,blockPtr,bypp);
						blockPtr += bypp;
					}
				}
			}
			else
			{
				U8 * rows[4];
				for(int r=0;r<4;r++)
					rows[r] = rrSurface_Seek(to,to_x,to_y+r);

				copy_4x4_block_to_rows_advance(rows,blockPtr,bypp);
			}
		}
	}
	else
	{
		// do blocks -> blocks
		// for OodleTex_Layout_CopyBCNToLinear

		const U8 * blockPtr = from->blocks;
		
		int bypb = from->blockSizeBytes;
		RR_ASSERT( bypb == 8 || bypb == 16 );

		for (SINTa bi = 0; bi < layout->m_nblocks; ++bi)
		{
			const BlockLocation8 * loc = layout->m_block_ids + bi;
			if ( loc->surface_id_plus_1 == 0 )
			{
				// intentional null block
				blockPtr += bypb;
				continue;
			}

			rrSurface * to = to_surfaces + loc->surface_id_plus_1 - 1;

			// just assert not assert_always since layout creation does validate this
			int to_x = loc->x;
			int to_y = loc->y;

			if ( to_x < 0 )
			{
				// intentional null block
				blockPtr += bypb;
				continue;
			}

			RR_ASSERT( to_x >= 0 && to_x < to->width );
			RR_ASSERT( to_y >= 0 && to_y < to->height );

			U8 * to_ptr = rrSurface_Seek(to,to_x,to_y);
			
			//memcpy(to_ptr,blockPtr,bypb);
			
			RR_ASSERT( bypb == 8 || bypb == 16 );
			if ( bypb == 8 )
				memcpy(to_ptr,blockPtr,8);
			else
				memcpy(to_ptr,blockPtr,16);
			
			blockPtr += bypb;
		}
	}
}

int TotalBlockCount(const rrSurface * from,int num_surfaces)
{
	int block_count = 0;
	for LOOP(i,num_surfaces)
	{
		block_count += wh_to_num_blocks(from[i].width,from[i].height);
	}
	return block_count;
}

void BlockSurface_AllocCopy_from_RRS(BlockSurface* to,const rrSurface * from_array,int num_surfaces)
{
	THREADPROFILESCOPE("blksrfcpy");

	int block_count = TotalBlockCount(from_array,num_surfaces);
	
	BlockSurface_Alloc(to,block_count,from_array->pixelFormat);
	
	if ( rrPixelFormat_IsBlockCompressed(from_array->pixelFormat) )
	{		
		U8 * toPtr = to->blocks;

		for LOOP(i,num_surfaces)
		{
			RR_ASSERT( from_array[i].pixelFormat == from_array[0].pixelFormat );

			// assumes from stride is tight packed :
			//	@@ or copy row by row
			int nbx = (from_array[i].width + 3)/4;
			RR_ASSERT_ALWAYS( from_array[i].stride == nbx * to->blockSizeBytes );

			int cur_num_blocks = wh_to_num_blocks(from_array[i].width,from_array[i].height);
			SINTa cur_size_bytes = to->blockSizeBytes * cur_num_blocks;
			memcpy(toPtr,from_array[i].data,cur_size_bytes);
			toPtr += cur_size_bytes;
		}
		
		SINTa bytes = BlockSurface_GetDataSizeBytes(to);
		U8 * toEnd = to->blocks + bytes;
		RR_ASSERT_ALWAYS( toPtr == toEnd );
	}
	else
	{
		// do 4x4 -> rows
		
		U8 * toPtr = to->blocks;
		int bypp = rrPixelFormat_GetInfo(from_array[0].pixelFormat)->bytesPerPixel;
		RR_ASSERT( bypp >= 1 && bypp <= 16 );
		
		// @@ todo template switch on bypp
		// bypp is in [1,16]
		
		rrSurfaceRowCache fmRows;

		for LOOP(i,num_surfaces)
		{
			RR_ASSERT( from_array[i].pixelFormat == from_array[0].pixelFormat );

			const rrSurface * from = &from_array[i];
			
			int nbx = (from_array[i].width + 3)/4;

			fmRows.StartReadC(from,from->pixelFormat,8,4);
						
			// RowCache padding takes care of partial blocks for us
			
			// work on rows of blocks :
			for(int y=0;y<from->height;y+=4)
			{			
				fmRows.MoveCache(y,4);
			
				const U8 * rows[4];
				for(int r=0;r<4;r++)
					rows[r] = (U8 *) fmRows.GetRow(y + r);
						
				// for each block :
				for LOOP(bx,nbx)
				{
					copy_4x4_rows_to_block_advance(toPtr,rows,bypp);
					// advances toPtr
				}
			}
		}

		U8 * toEnd = to->blocks + to->count * to->blockSizeBytes;
		RR_ASSERT_ALWAYS( toPtr == toEnd );
	}	
	
}

void BlockSurface_AllocCopy_from_RRS_Layout(BlockSurface* to,const rrSurface * from_surfaces,int num_surfaces,const OodleTex_Layout * layout)
{
	if ( !layout )
	{
		BlockSurface_AllocCopy_from_RRS(to,from_surfaces,num_surfaces);
		return;
	}

	THREADPROFILESCOPE("blksrfcpy");

	RR_ASSERT( layout->m_surfaces.size32() == num_surfaces );

	RR_ASSERT_ALWAYS( ! rrPixelFormat_IsBlockCompressed(from_surfaces->pixelFormat) );

	// TODO int counts seem bad here
	BlockSurface_Alloc(to,check_value_cast<int>(layout->m_nblocks),from_surfaces->pixelFormat);

	U8 * toPtr = to->blocks;

	int bypp = rrPixelFormat_GetInfo(from_surfaces->pixelFormat)->bytesPerPixel;
	RR_ASSERT( bypp >= 1 && bypp <= 16 );

	RR_ASSERT( to->blockSizeBytes == bypp*16 );

	SINTa num_nulls = 0;
	for (SINTa bi = 0; bi < layout->m_nblocks; ++bi)
	{
		const BlockLocation8 * loc = layout->m_block_ids + bi;

		if ( loc->surface_id_plus_1 == 0 )
		{
			// intentional null block
			memset(toPtr,0,to->blockSizeBytes);
			toPtr += to->blockSizeBytes;
			num_nulls++;
			continue;
		}

		const rrSurface * from = from_surfaces + loc->surface_id_plus_1 - 1;

		// just assert not assert_always since layout creation does validate this
		S32 fm_x = loc->x;
		S32 fm_y = loc->y;
		RR_ASSERT( fm_x >= 0 && fm_x < from->width );
		RR_ASSERT( fm_y >= 0 && fm_y < from->height );

		// @@ todo template switch on bypp
		// bypp is in [1,16]

		// need to handle partial blocks here, don't have RowCache to help
		if ( fm_x+4 > from->width || fm_y+4 > from->height )
		{
			for LOOP(by,4)
			{
				for LOOP(bx,4)
				{
					// clamp coordinate, repeat boundary:
					int x = RR_MIN(fm_x+bx,from->width-1);
					int y = RR_MIN(fm_y+by,from->height-1);

					const U8 * fmPtr = rrSurface_SeekC(from,x,y);

					memcpy(toPtr,fmPtr,bypp);
					toPtr += bypp;
				}
			}
		}
		else
		{
			const U8 * rows[4];
			for(int r=0;r<4;r++)
				rows[r] = rrSurface_SeekC(from,fm_x,fm_y+r);

			copy_4x4_rows_to_block_advance(toPtr,rows,bypp);
		}
	}
}

RR_NAMESPACE_END
