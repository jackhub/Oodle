// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "texbase.h"
#include "templates/rrvector.h"
#include "bc7bits.h"

RR_NAMESPACE_START

struct dword_and_count
{
	U32 dw;
	U32 count;
};

struct dword_and_count_compare_dword
{
	bool operator ()(const dword_and_count & lhs,const dword_and_count & rhs)
	{
		if ( lhs.dw == rhs.dw )
		{
			// if values the same, sort count to highest first
			return lhs.count > rhs.count;
		}
		return lhs.dw < rhs.dw;
	}
};

struct dword_and_count_compare_count_highest_first
{
	bool operator ()(const dword_and_count & lhs,const dword_and_count & rhs)
	{
		if ( lhs.count == rhs.count )
		{
			return lhs.dw < rhs.dw;
		}
		return lhs.count > rhs.count;
	}
};

struct dword_and_count_compare_count_lowest_first
{
	bool operator ()(const dword_and_count & lhs,const dword_and_count & rhs)
	{
		if ( lhs.count == rhs.count )
		{
			return lhs.dw < rhs.dw;
		}
		return lhs.count < rhs.count;
	}
};

// sort_and_count_uniques sorts "vec_of_dwords"
//  and fills "pcounting" with all unique dwords & their occurance count
// it does NOT sort pcounting
void sort_and_count_uniques(vector<dword_and_count> * pcounting,vector<U32> & vec_of_dwords);

void sort_dword_and_count_compare_count_highest_first(vector<dword_and_count> * pcounting);

void sort_dword_and_count_compare_dword(vector<dword_and_count> * pcounting);

//================================================================
// sixteen bytes (128 bits) (BC7)

struct bc7bits_and_count
{
	bc7bits val;
	U32 count;
};

struct bc7bits_and_count_compare_bc7bits
{
	bool operator ()(const bc7bits_and_count & lhs,const bc7bits_and_count & rhs)
	{
		if ( lhs.val == rhs.val )
		{
			// if values the same, sort count to highest first
			return lhs.count > rhs.count;
		}
		return lhs.val < rhs.val;
	}
};

struct bc7bits_and_count_compare_count_highest_first
{
	bool operator ()(const bc7bits_and_count & lhs,const bc7bits_and_count & rhs)
	{
		/*
		if ( lhs.count == rhs.count )
		{
			return lhs.val < rhs.val;
		}
		*/
		return lhs.count > rhs.count;
	}
};

struct bc7bits_and_count_compare_count_lowest_first
{
	bool operator ()(const bc7bits_and_count & lhs,const bc7bits_and_count & rhs)
	{
		/*
		if ( lhs.count == rhs.count )
		{
			return lhs.val < rhs.val;
		}
		*/
		return lhs.count < rhs.count;
	}
};

// sort_and_count_uniques sorts "vec_of_bc7bitss"
//  and fills "pcounting" with all unique bc7bitss & their occurance count
// it does NOT sort pcounting
void sort_and_count_uniques(vector<bc7bits_and_count> * pcounting,vector<bc7bits> & vec_of_bc7bitss);

void sort_bc7bits_and_count_compare_count_highest_first(vector<bc7bits_and_count> * pcounting);
void sort_bc7bits_and_count_compare_count_lowest_first(vector<bc7bits_and_count> * pcounting);

void sort_bc7bits_and_count_compare_bc7bits(vector<bc7bits_and_count> * pcounting);


RR_NAMESPACE_END
