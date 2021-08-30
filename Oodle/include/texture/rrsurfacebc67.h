// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "rrsurface.h"
#include "rrdxtcenums.h"

RR_NAMESPACE_START

enum { BC6H_BlockSize = 16 };

// pTo points to 4*16 unsigned shorts that get filled with the half-float values
// 4 values per pixel: r, g, b, a in that order.
void BC6H_DecodeBlock(U16 * pTo, const void * from, bool isSigned);

enum { BC7_BlockSize = 16 };

struct rrColorBlock4x4RGBA;
void BC7_DecodeBlock(rrColorBlock4x4RGBA * pTo,const void * from);

struct BlockSurface;

rrbool rrSurfaceDXTC_DecompressBC6H(BlockSurface * to, const BlockSurface * from );
rrbool rrSurfaceDXTC_DecompressBC7(BlockSurface * to, const BlockSurface * from );

rrbool rrSurfaceDXTC_CompressBC6H(BlockSurface * to, const BlockSurface * from, bool isSigned, rrDXTCLevel level, rrDXTCOptions options);
rrbool rrSurfaceDXTC_CompressBC7(BlockSurface * to, const BlockSurface * from, rrDXTCLevel level, rrDXTCOptions options);

RR_NAMESPACE_END
