// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#ifndef __RADRRBITMAP_DXTC_COMPRESS_HELP_H__
#define __RADRRBITMAP_DXTC_COMPRESS_HELP_H__

#include "rrcolor.h"
#include "rrdxtcblock.h"

// these only replace pBlock if pError improves :

RR_NAMESPACE_START

bool Compress_EndPoints(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2);
							
bool Compress_EndPointsQ_NoReverse(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor565Bits & c0,const rrColor565Bits & c1);
							
bool Compress_EndPoints_Force3C(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2);
							
U32 * Compress_TwoColorBest_AddEndPoints(U32 * pEndPoints,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2);
							
bool Compress_TwoColorBest(rrDXT1Block * pBlock,
							U32 * pError, const rrColorBlock4x4 & colors,rrDXT1PaletteMode mode,
							const rrColor32BGRA & end1,const rrColor32BGRA & end2);
						
// SingleColor is unconditional :							
void Compress_SingleColor_Compact(rrDXT1Block * dxtBlock,
							const rrColor32BGRA & c);

void Compress_SingleColor_Compact_3C(rrDXT1Block * dxtBlock,
							const rrColor32BGRA & c);
							
RR_NAMESPACE_END
							
#endif // __RADRRBITMAP_DXTC_COMPRESS_HELP_H__
