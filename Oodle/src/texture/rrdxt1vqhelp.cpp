// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4

#include "rrdxt1vqhelp.h"
#include "rrdxt1vqhelp.inl"
#include "templates/rralgorithm.h"

RR_NAMESPACE_START

void sort_dword_and_count_compare_count_highest_first(vector<dword_and_count> * pcounting)
{
	// sort counting by count :
	stdsort(pcounting->begin(),pcounting->end(),dword_and_count_compare_count_highest_first());
}

void sort_dword_and_count_compare_dword(vector<dword_and_count> * pcounting)
{
	// sort counting by count :
	stdsort(pcounting->begin(),pcounting->end(),dword_and_count_compare_dword());
}

void sort_and_count_uniques(vector<dword_and_count> * pcounting,vector<U32> & endpoints)
{			
	vector<dword_and_count> & counting = *pcounting;
	counting.clear();
	
	if ( endpoints.empty() )
		return;

	// sort & count endpoint use :
	stdsort(endpoints.begin(),endpoints.end());

	counting.reserve(endpoints.size());		
	counting.push_back();
	counting.back().dw = endpoints[0];
	counting.back().count = 1;
	
	for(int epi=1;epi<endpoints.size32();epi++)
	{
		U32 cur = endpoints[epi];
		if ( cur == counting.back().dw )
		{
			counting.back().count++;
		}
		else
		{
			counting.push_back();
			counting.back().dw = cur;
			counting.back().count = 1;
		}
	}
}		
		
void sort_bc7bits_and_count_compare_count_highest_first(vector<bc7bits_and_count> * pcounting)
{
	// sort counting by count :
	stdsort(pcounting->begin(),pcounting->end(),bc7bits_and_count_compare_count_highest_first());
}

void sort_bc7bits_and_count_compare_count_lowest_first(vector<bc7bits_and_count> * pcounting)
{
	// sort counting by count :
	stdsort(pcounting->begin(),pcounting->end(),bc7bits_and_count_compare_count_lowest_first());
}

void sort_bc7bits_and_count_compare_bc7bits(vector<bc7bits_and_count> * pcounting)
{
	// sort counting by count :
	stdsort(pcounting->begin(),pcounting->end(),bc7bits_and_count_compare_bc7bits());
}

void sort_and_count_uniques(vector<bc7bits_and_count> * pcounting,vector<bc7bits> & endpoints)
{			
	vector<bc7bits_and_count> & counting = *pcounting;
	counting.clear();
	if ( endpoints.empty() ) return;

	// sort & count endpoint use :
	stdsort(endpoints.begin(),endpoints.end());

	counting.reserve(endpoints.size());		
	counting.push_back();
	counting.back().val = endpoints[0];
	counting.back().count = 1;
	
	for(int epi=1;epi<endpoints.size32();epi++)
	{
		bc7bits cur = endpoints[epi];
		if ( cur == counting.back().val )
		{
			counting.back().count++;
		}
		else
		{
			counting.push_back();
			counting.back().val = cur;
			counting.back().count = 1;
		}
	}
}		
		
RR_NAMESPACE_END
