// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

//idoc(parent,OodleAPI_Texture)
//idoc(end)
#include "texbase.h"
#include "cpux86.h"

#ifdef __RADSSE2__
#include "emmintrin.h"
#endif

OODLE_NS_START

OODLE_NS_END

//==================================================
// publicate the Oodle Texture header wrapper stuff :
// only for public header, so can be in cpp not our .h

PUBPUSH
PUBPRI(-10040)
#if 0
PUBTYPESTART

//===================================================
// Oodle2 Texture header
// (C) Copyright 1994-2021 Epic Games Tools LLC
//===================================================

#ifndef __OODLE2TEX_H_INCLUDED__
#define __OODLE2TEX_H_INCLUDED__

#ifndef OODLE2TEX_PUBLIC_HEADER
#define OODLE2TEX_PUBLIC_HEADER 1
#endif

#ifndef __OODLE2BASE_H_INCLUDED__
#include "oodle2base.h"
#endif

#ifdef _MSC_VER
#pragma pack(push, Oodle, 8)

#pragma warning(push)
#pragma warning(disable : 4127) // conditional is constant
#endif
PUBTYPEEND
#endif
#endif
PUBPOP

PUBPUSH
PUBPRI(1999)
#if 0
PUBSTART
#ifdef _MSC_VER
#pragma warning(pop)
#pragma pack(pop, Oodle)
#endif

#endif // __OODLE2TEX_H_INCLUDED__
PUBEND
PUBPOP

