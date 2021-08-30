// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

// @cdep pre $cbtargetsse4
// @cdep pre $cbtargetsse4

#include "rrdxt1vq.h"
#include "rrdxt1vqhelp.h"
#include "rrdxt1vqhelp.inl"
#include "rrdxtcblock.h"
#include "blocksurface.h"
#include "rrdxtcompresshelp.h"
#include "rrdxtcompresshelp.inl"
#include "rrdxtccompress.h"
#include "rrdxtccompress.inl"
#include "rrsurfacedxtc.h"
#include "rrvecc.h"
#include "rrcolorvecc.h"
#include "templates/rrvector.h"
#include "templates/rrhashtable.h"
#include "templates/rralgorithm.h"
#include "rrlogutil.h"
#include "log2table.h"
#include "perceptualactivity.h"
#include "perceptualactivity.inl"
#include "rrsurfacerowcache.h"

#include "newlz_simd.h"
#include "vec128.inl"

#ifdef DO_BUILD_SSE4
#include <smmintrin.h>
#endif

// 04-30-2020 : lambda normalization
//#define BC1RD_LAMBDA_SCALE (16.f/10.f) // before
// scale up by 5/4
//	to move lambda 40 to 50
//#define BC1RD_LAMBDA_SCALE 2.f
// too much
#define BC1RD_LAMBDA_SCALE (1.7f * OO2TEX_GLOBAL_LAMBDA_SCALE)
	
	
//#include "rrsurfaceblit.h"
//#include "../texutil/rrsurfacebyname.h"

//#include "rrsimpleprof.h"
#include "rrsimpleprofstub.h"

//===========================================

// $$$$ : space-quality tradeoffs

//===========================================

// size of endpoint palette to search in endpoints pass : $$$$

// note that endpoints vq palette gets the heuristic reduce ( < 1024 and often less)
//	this needs to be much bigger than for indices
//	because indices get a separate merger pass
//	endpoints relies entirely on this for its merging
//#define ENDPOINTS_VQ_PALETTE_SEARCH_LIMIT			9999 // no limit
#define ENDPOINTS_VQ_PALETTE_SEARCH_LIMIT			1024 // no limit
//#define ENDPOINTS_VQ_PALETTE_SEARCH_LIMIT			512

// with lz_window can this be smaller now?
//	-> nah still a pretty big penalty
#define MAX_NUM_ADDED_ENTRIES_TO_TRY_ENDPOINTS	256 
//#define MAX_NUM_ADDED_ENTRIES_TO_TRY_ENDPOINTS	128 

//===========================================

// size of index palette to search in index pass : $$$$

//#define INDEX_VQ_PALETTE_SEARCH_LIMIT			9999 // no limit
//#define INDEX_VQ_PALETTE_SEARCH_LIMIT			512
#define INDEX_VQ_PALETTE_SEARCH_LIMIT			256

//#define MAX_NUM_ADDED_ENTRIES_TO_TRY_INDICES	256 
#define MAX_NUM_ADDED_ENTRIES_TO_TRY_INDICES	128 

//===========================================

// early reject distances : $$$$

// !!WARNING!!
// because these are lambda-scaled, they are a real nice speedup at low lambda
//		not so much at high lambda
//		(we do get other speedups at high lambda, due to smaller vq palette sizes)
// also quality can be unchanged at high lambda, but affected at low lambda
//	the standard "total BPB saved at vcdiff +3"
//	  is at quite high lambda
//	so you won't see the affect there
// if you look at RD curves, you will see it at lower lambda!
// -> add a report at vcdiff +1 to see these

// EARLY_OUT_INDEX_DIFF
//	 a full index step from ep0 to ep1 is "6" (0-2-4-6 or 0-3-6)
//	 in 3c mode an index black is 13
// scaled by lamda : (lambda=10 is identity scaling)
//#define EARLY_OUT_INDEX_DIFF					24
//#define EARLY_OUT_INDEX_DIFF					14  // <- smallest value with epsilon quality impact
//#define EARLY_OUT_INDEX_DIFF					12
//#define EARLY_OUT_INDEX_DIFF					10  // <- starts to be a real big speedup here
// at very low lamda EARLY_OUT_INDEX_DIFF of 10 is a bit too low,
//	  it's hurting quality a bit
// what we can do is just add a consant to bump it up for low lambda :
//	
#define EARLY_OUT_INDEX_DIFF_TIMES_LAMBDA		9
#define EARLY_OUT_INDEX_DIFF_PLUS_CONSTANT		55
// at lambda= 10 that takes us to 145 which is the same as the "14" before (smallest value with epsilon quality impact)

// scaled by lamda :
//#define EARLY_OUT_COLOR_ENDPOINT_DISTANCE		200  // using bbox-dsqr and midpoint
#define EARLY_OUT_COLOR_ENDPOINT_DISTANCE		20	 // lambda went up 10X

#define EARLY_OUT_ENDPOINTS_SSD_MAX_INCREASE_SCALED_BY_LAMBDA	3
				
//===========================================

#define NUM_VQ_ITERATIONS	2

// we need 2 iterations
// 1st iteration is just for seeding
//	  it takes us from baseline to a decent candidate VQ encoding
// 2nd iteration basically throws away all the work of the 1st iteration
//    but it needs the 1st iteration for initial seeds & codelens

//===========================================


/**

If image is larger than 256 KB in dxt1 (512 K pixels = 512x1024 crops)
then cut it into chunks and just do them independently
this keeps N^2 VQ issues from getting out of control
and also maps to OodleLZ chunking

256 KB dxt1 = 512K pixels = 32K blocks

VQ reduce at least 4:1 = 8K palette
pretty damn big
too big to brute force search all 8K palette entries to find closest

==============

cost to send entry with count C is not like a log2(C/N) probability thing
the more important aspect is the (32 bits/C) part
that is you must send the raw value (32 bits)
but you only have to send that once
so the cost per use is 1/C

consider C=1 vs C=2
the log2(C) difference is only 1 bit difference
but 32/C is 16 bits difference

**/

RR_NAMESPACE_START

struct block_and_codelen
{
	rrDXT1Block	block;
	F32 codelen;
};

struct block_and_codelen_compare_codelen
{
	bool operator ()(const block_and_codelen & lhs,const block_and_codelen & rhs)
	{
		return lhs.codelen < rhs.codelen;
	}		
};

//===========================================

// hash U32 indices/endpoints -> F32 codelen
typedef RR_NAMESPACE::hash_table<U32,F32,hash_table_ops_mask31hash<U32> > t_hash_dw_to_codelen;
typedef RR_NAMESPACE::hash_table<U32,U32,hash_table_ops_mask31hash<U32> > t_hash_u32_to_u32;

struct rrDXT1_VQ_Block
{
	rrColorBlock4x4 colors;
	rrColor32BGRA color_bbox_lo;
	rrColor32BGRA color_bbox_hi;
	U8 block_has_any_transparency;
	U8 block_has_any_black3c;
	U8 block_is_degenerate_all_black_or_transparent;
	U8 pad;
	U32 block_1bt_mask;
	
	SingleFloatBlock4x4 activity;
	F32 block_activity_scale;
	
	rrDXT1Block baseline;
	F32 baseline_vqd;
	
	rrDXT1Block cur;
	rrDXT1Block second_best;
	F32 cur_vqd;
	F32 cur_J;
	int vq_entry_index;
	int vq_entry_link;
};

struct rrDXT1_VQ_Entry
{
	U32 dw;	// rrDXT1EndPoints endpoints;
	F32 codelen;
	
	int block_count;
	int block_link;
	
	// acceleration for endpoint vq :
	rrColor32BGRA palette[4];
};


struct vq_codelen_help
{
	F32 codelen_2count;
	//U32 codelen_singleton;
	F32 codelen_escape;
	
	//U32 sum_of_palette_counts;
	//U32 nblocks;
	//S32 nblocks_neglog2tabled;
	S32 codelen_denom;
};

#define DO_bc1_indices_vq_reduce_CHANGE_INDICES

static void bc1_indices_vq_reduce(vector<dword_and_count> * indices_out, const vector<dword_and_count> & indices_in, int lambda, int nblocks, rrDXT1_VQ_Block * blocks, rrDXT1PaletteMode pal_mode);

static void Optimize_Endpoints_For_Assigned_Blocks(
	vector<rrDXT1_VQ_Block> & blocks,
	vector<dword_and_count> & dcv,
	rrDXT1PaletteMode pal_mode);

static void Optimize_Indices_For_Assigned_Blocks(
	vector<rrDXT1_VQ_Block> & blocks,
	vector<dword_and_count> & dcv,
	rrDXT1PaletteMode pal_mode);
	

static RADFORCEINLINE F32 VQD(const rrColorBlock4x4 & colors,const rrDXT1Block & dxtb,const SingleFloatBlock4x4 & activity,rrDXT1PaletteMode pal_mode)
{
	// delta ignoring alpha difference
	rrColor32BGRA palette[4];
	DXT1_ComputePalette(dxtb.c0,dxtb.c1,palette,pal_mode);
	
	return VQD(colors,palette,dxtb.indices,activity);
}
	
// hash LZ J uses R ~ 16 * bits
//	this log2 is chosen to keep lambda scaling the same as it was
//	need to make sure (lambda * R) fits in an int as well
#define vq_codelen_one_bit_log2	(4)
#define vq_codelen_one_bit	(1<<vq_codelen_one_bit_log2)

// @@ vq_codelens need work of course

static F32 vq_J( F32 D, F32 R1,F32 R2, int lambdai )
{
	/*
		
	LZ scheme J is
		D + lambda * R
	D = VQD
	D ~ 28 * SAD
	D ~ 2 * SSD
	R ~ 16 * bits
		(R is "codelen" vq_codelen_one_bit)
	*/
	
	F32 R = R1+R2;

	F32 lambdaf = lambdai * (BC1RD_LAMBDA_SCALE/vq_codelen_one_bit);
	F32 J = D + lambdaf * R;

	// in terms of bits/VQD , lambda scale here is *2*

	return J;	
}

static F32 vq_J_10( F32 D, F32 R, int lambdai )
{
	//return vq_J(D,R1+R2,lambdai);
	
	F32 lambdaf = lambdai / 10.f;
	//F32 lambdaf = lambdai * (BC1RD_LAMBDA_SCALE/vq_codelen_one_bit);
	F32 J = D + lambdaf * R;
	return J;	
}

static F32 vq_J_to_R_codelen(F32 J, int lambdai)
{
	F32 lambdaf = lambdai / 10.f;
	//F32 lambdaf = lambdai * (BC1RD_LAMBDA_SCALE/vq_codelen_one_bit);
	// F32 J = lambdaf * R;
	return J / lambdaf;
}

static F32 vq_codelen_lz(int lzi)
{
	// lzi is in blocks	
	// lzi starts at 0, don't take log of zero
	RR_ASSERT( lzi >= 0 );
	
	// @@ REVISIT ME
	//	add a constant to lzi ?
	//	to decrease the severity of recency favoring?
	
	F32 offset_bits = 2*(F32)rrlog2_bk(lzi+2) + 1;
//	F32 bits = 9 + offset_bits;
	F32 bits = 10 + offset_bits;
	// 9 or 10 about the same
	return bits * vq_codelen_one_bit;
}

static F32 vq_codelen_palette_count( U32 count, const vq_codelen_help *phelp )
{
	// P = (count/nblocks)
	//	 is the probability of using this palette index, out of all possible blocks
	// P = (count/sum_of_palette_counts)
	//	 is the (higher) probability of using this palette index, out of all palette indices
	// P = (sum_of_palette_counts/nblocks)
	//	 is the probability of using a vq palette index (1 - escape probability)
	
	// using 
	// P = (count/nblocks)
	// is like first sending a no-escape flag
	//	and then selecting this index from sum_of_palette_counts
	
	// bits to use me are
	//  log2(P) to code my index (and to send the non-escape flag)
	// PLUS (32/count) to send my endpoints (spread over each use)
	
	// use count+1 to encourage us to get another count :
	//F32 P = (count+1)/(F32)phelp->nblocks;
	//F32 bits = -rrlog2(P);
	
	S32 bits_log2one = log2tabled_bk_32(count+1) - phelp->codelen_denom;
	F32 bits = bits_log2one * (vq_codelen_one_bit / (F32)RR_LOG2TABLE_ONE);
		
	// @@ ??
	// scale up selector codelen by log2(e) because LZ is worse than indices :
	//bits *= 1.5; 
	
	// add cost of sending the endpoint/index bits :
	// @@ ?? endpoints are actually slightly cheaper than 32 with entropy coding
	bits += (32.0f * vq_codelen_one_bit)/count; 
		// use (count+1) here ?
		//		becase we want to know the cost not as is, but if I coded one more entry in this slot
		//  -> no definitely not count+1 is much worse
		//	@@ but it's hard to play with tweaking this right now because it's also used for
		//	   the "novel" (count=2) and "escape" estimate
		//	   so things are not decoupled for tweakage
	
	//U32 codelen = (U32)( bits );
	return bits;
}

#if 0
static U32 vq_codelen_escape_count( U32 counts_singles, U32 counts_truncated, U32 nblocks )
{
	// cost to send an escape is
	// log2(P_escape)
	// PLUS 32 to send my endpoints
	
	// sum_of_palette_counts == counts_singles + counts_truncated
	
	// escape count is either counts_singles or (counts_singles + counts_truncated)
	
	// P(escape) will go *down* with each VQ pass
	//	it starts out quite high (lots of elements not in the VQ) and gets lower
	//	try to estimate what it is *after* this VQ pass
	
	U32 escape_count = 1 + counts_singles + (counts_truncated/2); // @@ ??
	F32 P = escape_count/(F32)nblocks;
	F32 bits = -rrlog2(P);
	bits += 32.0; // this dwarfs the log2(P) anyway
	
	U32 codelen = (U32)( bits * vq_codelen_one_bit );
	return codelen;
}
#endif

static void vq_do_initial_palette_reduction(vector<dword_and_count> & counting,int counts_all_unique_endpoints_count,int nblocks)
{
	// counts_all_unique_endpoints_count includes singletons that have already been removed
	RR_ASSERT( nblocks >= counts_all_unique_endpoints_count );
	RR_ASSERT( counts_all_unique_endpoints_count >= counting.size32() );

	#if 0
	// count = 1's have already been removed
	// maybe remove count=2 too ?
	// -> hurts

	RR_ASSERT( counting.back().count >= 2 );
	while ( ! counting.empty() && counting.back().count == 2 )
	{
		counting.pop_back();
	}
	#endif
			
	// this is sort of hacky heuristic
	// we just want to reduce it below the eventual target
	// because we want to force more clumping & add some using the kmeans++ style incremental adder below
	
	// @@ this should be a factor of "quality" ?
	//	 is this how the levels are set?
	
	// just reduce 8:1	
	// limits palette to 4K so we don't get too many things to look up
	int max_palette_size = nblocks/8; // <- this does not apply much pressure
	// almost always initial palette size is already smaller than this
	
	// in the first pass counts_all_unique_endpoints_count/2 rarely applies much pressure
	//	 because there are lots of singles
	// on the second pass this can apply a lot of pressure at high lambda
	//	 because we don't add many singletons, then here we do a 1/2 again
	max_palette_size = RR_MIN(max_palette_size,counts_all_unique_endpoints_count/2); // @@
	
	// @@ just don't do this?
	//max_palette_size = RR_MAX(max_palette_size,512); // not less than 512
	// @@ on tiny images this 512 gets hit and we don't work
	//	should just go to like 7/8 of counts_all_unique_endpoints_count or something there
	
	#if 0
	if ( max_palette_size < 512 )
	{
		max_palette_size = nblocks/8;
		max_palette_size = RR_MIN(max_palette_size,counts_all_unique_endpoints_count*3/4);
	}
	#endif
	
	//rrprintfvar(max_palette_size);
	
	if ( counting.size32() > max_palette_size )
	{
		// just cut off the tail of "counting"
		// @@ these are not necessarily the best blocks to cut
		//	could be better about sorting them by how much they help D
		
		// we're often cutting somewhere in the long count=2 tail
		//	we pick an arbitrary spot in there to cut
		//  it would be better to sort within each count
		// @@ could sort by how much J gain it provided vs 2nd best choice
		//	 (we want to keep the entries that are furthest from earlier entries)
		
		// what's the frequency at the cut point :
		//rrprintfvar(counting.size());
		//rrprintfvar(max_palette_size);
		//rrprintfvar(counting[max_palette_size].count);
		
		// @@ ? don't cut off high counts ?
		//	typically we should be cutting in the count=2,3 region		
		// the question is, when the count is 4 or whatever
		//	can we take those blocks and map them onto some other vq bucket
		//	and is that a good or bad move for current lambda? (is it a J gain?)
		#if 0
		while( counting[max_palette_size].count > 4 && max_palette_size < counting.size32() )
		{
			// @@ should this VQ slot be left separate, or can it be mapped onto another slot?
			//  if the blocks assigned to this slot were better J on another slot, they would have been mapped there already
			//	but mapping it over now is more favorable
			//	because the target slot will add on my count
			// say my count is 4
			//	there was some other slot with count 6
			//	when it was count 6, my blocks didn't want to go there, they chose me for the best J
			//	but after I map over his count will be 10 and now his codelen will be even more favorable
			max_palette_size++;
		}
		#endif
		
		counting.resize(max_palette_size);
	}
}

static F32 find_hash_codelen_or_escape(U32 dw,const t_hash_dw_to_codelen & hash,const vq_codelen_help * pcodelens)
{
	t_hash_dw_to_codelen::entry_ptrc ep = hash.find(dw);
	if ( ep )
		return ep->data();
	else
		return pcodelens->codelen_escape;
}
 
static inline bool indices_are_flat(U32 indices)
{
	return indices == RR_ROTL32(indices,2);
}

static void hacky_clamp_counts(vector<dword_and_count> * pcounting,int nblocks)
{
	// 05-19-2020 :
	// do this?
	// I don't have any real strong (visual) evidence showing this is positive
	//	but I do like it theoretically
	//	and I don't see any evidence against it (nor for it particularly)
	// in terms of vcdiff , numerically this hurts a tiny bit
	// it does reduce the number of flat blocks in the output
	//	so it is doing what I think it is
	// see : "count number of flat blocks in the output" in textest_bc1
	
	#if 0
	// don't do it
	return;
	#else
		
	// clamp any large counts
	//	to prevent run-away favoritism feedback
	//	say a common option like flat AAA
	//	has count = 10% of all blocks
	//	before going into vq
	//	then it will be more desirable because it has a lower codelen
	//	so it will become even more popular after each pass
	//
	// the idea of clamping counts here is to pretend it's not as common as it really is
	//	to prevent its codelen from being too attractive (relative to other options)
	//	so it has to be more competitive in D before being chosen
	//
	// I visually see the over-flattening on "red_blue"
	//	but the only big numeric effect is on "good_record_only"
	
	if ( pcounting->size() < 2 ) // degenerate
		return;
	
	// the only thing I've ever seen go over 4% are the flats (AAA and FFF)
	//	they can go to 20%
	U32 max_count = (nblocks * 50 + 999) / 1000; // 5.0%
	//U32 max_count = (nblocks * 40 + 999) / 1000; // 4.0%
	// more extreme clamps :
	//  -> in the end this hurts the numeric quality scores a tiny bit
	//	  and it's very hard to see the visual effect
	//		(does it reduce blocking?)
	//	  yes there's some effect per my eyeballs, is it better? hard to say
	//U32 max_count = (nblocks * 25 + 500) / 1000; // 2.5%
	//U32 max_count = (nblocks * 20 + 500) / 1000; // 2.0%
	
	if ( max_count < 5 ) return; // too small (same as having a min nblocks)
		
	vector<dword_and_count> & counting = *pcounting;
	
	RR_ASSERT( counting[0].dw != counting[1].dw );

	// high count first :
	RR_ASSERT( counting.front().count >= counting.back().count );
	
	for LOOPVEC(i,counting)
	{
		// sorted by count :
		if ( counting[i].count <= max_count )
			break;

		//textest bc1 -r30 R:\manyimages
		// log what gets clamped :
		// it's all AA's and FF's (1/3's and 2/3's)
		// AAAAAAAA : 3403 = 41.54%
		// AAAAAAAA : 1875 = 22.89%
		// FFFFFFFF : 499 = 6.09%
		//rrprintf("%08X : %d = %.2f%%\n",counting[i].dw,counting[i].count,counting[i].count*100.0/nblocks);

		counting[i].count = max_count;
	}
		
	#endif
}

enum EIndices
{
	eEndPoints = 0,
	eIndices = 1
};

enum EReduce
{
	eNoReduce=0,
	eReduce = 1
};
		
static void setup_palette_and_vq_codelen_help(vq_codelen_help * pcodelens,vector<dword_and_count> & counting,int nblocks,EIndices is_indices,EReduce do_initial_palette_reduction)
{
	// counting has the unique DWORDS & their count , but is not yet sorted by counted

	// sort counting by count :
	sort_dword_and_count_compare_count_highest_first(&counting);
	
	int counts_all_unique_endpoints_count = counting.size32();
	
	// should be all unique dws :
	RR_ASSERT( counting.size() < 2 || counting[0].dw != counting[1].dw );

	// high count first :
	RR_ASSERT( counting.front().count >= counting.back().count );
	
	// remove all the count=1 at the tail and call them escapes :
	RR_ASSERT( counting.back().count >= 1 );
	while ( ! counting.empty() && counting.back().count == 1 )
	{
		counting.pop_back();
	}
	
	int counts_num_singles_removed = counts_all_unique_endpoints_count - counting.size32();
	counts_num_singles_removed;
				
	if ( counting.size() > 2 )
	{
		if ( is_indices )
		{
			hacky_clamp_counts(&counting,nblocks);
		}
		
		if ( do_initial_palette_reduction )
		{
			// limit VQ palette size :
			RR_ASSERT( ! is_indices );
		
			vq_do_initial_palette_reduction(counting,counts_all_unique_endpoints_count,nblocks);
		}
	}
	
	/*
	// sum of counts that made it into the vq palette
	//	if nothing was removed (no singles) this would == nblocks
	U32 counts_sum_after = 0;
	for LOOPVEC(i,counting)
		counts_sum_after += counting[i].count;
	RR_ASSERT( counts_sum_after <= (U32)nblocks );
	/**/
		
	/*
	// look at the distribution :
	// it's very peaky
	// just a few with high count
	// then lots of 2s and 3s
	// (could make this a more compact log with a count of counts)
	rrprintf("setup_palette_and_vq_codelen_help (%s) (%s)\n",
		is_indices ? "indices" : "endpoints",
		do_initial_palette_reduction ? "reduce" : "no reduce" );
	for LOOPVEC(i,counting)
	{
		rrprintf("%08X : %d = %.2f%%\n",counting[i].dw,counting[i].count,counting[i].count*100.0/nblocks);
		if ( i == 20 ) break;
	}
	/**/
	
	//pcodelens->nblocks = nblocks;
	
	// what's the denominator for codelen in log2(count/denom) ?
	//	is it total # of blocks? or only the ones that made it into the VQ palette?
	// -> doesn't seem to matter much
	//	-> this is essentially just a constant that's added to all the J's
	//		so it really should completely factor out
	//		some exception to that since we compare to "lz" codelen
	U32 total_for_codelen_denom = nblocks;
	//U32 total_for_codelen_denom = nblocks - counts_num_singles_removed;
	//U32 total_for_codelen_denom = counts_sum_after;
	pcodelens->codelen_denom = log2tabled_bk_32(total_for_codelen_denom);


	// make codelens for rate estimate
	// pretend we are statistical coding the palette selection
	
	pcodelens->codelen_2count = vq_codelen_palette_count( 2,pcodelens );
	
	F32 codelen_singleton = vq_codelen_palette_count( 1,pcodelens );
		
	pcodelens->codelen_escape = codelen_singleton;	
}

static void update_mtf_window(int new_val,int * lz_window, int & lz_window_size, int max_lz_window_size)
{
	RR_ASSERT( lz_window_size <= max_lz_window_size );

	// is it already in lz_window ?
	int lz_found_i = -1;
	for LOOP(lz_i,lz_window_size)
	{
		if ( lz_window[lz_i] == new_val )
		{
			lz_found_i = lz_i;
			break;
		}
	}
	
	if ( lz_found_i < 0 )
	{
		// add it :
		if ( lz_window_size < max_lz_window_size )
			lz_window_size++;
		
		lz_found_i = lz_window_size-1;
	}

	// MTF :
	memmove(lz_window+1,lz_window,lz_found_i*sizeof(lz_window[0]));
	lz_window[0] = new_val;
}
				
struct block_final_state
{
	F32 cur_J;
	F32 min_vqd;
	F32 maximum_codelen;
	U32 must_beat_sad;
};

bool rrDXT1_VQ_Single(BlockSurface * to_blocks,
	const BlockSurface * from_blocks,
	const BlockSurface * baseline_blocks,
	const BlockSurface * activity_blocks,
	int lambda,
	rrDXTCOptions options)
{
	SIMPLEPROFILE_SCOPE(vq_single);
				
	int nblocks = to_blocks->count;
	
	
	RR_ASSERT( to_blocks->pixelFormat == rrPixelFormat_BC1 || to_blocks->pixelFormat == rrPixelFormat_BC2 || to_blocks->pixelFormat == rrPixelFormat_BC3 );
	RR_ASSERT( baseline_blocks->pixelFormat == to_blocks->pixelFormat );

	// For BC1, we always ignore alpha
	// BC2 and BC3 are always in four-color mode
	rrDXT1PaletteMode pal_mode = ( to_blocks->pixelFormat == rrPixelFormat_BC1 ) ? rrDXT1PaletteMode_NoAlpha : rrDXT1PaletteMode_FourColor;
	
	if ( options & rrDXTCOptions_BC1_OneBitAlpha )
	{
		RR_ASSERT( to_blocks->pixelFormat == rrPixelFormat_BC1 );
		pal_mode = rrDXT1PaletteMode_Alpha;
	}

	RR_ASSERT( nblocks <= 32*1024 );
	
	vector<rrDXT1_VQ_Block> blocks;
	blocks.resize(nblocks);
	
	vector<dword_and_count> endpoint_counting;
	endpoint_counting.reserve(nblocks);
		
	vector<dword_and_count> index_counting;
	index_counting.reserve(nblocks);
	
	vector<dword_and_count> dwc_scratch;
	dwc_scratch.reserve(nblocks);
	
	vector<U32> endpoints;
	endpoints.resize(nblocks);
			
	vector<U32> indices;
	indices.resize(nblocks);
		
	// read baseline encoding to seed :
	{		
		/*
		double cur_total_vqd = 0;
		double cur_total_ssd = 0;
		double cur_total_sad = 0;
		*/
		
		for LOOP(bi,nblocks)
		{
			rrDXT1_VQ_Block * pblock = &blocks[bi];
		
			const U8 * fmPtr = BlockSurface_SeekC(from_blocks,bi);
			RR_ASSERT( from_blocks->pixelFormat == rrPixelFormat_R8G8B8A8 );
		
			rrColorBlock4x4 & colors = pblock->colors;
			memcpy(&colors,fmPtr,sizeof(rrColorBlock4x4));
			SwapRB(&colors);
			
			bool block_has_any_transparency = false;
			U32 block_1bt_mask = 0;

			if ( pal_mode == rrDXT1PaletteMode_Alpha )
			{
				// this is done by rrSurfaceDXTC_CompressBC1 in the non-RDO path too
				
				int num_transparent = 0;

				// Canonicalize, then you can just use RGBA deltas
				//	no need for the special alpha-aware error metric (still using that at the moment though)
				for LOOP(i,16)
				{
					rrColor32BGRA_MakeOneBitTransparentCanonical(&colors.colors[i]);

					if ( colors.colors[i].dw == 0 )
					{
						num_transparent++;
					}
				}
				
				block_1bt_mask = DXT1_OneBitTransparent_Mask(colors);

				block_has_any_transparency = num_transparent > 0;

				RR_ASSERT( block_has_any_transparency == (block_1bt_mask != 0) );

				if ( num_transparent == 16 )
				{
					// @@ degenerate
					// store all FF to block and do nothing else
					// -> that should have already been done in baseline
					// this will also trigger num_nonblack_colors == 0 below

					// at the moment we still need to process this block
					//	to get it counted, or we hit other degeneracies elsewhere
					// -> maybe fix that so we're better at earlying out on degenerate blocks
					//	such as solid color / all black
				}
			}
			else
			{
				// ensure all A's are 255 here :
				//	this lets us do RGBA SSD later and not worry about A :
				KillAlpha(colors);
			}
			
			RR_ASSERT( rrColorBlock4x4_IsBC1Canonical(colors,pal_mode) );

			{
				// find block color bbox :
				rrColor32BGRA loC;
				loC.u.b = loC.u.g = loC.u.r = 255;
				rrColor32BGRA hiC; 
				hiC.dw = 0;
				rrColor32BGRA hiC_all;
				hiC_all.dw = 0;

				int num_nonblack_colors = 0;

				for(int i=0;i<16;i++)
				{
					const rrColor32BGRA & c = colors.colors[i];
					//avg += ColorToVec3i( c );
					
					if ( c.u.r < 12 && c.u.g < 12 && c.u.b < 12 &&
						pal_mode != rrDXT1PaletteMode_FourColor ) // BLACKNESS_DISTANCE
					{
						// near black
						// transparent comes in here too
						
						hiC_all.u.b = RR_MAX(hiC_all.u.b,c.u.b);
						hiC_all.u.g = RR_MAX(hiC_all.u.g,c.u.g);
						hiC_all.u.r = RR_MAX(hiC_all.u.r,c.u.r);
					}
					else
					{
						// rgb bbox of the non-black/alpha colors
						RR_ASSERT( c.u.a == 255 );
						num_nonblack_colors++;

						hiC.u.b = RR_MAX(hiC.u.b,c.u.b);
						hiC.u.g = RR_MAX(hiC.u.g,c.u.g);
						hiC.u.r = RR_MAX(hiC.u.r,c.u.r);
						loC.u.b = RR_MIN(loC.u.b,c.u.b);
						loC.u.g = RR_MIN(loC.u.g,c.u.g);
						loC.u.r = RR_MIN(loC.u.r,c.u.r);
					}
				}
				
				if ( num_nonblack_colors == 0 )
				{
					// loC/hiC not set
					// use the black region bbox instead
					loC.dw = 0;
					hiC = hiC_all;					

					// if hiC == 0,0,0 , total degen, all black
				}
				else
				{
					hiC_all.u.b = RR_MAX(hiC.u.b,hiC_all.u.b);
					hiC_all.u.g = RR_MAX(hiC.u.g,hiC_all.u.g);
					hiC_all.u.r = RR_MAX(hiC.u.r,hiC_all.u.r);
				}

				// color_bbox_is only the RGB part :
				loC.u.a = 255;
				hiC.u.a = 255;
				pblock->color_bbox_lo = loC;
				pblock->color_bbox_hi = hiC;
			
				// in FourColor mode, num_nonblack_colors is always 16
				pblock->block_has_any_transparency = block_has_any_transparency;
				pblock->block_has_any_black3c = num_nonblack_colors < 16;
				pblock->block_is_degenerate_all_black_or_transparent = ( hiC_all.dw == 0 );
				// block_is_degenerate_all_black_or_transparent can still have non-trivial coding if it's black/alpha
				//	-> r:\black_a_512.tga
				
				pblock->block_1bt_mask = block_1bt_mask;

				// if block_has_any_transparency, then block_has_any_black will be on too
				RR_ASSERT( pblock->block_has_any_black3c || ! block_has_any_transparency );
			}
			
			// read block from input surface :
			rrDXT1Block & block = pblock->baseline;
			const U8 * baselinePtr = BlockSurface_SeekC(baseline_blocks,bi);
			//rrDXT1_GetBlock(bc1RowPtr,&block);
			memcpy(&block,baselinePtr,sizeof(block));
			
			const U8 * activityPtr = BlockSurface_SeekC(activity_blocks,bi);
			RR_ASSERT( activity_blocks->pixelFormat == rrPixelFormat_1_F32 );
			memcpy(&(pblock->activity),activityPtr,sizeof(SingleFloatBlock4x4));
			
			pblock->baseline_vqd = VQD(colors,block,pblock->activity,pal_mode);
			//rrCompressDXT1_2( &block, &pblock->baseline_ssd, colors, rrDXTCOptions_None, pal_mode );
		
			pblock->cur = pblock->baseline;
			pblock->cur_vqd = pblock->baseline_vqd;
			pblock->cur_J = 0.f;
		
			pblock->second_best = pblock->cur;
			
			if ( pal_mode == rrDXT1PaletteMode_Alpha )
				RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );
			
			//bc1RowPtr += sizeof(rrDXT1Block);
		
			// ? swap endpoints to 4 color order? so initial seeds are only 4-color order?
			//	-> no much worse on stuff like frymire
			//	could also reject c0 == c1 degenerate endpoints
			//	-> baseline never has degenerate endpoints
			//	  it also never has indices = ep0 or ep1 (000 or 555)
			//	  but those can all arise after RD
		
			#if 0

			U32 ssd = DXT1_ComputeSSD_RGB(colors,block,pal_mode);
			U32 sad = DXT1_ComputeSAD_RGB(colors,block,pal_mode);

			cur_total_vqd += pblock->baseline_vqd;
			cur_total_ssd += ssd;
			cur_total_sad += sad;
			
			#endif				
		
			endpoints[bi] = rrDXT1Block_EndPoints_AsU32(block);
			indices[bi] = block.indices;
		}
		
		/*
		{
			F32 reference = 2 * cur_total_ssd;
			F32 cur_sad_scale = reference / cur_total_sad;
			F32 cur_vqd_scale = reference / cur_total_vqd;
			rrprintfvar(cur_sad_scale);
			rrprintfvar(cur_vqd_scale);
		}
		
		g_total_vqd += cur_total_vqd;
		g_total_ssd += cur_total_ssd;
		g_total_sad += cur_total_sad;
		log_total_vqd();
		*/
		
		sort_and_count_uniques(&endpoint_counting,endpoints);
		sort_and_count_uniques(&index_counting,indices);				
	}
		
	t_hash_dw_to_codelen dw_to_codelen_hash;
	dw_to_codelen_hash.reserve_initial_size(nblocks);
	
	t_hash_u32_to_u32 dw_to_index_hash;
	dw_to_index_hash.reserve_initial_size(nblocks);
	
	vector<rrDXT1_VQ_Entry> vqendpoints;
	vqendpoints.reserve( nblocks );
				
	vector<rrDXT1_VQ_Entry> vqindices;
	vqindices.reserve( nblocks );
	
	{
	SIMPLEPROFILE_SCOPE(vq_iters);
	
	for LOOP(vq_iterations,NUM_VQ_ITERATIONS)
	{
		//=================================================
		// first endpoint loop
		
		// endpoint_counting & index_counting is carried from last iteration
		
		// make index codelen hash table for endpoint loop :
		
		vq_codelen_help index_codelen_help;
		setup_palette_and_vq_codelen_help(&index_codelen_help,index_counting,nblocks,eIndices,eNoReduce);
		
		dw_to_codelen_hash.clear();
		dw_to_codelen_hash.reserve(index_counting.size32());
		
		RR_ASSERT( index_counting.size() < 2 || index_counting[0].count >= index_counting[1].count );
		
		//U32 cheapest_index_codelen = vq_codelen_palette_count( index_counting[0].count,&index_codelen_help );
			
		for LOOPVEC(i,index_counting)
		{
			F32 codelen = vq_codelen_palette_count( index_counting[i].count,&index_codelen_help );
			U32 dw = index_counting[i].dw;
			RR_ASSERT( dw_to_codelen_hash.find(dw) == NULL );
			dw_to_codelen_hash.insert( dw, codelen );
		}		
		
		//=================================================
		// now make endpoint initial VQ Table :	
			
		vq_codelen_help endpoint_codelen_help;
		// @@ setup_initial forces reduction :
		setup_palette_and_vq_codelen_help(&endpoint_codelen_help,endpoint_counting,nblocks,eEndPoints,eReduce);
			
		// make VQ palette
		// @@ with accelerations to help evaluate block error quickly
		//	could pre-compute dxt1 palettes in simd vectors for example

		vqendpoints.clear();
		vqendpoints.resize( endpoint_counting.size() );
		
		dw_to_index_hash.clear();
		dw_to_index_hash.reserve( endpoint_counting.size32() );
		
		for LOOPVEC(i,endpoint_counting)
		{
			rrDXT1_VQ_Entry & vqp = vqendpoints[i];
			vqp.dw = endpoint_counting[i].dw;
			
			RR_ASSERT( dw_to_index_hash.find(vqp.dw) == NULL );
			dw_to_index_hash.insert( vqp.dw, i );
			
			rrDXT1EndPoints ep;
			ep.dw = vqp.dw;
			DXT1_ComputePalette(ep.u.c0,ep.u.c1,vqp.palette,pal_mode);
			vqp.codelen = vq_codelen_palette_count( endpoint_counting[i].count,&endpoint_codelen_help );
			
			vqp.block_link = -1;
			vqp.block_count = 0;
		}
		
		{
		SIMPLEPROFILE_SCOPE(endpoint_pass);
		
		int endpoint_vq_palette_size_before = vqendpoints.size32(); // == endpoint_counting.size()
		//rrprintfvar(endpoint_vq_palette_size_before);
		RR_ASSERT( endpoint_counting.size() <= 4*1024 );

		int endpoint_vq_palette_num_to_search = RR_MIN(endpoint_vq_palette_size_before,ENDPOINTS_VQ_PALETTE_SEARCH_LIMIT);

		int lz_window[MAX_NUM_ADDED_ENTRIES_TO_TRY_ENDPOINTS];
		int lz_window_size = 0;
		
		for LOOPVEC(i,blocks)
		{
			// try block with something from VQ vqendpoints
			rrDXT1_VQ_Block * pblock = &blocks[i];
			const rrColorBlock4x4 & colors = pblock->colors;
			
			U32 best_ssd = RR_DXTC_ERROR_BIG;
			F32 best_vqd = (F32)RR_DXTC_ERROR_BIG;
			F32 best_J = (F32)RR_DXTC_ERROR_BIG;
			int best_p = -1;			
			rrDXT1Block best_block;
			rrDXT1Block best_block2; // save 2nd best choice for index loop
			// seed best_block with something valid :
			best_block2 = best_block = pblock->baseline;
			
			rrDXT1EndPoints baseline_ep;
			baseline_ep.dw = pblock->baseline.endpoints;
			rrColor32BGRA baseline_palette[4];
			DXT1_ComputePalette(baseline_ep.u.c0,baseline_ep.u.c1,baseline_palette,pal_mode);
								
			// this is the very slow N*M loop : (N blocks * M codebook entries)
			//		(which is N^2 because M is proportional to N)
			// this is basically just doing FindIndices & VQD over and over again
			//	@@ could speed this up a lot
			//	  current block colors[] could be pre-loaded into SIMD vectors
			//	  the palette for each endpoints[] in the set could be prepped
			
			int search_count = endpoint_vq_palette_num_to_search + lz_window_size;
			
			#if 1
			// if block_is_degenerate_all_black_or_transparent skip?
			//	-> yes seems fine
			if ( pblock->block_is_degenerate_all_black_or_transparent )
			{
				// don't continue, still need to add it to block linked list?
				//	continue;
				search_count = 0;
			}
			#endif

			for LOOP(search_i,search_count)
			{
				int vqp;
				if ( search_i < endpoint_vq_palette_num_to_search )
				{
					vqp = search_i;
				}
				else
				{
					int lzi = search_i-endpoint_vq_palette_num_to_search;
					RR_ASSERT( lzi < lz_window_size );
					vqp = lz_window[lzi];
				}
				
				rrDXT1_VQ_Entry & vqendpoint = vqendpoints[vqp];
								
				// lower palette index = higher count = preferable
				
				bool vqendpoint_is4c = DXT1_Is4Color(vqendpoint.dw,pal_mode);

				// transparency -> needs 3c
				if ( pblock->block_has_any_transparency && vqendpoint_is4c )
					continue;
					
				#if 0
				// demand 3c if block has black? -> no, hurts
				if ( pblock->block_has_any_black3c && vqendpoint_is4c )
					continue;
				#endif

				#if 0
				{
				// early out?
				// if using J count and an SSD of zero could not beat current J for this count
				//	then break out
				//	because vqendpoints is sorted by codelen,
				//	 codelen is going up
				//	(how often does that happen? is there speed benefit
				//		-> no this doesn't help at all
			
				// in theory indexes could be the cheapest possible :
				U32 index_codelen = cheapest_index_codelen;
				U32 best_possible_J = vq_J(0,vqendpoint.codelen,index_codelen,lambda);
				if ( best_possible_J >= best_J )
				{
					break;
				}
				}
				#endif
				
//total BPB saved at vcdiff +3.0 : [ 1.941 ] 2.7578 s

				#if 0 // old way
//total BPB saved at vcdiff +3.0 : [ 1.942 ] 2.1785 s

				// early out reject p if its color bbox is way wrong
				// pretty good speedup possible (41s -> 25s), some quality penalty
				{
				// currently judging "close" by distance of vq endpoints to baseline endpoints
				// palette [0,1] are the endpoints
				// @@ could be better way to check "close"
				//	use block color bbox instead of baseline endpoints?
				//	check distance of vq endpoints to color bbox
				//	  also need to check the midpoint of vq endpoints
				//		(being out of color bbox is okay sometimes if midpoint is in)
				U32 d00 = rrColor32BGRA_DeltaSqrRGB(baseline_palette[0],vqendpoint.palette[0]);
				U32 d10 = rrColor32BGRA_DeltaSqrRGB(baseline_palette[1],vqendpoint.palette[0]);
				
				U32 d01 = rrColor32BGRA_DeltaSqrRGB(baseline_palette[0],vqendpoint.palette[1]);
				U32 d11 = rrColor32BGRA_DeltaSqrRGB(baseline_palette[1],vqendpoint.palette[1]);				
				
				// vq endpoints one or the other must be close to baseline
				U32 d0 = RR_MIN(d00,d10);
				U32 d1 = RR_MIN(d01,d11);
				U32 d = RR_MIN(d0,d1);
				U32 min_d = 1024*lambda/10; // this is okay for quality, does it help speed at all?
				if ( d > min_d )
					continue;
				}
				#endif
				
				#if 1 // new way
				// reject endpoints vs color bbox :

				// bbox way is not significantly better than prior baseline endpoint distance way
				//	(and it's a bit slower to compute)
				// but I do like it better conceptually so I'm going with it for now
				//	@@ for an early out this is really rather slow to compute
				
				#if 0 //def __RADSSE2__
				// TEMP check
				RR_ASSERT( reject_endpoints_color_bbox_sse2(vqendpoint.palette,&(pblock->color_bbox_lo),EARLY_OUT_COLOR_ENDPOINT_DISTANCE*lambda)
					== reject_endpoints_color_bbox_scalar(vqendpoint.palette,&(pblock->color_bbox_lo),EARLY_OUT_COLOR_ENDPOINT_DISTANCE*lambda) );
				#endif

				// palette[0],[1] are endpoints
				//	color_bbox is only the RGB non-black part

				if ( reject_endpoints_color_bbox(vqendpoint.palette,&(pblock->color_bbox_lo),EARLY_OUT_COLOR_ENDPOINT_DISTANCE*lambda) )
				{
					continue;
				}	
				#endif

				// find indices for these endpoints :	
				// just makes new indices for these endpoints
				//	 this gives us the minimum possible ssd for these endpoints on this block
				//	 (we will later check cur.indices too)
				U32 ssd;
				U32 new_indices = DXT1_FindIndices(colors,vqendpoint.palette,pal_mode,&ssd);

				// favor lower palette indeces at equal ssd
				//	with index_codelen you can't do this (and minimize J)
				//	endpoint codelen is strictly going up, but index codelen can go all over
				// @@ could be useful as an early out acceleration?
				//	hurts vcdiff a little, helps rmse, what's the speed effect?
				//	 -> yes helps speed quite a lot
				if ( ssd < best_ssd + EARLY_OUT_ENDPOINTS_SSD_MAX_INCREASE_SCALED_BY_LAMBDA*lambda )
				{					
					// in this loop we measure cost of indices
					//	so we will favor repeated indices if we chance upon them
					//	but we don't actively look for them
					F32 index_codelen = find_hash_codelen_or_escape(new_indices,dw_to_codelen_hash,&index_codelen_help );
				
					/*
					if ( new_indices == 0 || new_indices == 0x55555555 )
					{
						index_codelen = index_codelen_help.codelen_escape;
						index_codelen += vq_codelen_one_bit;
					}
					*/
				
					rrDXT1Block block;
					block.indices = new_indices;
					block.endpoints = vqendpoint.dw;;
					
					F32 vqd = VQD(colors,vqendpoint.palette,new_indices,pblock->activity);
				
					F32 endpoint_codelen = vqendpoint.codelen;
				
					#if 1
					// if in the lz_window region :
					//	endpoint codelen from lz offset instead of frequency ?
				
					if ( search_i >= endpoint_vq_palette_num_to_search )
					{
						int lzi = search_i-endpoint_vq_palette_num_to_search;
						F32 lz_codelen = vq_codelen_lz(lzi);
						endpoint_codelen = RR_MIN(lz_codelen,endpoint_codelen);
					}
					#endif
					
					F32 J = vq_J(vqd,endpoint_codelen,index_codelen,lambda);
					
					// force strictly decreasing vqd ?
					//	-> quite good for red_blue and quite bad for good_record
					//	-> overall meh
					//if ( J < best_J && vqd < best_vqd )
					if ( J < best_J )
					{
						best_J = J;			
						best_vqd = vqd;
						best_block2 = best_block;
						best_block = block;
						best_p = vqp;

						// ssd doesn't necessarily go down when J does, so MIN here :
						// we intentionally set best_ssd here only when we get a good J block
						// not any time we see a lower SSD
						// if we did that, we could make best_ssd too low for any block to qualify
						//	without ever getting a J we liked
						best_ssd = RR_MIN(best_ssd,ssd);
					}
					
					if ( vq_iterations > 0 ) // second iter only
					{
						// see if cur indices from last iteration can be retained
						//
						// -> yes helps a little
						//	mainly on wood_worn and good_record_only
						//	near nop on the rest
					
						U32 cur_indices = pblock->cur.indices;
					
						if ( cur_indices != new_indices )
						{
							// compared to "new_indices" we should see a lower index_codelen and higher vqd
 						
							index_codelen = find_hash_codelen_or_escape(cur_indices,dw_to_codelen_hash,&index_codelen_help );
					
							block.indices = cur_indices;
							//block.endpoints unchanged

							vqd = VQD(colors,vqendpoint.palette,cur_indices,pblock->activity);

							J = vq_J(vqd,endpoint_codelen,index_codelen,lambda);
						
							if ( J < best_J )
							{
								best_J = J;			
								best_vqd = vqd;
								best_block2 = best_block;
								best_block = block;
								best_p = vqp;

								best_ssd = RR_MIN(best_ssd,ssd);
							}
						}					
					}
				}
			}
			
			// consider sending an escape & baseline encoding instead
								
			rrDXT1Block candidates[2] = { pblock->baseline };
			F32 candidates_vqd[2] = { pblock->baseline_vqd };
			int ncandidates = 1;
			if ( pblock->cur != pblock->baseline )
			{
				// 1st iter, cur is == baseline and this is dupe work

				//RR_ASSERT( pblock->cur_vqd == VQD(pblock->colors,pblock->cur,pblock->activity) );
				// not up to date :
				pblock->cur_vqd = VQD(pblock->colors,pblock->cur,pblock->activity,pal_mode);

				candidates[ncandidates] = pblock->cur;
				candidates_vqd[ncandidates] = pblock->cur_vqd;		
				ncandidates++;
			}
			
			for LOOP(candidate_i,ncandidates)
			{
				rrDXT1Block candidate = candidates[candidate_i];
				F32 candidate_vqd = candidates_vqd[candidate_i];

				// big vcdiff gain to NOT check this :			
				//if ( candidate_vqd < best_vqd ) // almost always true (for baseline, but NOT for cur) remove me?
				{
					// assume baseline endpoints are an escape
					//	if they're not, then it should have been found in the vq loop above
					
					U32 cand_endpoints = candidate.endpoints;
					F32 endpoints_codelen = endpoint_codelen_help.codelen_escape;
				
					#if 1
					// check if baseline is in vqendpoints ?
					// see if cur is in palette to get a cheaper codelen, if it's not really an escape
					// -> this is almost a NOP for endpoints because we do full search of vqendpoints already
					//	(unlike indexes where it is a solid help)
					
					int endpoints_found_p = -1;
					
					/*
					for LOOPVEC(pp,vqendpoints)
					{
						if ( vqendpoints[pp].dw == endpoints )
						{
							endpoints_codelen = vqendpoints[pp].codelen;
							endpoints_found_p = pp;
							break;
						}
					}
					*/
					
					{
						t_hash_u32_to_u32::entry_ptrc ep = dw_to_index_hash.find(cand_endpoints);
						if ( ep != NULL )
						{
							endpoints_found_p = ep->data();
							endpoints_codelen = vqendpoints[endpoints_found_p].codelen;
							RR_ASSERT( vqendpoints[endpoints_found_p].dw == cand_endpoints );
						}
					}
					
					#endif
				
					// J eval :
					F32 index_codelen = find_hash_codelen_or_escape(candidate.indices,dw_to_codelen_hash,&index_codelen_help );
						
					F32 J = vq_J(candidate_vqd,endpoints_codelen,index_codelen,lambda);
					
					if ( J < best_J )
					{
						if ( endpoints_found_p < 0 )
						{
							// add to palette :
							vqendpoints.push_back();			
							best_p = vqendpoints.size32()-1;
							
							rrDXT1_VQ_Entry & vqp = vqendpoints.back();
							//vqp.endpoint_codelen = endpoint_codelen_singleton;
							vqp.codelen = endpoint_codelen_help.codelen_2count;
							// endpoint_codelen as if I have a 2 count
							//	since someone using me again would make my count at least 2
							//	this makes it more desirable to reuse me than to send another escape
							vqp.dw = cand_endpoints;
							DXT1_ComputePalette(cand_endpoints,vqp.palette,pal_mode);
							vqp.block_link = -1;
							vqp.block_count = 0;		
							
							RR_ASSERT( dw_to_index_hash.find(cand_endpoints) == NULL );
							dw_to_index_hash.insert(cand_endpoints,best_p);	
						}
						else
						{
							best_p = endpoints_found_p;
						}
						
						best_J = J;						
						best_block2 = best_block;
						best_block = candidate;
						best_vqd = candidate_vqd;
					}
				}
			}
			
			// @@ TODO :
			//	record the top N choices (maybe just N = 2)
			//	 rather than just one choice
			//  for use in the index VQ so it has endpoint options
			
			RR_ASSERT( best_block.endpoints == vqendpoints[best_p].dw );
			pblock->cur = best_block;
			pblock->second_best = best_block2;
			pblock->cur_vqd = best_vqd;
			pblock->cur_J = best_J;
			pblock->vq_entry_index = best_p;
			pblock->vq_entry_link = vqendpoints[best_p].block_link;
			vqendpoints[best_p].block_link = i;
			vqendpoints[best_p].block_count ++;
			
			#if 0
			// vqendpoint codelen comes from counts of previous pass
			//	as we go through this pass, update it?
			// -> not helping so far
			// update codelen :
			// block_count starts at 0 and counts up as it gets used
			//	  could use P = block_count / num_blocks_walked_so_far
			F32 new_codelen = vq_codelen_palette_count( vqendpoints[best_p].block_count,&endpoint_codelen_help );
			vqendpoints[best_p].codelen = RR_MIN(new_codelen,vqendpoints[best_p].codelen);
			#endif
			
			if ( pal_mode == rrDXT1PaletteMode_Alpha )
				RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );
			
			// update lz_window :
			
			if ( best_p >= endpoint_vq_palette_num_to_search )
			{
				update_mtf_window(best_p,lz_window,lz_window_size,MAX_NUM_ADDED_ENTRIES_TO_TRY_ENDPOINTS);
			}
		}
		
		}
		
		//======================================================
		// VQ INDICES
		
		// prep by making endpoint codelens from the pass we just did
		//	(note endpoint_counting changes here)

		endpoint_counting.clear();
		//endpoint_counting.reserve( vqendpoints.size() );
		for LOOPVEC(i,vqendpoints)
		{
			U32 c = vqendpoints[i].block_count;
			if ( c == 0 ) continue;
			// count == 1 will be deleted too, but we save them for now just to do counts_singles
			endpoint_counting.push_back();
			endpoint_counting.back().dw = vqendpoints[i].dw;
			endpoint_counting.back().count = c;
		}

		// don't be reducing here, we just want to count them :
		setup_palette_and_vq_codelen_help(&endpoint_codelen_help,endpoint_counting,nblocks,eEndPoints,eNoReduce);
		
		dw_to_codelen_hash.clear();
		dw_to_codelen_hash.reserve(endpoint_counting.size32());
		
		for LOOPVEC(i,endpoint_counting)
		{
			F32 codelen = vq_codelen_palette_count( endpoint_counting[i].count,&endpoint_codelen_help );
			U32 dw = endpoint_counting[i].dw;
			RR_ASSERT( dw_to_codelen_hash.find(dw) == NULL );
			dw_to_codelen_hash.insert( dw, codelen );
		}		

		// refill index_counting from the previous endpoint pass

		// this has zero effect :
		//Optimize_Indices_For_Assigned_Blocks(blocks,dwc_scratch);
		
		for LOOPVEC(i,blocks)
		{
			rrDXT1_VQ_Block * pblock = &blocks[i];
			indices[i] = pblock->cur.indices;
		}
		
		sort_and_count_uniques(&index_counting,indices);		
	
		//rrprintfvar(index_counting.size());
		
		if ( lambda > 0 )
		{
			// index_counting is made

			// before first VQ iter
			//	do direct reduction of the indices :
			
			sort_dword_and_count_compare_count_highest_first(&index_counting);
			
			hacky_clamp_counts(&index_counting,nblocks);
			
			// vq_reduce also changes blocks->cur
			vector<dword_and_count> index_counting_reduced;
			bc1_indices_vq_reduce(&index_counting_reduced,index_counting,lambda,nblocks,blocks.data(),pal_mode);
			
			index_counting.swap(index_counting_reduced);
			
			//index_counting now has the unique indices & their count but is not yet sorted
		}
		
		//=================================================
		// now make index initial VQ Table :	
				
		//rrprintfvar(index_counting.size());
		
		setup_palette_and_vq_codelen_help(&index_codelen_help,index_counting,nblocks,eIndices,eNoReduce);

		vqindices.clear();
		vqindices.resize( index_counting.size() );
		
		dw_to_index_hash.clear();
		dw_to_index_hash.reserve( index_counting.size32() );
		
		for LOOPVEC(i,index_counting)
		{
			rrDXT1_VQ_Entry & vqp = vqindices[i];
			vqp.dw = index_counting[i].dw;
			
			RR_ASSERT( index_counting[i].count >= 2 );
			
			//DXT1_ComputePalette(vqp.endpoints.u.c0,vqp.endpoints.u.c1,vqp.palette,pal_mode);
			vqp.codelen = vq_codelen_palette_count( index_counting[i].count,&index_codelen_help);
			
			vqp.block_link = -1;
			vqp.block_count = 0;
			
			RR_ASSERT( dw_to_index_hash.find(vqp.dw) == NULL );
			dw_to_index_hash.insert(vqp.dw,i);
		}
		
		{
		SIMPLEPROFILE_SCOPE(indices_pass);
		
		int index_vq_palette_size_before = vqindices.size32();
		//rrprintfvar(index_vq_palette_size_before);
		
		// it does help a decent amount still to re-score the VQ palette here
		//	I still need it to find merges to flat at the moment
		//		because the merger doesn't go to flat
		//	    (merger now does do flats, that's not it)
		//	the decision here is just slightly more accurate
		//		uses VQD instead of SSD
		//		uses codelen for J instead of very rough estimate

		int index_vq_palette_num_to_search = RR_MIN(index_vq_palette_size_before,INDEX_VQ_PALETTE_SEARCH_LIMIT);
		
		int lz_window[MAX_NUM_ADDED_ENTRIES_TO_TRY_INDICES];
		int lz_window_size = 0;
		
		// for each block
		for LOOPVEC(block_i,blocks)
		{
			// try block with something from VQ vqindices
			rrDXT1_VQ_Block * pblock = &blocks[block_i];
			const rrColorBlock4x4 & colors = pblock->colors;
			
			// pblock->cur has the output of the endpoint vq stage
			// cur_ssd = the error of vq endpoint with free choice of indices
			
			if ( pal_mode == rrDXT1PaletteMode_Alpha )
				RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );

			// best_ssd = best error with vq of indices too			
			F32 best_vqd = (F32)RR_DXTC_ERROR_BIG;		
			F32 best_J = (F32)RR_DXTC_ERROR_BIG;
			int best_p = -1;
			
			// "cur" is the result of previous endpoint vq pass
			U32 cur_endpoints = rrDXT1Block_EndPoints_AsU32(pblock->cur);
			F32 cur_endpoints_codelen = find_hash_codelen_or_escape(cur_endpoints,dw_to_codelen_hash,&endpoint_codelen_help );
				
			U32 second_endpoints = rrDXT1Block_EndPoints_AsU32(pblock->second_best);
			F32 second_endpoints_codelen = find_hash_codelen_or_escape(second_endpoints,dw_to_codelen_hash,&endpoint_codelen_help );

			rrColor32BGRA first_palette[4];
			DXT1_ComputePalette(cur_endpoints,first_palette,pal_mode);
			
			rrColor32BGRA second_palette[4];
			DXT1_ComputePalette(second_endpoints,second_palette,pal_mode);
			
			// remember if we preferred first or second endpoints :
			U32 best_endpoints = cur_endpoints;
			
			// for all vq index palette on this block :
			// this is the very slow N^2 loop :
			int search_count = index_vq_palette_num_to_search + lz_window_size;
			for LOOP(search_i,search_count)
			{
				F32 vqp_indexes_codelen;
			
				int vqp;
				if ( search_i < index_vq_palette_num_to_search )
				{
					vqp = search_i;
					vqp_indexes_codelen = vqindices[vqp].codelen;
				}
				else
				{
					int lzi = search_i-index_vq_palette_num_to_search;
					RR_ASSERT( lzi < lz_window_size );
					vqp = lz_window[lzi];
					vqp_indexes_codelen = vqindices[vqp].codelen;
					
					#if 1
					// codelen model either by lz offset
					//	or by palette frequency
					// take the min?
					// @@ meh needs work
					// -> this is a small win on wood_worn which is the test that relies on LZ window the most
					//	on all others it's a NOP
					F32 lz_codelen = vq_codelen_lz(lzi);
					vqp_indexes_codelen = RR_MIN(lz_codelen,vqp_indexes_codelen);
					#endif
				}
				
				// lower palette index = higher count = preferable
				
				U32 vqp_indices = vqindices[vqp].dw;
				
				// @@ early out reject p if its indices are way wrong
				//	can we use the 16x8 index_diff here as an early out ?
			
				// if using J count and an SSD of zero could not beat current J for this count
				//	then break out
				//	(how often does that happen?)
					
				// @@ cur endpoints left alone (no endpoints-for-indices here)
				//	could try N best endpoints (currently tries 2 best)

					
				F32 vqd1 = (F32)RR_DXTC_ERROR_BIG;		
				F32 vqd2 = (F32)RR_DXTC_ERROR_BIG;		

				if ( pal_mode == rrDXT1PaletteMode_Alpha )
				{
					// early reject indices if they don't have alpha bits right
					if ( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,cur_endpoints,vqp_indices) )		
					{
						vqd1 = VQD(colors,first_palette ,vqp_indices,pblock->activity);
					}
						
					if ( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,second_endpoints,vqp_indices) )	
					{
						vqd2 = VQD(colors,second_palette,vqp_indices,pblock->activity);
					}
				}
				else
				{
					// cur_endpoints & second_endpoints :									
					vqd1 = VQD(colors,first_palette ,vqp_indices,pblock->activity);
					// if ( cur_endpoints != second_endpoints )
					vqd2 = VQD(colors,second_palette,vqp_indices,pblock->activity);
				}

				// @@ speedup opportunity :
				//	we're always doing VQD twice for 1st and 2nd endpoints
				//	a lot of work can be shared (index unpacking, activity vector)
				//	could be a bit faster by doing VQD_Twice()
					
				// favor lower palette indeces at equal ssd
				if ( vqd1 < best_vqd )
				{
					F32 J = vq_J(vqd1,vqp_indexes_codelen,cur_endpoints_codelen,lambda);
					
					if ( J < best_J )
					{
						best_J = J;			
						best_vqd = vqd1;
						best_p = vqp;
						best_endpoints = cur_endpoints;
					}
				}
				
				if ( vqd2 < best_vqd )
				{
					F32 J = vq_J(vqd2,vqp_indexes_codelen,second_endpoints_codelen,lambda);
					
					if ( J < best_J )
					{
						best_J = J;			
						best_vqd = vqd2;
						best_p = vqp;
						best_endpoints = second_endpoints;
					}
				}
				
				#if 0
				// take all the index VQ palette and find endpoints on them
				// and J score endpoints codelen
				
				// -> this did not help on test6
				
				// @@ just assume indices can be used in 4 color mode
				//	if they were 3-color indices this doesn't make much sense
				if ( DXT1_OptimizeEndPointsFromIndices_Fourc_NoReindex(&test,&ssd,test.indices,pblock->colors,false) )
				{
					// test.endpoints was filled
					U32 endpoints_codelen = find_hash_codelen_or_escape(test.endpoints,dw_to_codelen_hash,&endpoint_codelen_help );
					
					U32 J = vq_J(ssd,vqindices[p].codelen,endpoints_codelen,lambda);
					
					if ( J < best_J )
					{
						best_J = J;			
						best_ssd = ssd;
						best_p = p;
						best_endpoints = test.endpoints;
					}
				}
				#endif
			}
			
			// consider sending an escape & non-vq encoding instead
			// this compares the best vq-index ssd vs "cur" which is the result of vq_reduce
			
			// NOTE :
			//	 "cur" was changed by vq_reduce
			//	we could also have kept the "cur" that came out of vq endpoints and try against that
			//	-> TRIED IT, hurts a lot!
			//  we could also re-evaluate vs baseline (using new code cost estimate) -> NOPE			
			
			#ifdef DO_bc1_indices_vq_reduce_CHANGE_INDICES
			// cur.indices may have been changed by bc1_indices_vq_reduce
			pblock->cur_vqd = VQD(pblock->colors,pblock->cur,pblock->activity,pal_mode);
			#else
			RR_ASSERT( pblock->cur_vqd == VQD(pblock->colors,pblock->cur,pblock->activity,pal_mode) );
			#endif
			
			U32 cur_indices = pblock->cur.indices;
			
			// find cur.indices in vqindices
			// start with the assumption that cur indices are an escape (not in vqindices)
			F32 cur_indices_codelen = index_codelen_help.codelen_escape;
			int cur_indices_found_p = -1;
			
			// see if cur is in palette to get a cheaper codelen, if it's not really an escape
			// this was hurting because it prevented add-at-end
			// but with explicit lz_window it helps now
			// -> small but uniform benefit
			
			/*
			for LOOPVEC(pp,vqindices)
			{
				if ( vqindices[pp].dw == cur_indices )
				{
					cur_indices_codelen = vqindices[pp].codelen;
					cur_indices_found_p = pp;
					break;
				}
			}
			*/
			
			{
				t_hash_u32_to_u32::entry_ptrc ep = dw_to_index_hash.find(cur_indices);
				if ( ep != NULL )
				{
					cur_indices_found_p = ep->data();
					cur_indices_codelen = vqindices[cur_indices_found_p].codelen;
					RR_ASSERT( vqindices[cur_indices_found_p].dw == cur_indices );
				}
			}
					
			// checking this or not makes no difference :
			//only do escape if its lower error than best from palette :
			//if ( pblock->cur_vqd < best_vqd )
			{
				F32 J = vq_J(pblock->cur_vqd,cur_indices_codelen,cur_endpoints_codelen,lambda);
				
				#if 1
				if ( cur_endpoints != second_endpoints ) // equality is very rare
				{
					// can try second endpoints as well
					rrDXT1Block alt;
					alt.indices = cur_indices;
					alt.endpoints = second_endpoints;
					
					F32 alt_vqd = VQD(pblock->colors,alt,pblock->activity,pal_mode);
					
					F32 alt_J = vq_J(alt_vqd,cur_indices_codelen,second_endpoints_codelen,lambda);
				
					// this is much worse for vcdiff on wood_worn
					//	something non-local happening :(
					//if ( alt_J < J )
					// require vqd too improve too :
					if ( alt_J < J && alt_vqd < pblock->cur_vqd )
					{
						J = alt_J;
						// hacky slamming these :
						pblock->cur_vqd = alt_vqd;
						cur_endpoints = second_endpoints;
					}			
				}
				#endif
			
			
				if ( J < best_J )
				{
					best_J = J;			
					best_vqd = pblock->cur_vqd;
					best_endpoints = cur_endpoints;
		
					// pblock->cur left alone
						
					if ( cur_indices_found_p < 0 )
					{
						// not found in vqindices[]
						best_p = vqindices.size32();
			
						// add to palette :
						vqindices.push_back();
						rrDXT1_VQ_Entry & vqp = vqindices.back();
						//vqp.index_codelen = index_codelen_singleton;
						vqp.codelen = index_codelen_help.codelen_2count;
						// index_codelen as if I have a 2 count
						//	since someone using me again would make my count at least 2
						//	this makes it more desirable to reuse me than to send another escape
						
						vqp.dw = cur_indices;
						vqp.block_link = -1;
						vqp.block_count = 0;
						
						RR_ASSERT( dw_to_index_hash.find(cur_indices) == NULL );
						dw_to_index_hash.insert(cur_indices,best_p);	
					}
					else
					{
						best_p = cur_indices_found_p;
					}
				}
			}
	
			// change pblock->cur indices to the select vq palette entry
			// if escape happened these are a benign nop		
			pblock->cur.endpoints = best_endpoints;
			pblock->cur.indices = vqindices[best_p].dw;
			pblock->cur_vqd = best_vqd;
			pblock->cur_J = best_J;
			
			if ( pal_mode == rrDXT1PaletteMode_Alpha )
				RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );

			// record that best_p entry was used for this block :
			pblock->vq_entry_index = best_p;
			pblock->vq_entry_link = vqindices[best_p].block_link;
			vqindices[best_p].block_link = block_i;
			vqindices[best_p].block_count ++;
			
			// update lz_window :
			
			if ( best_p >= index_vq_palette_num_to_search )
			{
				update_mtf_window(best_p,lz_window,lz_window_size,MAX_NUM_ADDED_ENTRIES_TO_TRY_INDICES);
			}
			
		}
		
		} // profile scope
		
		// if iterating, scan out to "index_counting" to start again :
		
		index_counting.clear();
		index_counting.reserve( vqindices.size() );
		for LOOPVEC(i,vqindices)
		{
			U32 c = vqindices[i].block_count;
			if ( c == 0 ) continue;
			index_counting.push_back();
			index_counting.back().dw = vqindices[i].dw;
			index_counting.back().count = c;
		}

		// refill endpoint counting for next pass
		//	(it was changed by setup codelens & endpoints may have changed in the index vq loop)

		// doing this in-loop here helps a tiny bit :
		//	(the main benefit of it is in the "final_optimize"
		// note that doing this here breaks stored cur_vqd and cur_J
		Optimize_Endpoints_For_Assigned_Blocks(blocks,dwc_scratch,pal_mode);
		
		for LOOPVEC(i,blocks)
		{
			// try block with something from VQ vqendpoints
			rrDXT1_VQ_Block * pblock = &blocks[i];
			endpoints[i] = pblock->cur.endpoints;
		}
		
		sort_and_count_uniques(&endpoint_counting,endpoints);
	} // for vq_iterations	
	
	} // profile scope vq_iters
	
	//=========================================================
	
	#if 1
	{
		SIMPLEPROFILE_SCOPE(final_matrix);
		
		/*
		
		N*M final pass
		index_counting & endpoint_counting are filled
		need codelen helpers
		cur_J and cur_vqd are up to date in blocks
		
		
		up to this point we have done no joint endpoint-index vq palette selection
		endpoint pass did not look at index palette (it finds new indices, but scores them by index codelen)
		index pass did not look at endpoint palette (it just keeps endpoints fixed)
		
		
		this final pass uniformly lowers quality & rate
		it finds alternative blocks that are quite low rate because they are in the joint vq palette
		at possibly lower quality than what we already have
		-> that's what I thought would happen, but it's wrong
		in fact this pass can find higher quality encodings
		
		---------
		
		this is just brute force over a ton of SADs
		8k blocks
		~10k candidates
		= 80 M SADs
		
		
		could probably be a bit more clever here
		rather than just trying all blocks vs all candidates in the matrix
		since the SAD tolerance is reasonably tight
		you could take the color block and quantize it to some very rough color bins
		and then only look at the set in that same bin
		could reduce the candidate count by 4X or so
		without ruling out anything that could be within SAD tolerance
		
		for example could find the PCA axis over all blocks
		and turn each block into a scalar on that axis
		then you have to be within distance X to consider
		
		*/
		
		vq_codelen_help endpoint_codelen_help;
		setup_palette_and_vq_codelen_help(&endpoint_codelen_help,endpoint_counting,nblocks,eEndPoints,eNoReduce);
			
		vq_codelen_help index_codelen_help;
		setup_palette_and_vq_codelen_help(&index_codelen_help,index_counting,nblocks,eIndices,eNoReduce);

		// start with larger matrix dim than I want :
		#define FINAL_PASS_MATRIX_DIM_INITIAL	200  // 40k @@ ?? initial candidate set for codelen doesn't have a huge effect on speed

		// $$$$ these are a fine speed-quality tradeoff, needs careful tweak :
		//#define FINAL_PASS_MATRIX_DIM_SEARCH	110
		#define FINAL_PASS_MATRIX_DIM_SEARCH	100  // 10k @@ ??
		//#define FINAL_PASS_MATRIX_DIM_SEARCH	90  // 8k you can go lower, it hurts quality obvious but saves time
		
		int num_endpoints = RR_MIN(endpoint_counting.size32(), FINAL_PASS_MATRIX_DIM_INITIAL);
		int num_indices = RR_MIN(index_counting.size32(), FINAL_PASS_MATRIX_DIM_INITIAL);

		F32 codelen_endpoints[FINAL_PASS_MATRIX_DIM_INITIAL];
		F32 codelen_indices[FINAL_PASS_MATRIX_DIM_INITIAL];

		for LOOP(epi,num_endpoints)
		{
			codelen_endpoints[epi] = vq_codelen_palette_count(endpoint_counting[epi].count,&endpoint_codelen_help);
		}
		
		for LOOP(indi,num_indices)
		{
			codelen_indices[indi] = vq_codelen_palette_count(index_counting[indi].count,&index_codelen_help);
		}
		
		int initial_matrix_dim = num_indices * num_endpoints;

		//	take the top N*M with the lowest codelen
		
		vector<block_and_codelen> pairs;
		pairs.resize(initial_matrix_dim);
		
		for LOOP(epi,num_endpoints)
		{
			for LOOP(indi,num_indices)
			{
				U32 cur_endpoints = endpoint_counting[epi].dw;
				U32 cur_indices = index_counting[indi].dw;
				F32 codelen = codelen_endpoints[epi] + codelen_indices[indi];
				int i = epi + indi * num_endpoints;
				pairs[i].block.endpoints = cur_endpoints;
				pairs[i].block.indices = cur_indices;
				pairs[i].codelen = codelen;
			}
		}
		
		// sort by codelen
		//  lowest first
		stdsort(pairs.begin(),pairs.end(),block_and_codelen_compare_codelen());
		
		// now reduce matrix size to smaller
		//	with just the top (lowest) codelens :
		
		int num_pairs = RR_MIN(FINAL_PASS_MATRIX_DIM_SEARCH*FINAL_PASS_MATRIX_DIM_SEARCH,pairs.size32());
		pairs.resize(num_pairs);
		
		// decompress them to colors
		// NOTE(fg): with the pair loop now chunked, could decompress a chunk at a time
		// and save some memory, but I'll keep changes down to a minimum right now.
		
		vector<rrColorBlock4x4> pair_colors;
		pair_colors.resize(num_pairs);
		
		for LOOPVEC(i,pairs)
		{
			DXT1_Decompress(&pair_colors[i],pairs[i].block,pal_mode);
		}

		vector<block_final_state> final_st;
		final_st.resize(blocks.size());

		// initialize final_st for every block
		// (spilled state for the current totals for every block)
		for LOOPVEC(block_i,blocks)
		{
			rrDXT1_VQ_Block * pblock = &blocks[block_i];
			const rrColorBlock4x4 & colors = pblock->colors;

			// get the SAD of the current chosen encoding :
			rrColorBlock4x4 cur_colors;
			DXT1_Decompress(&cur_colors,pblock->cur,pal_mode);

			if ( pal_mode == rrDXT1PaletteMode_Alpha )
				RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );

			U32 cur_sad = ColorBlock4x4_ComputeSAD_RGBA(colors,cur_colors);
			F32 cur_J = pblock->cur_J;
			F32 cur_vqd = pblock->cur_vqd;

			// Optimize_Endpoints_For_Assigned_Blocks spoils this :
			//RR_DURING_ASSERT( F32 check_vqd = VQD(colors,pblock->cur,pblock->activity) );
			//RR_ASSERT( fequal(cur_vqd,check_vqd,0.001f) );
			cur_vqd = VQD(colors,cur_colors,pblock->activity);

			// fix cur_J for vqd change :
			RR_ASSERT( cur_J > pblock->cur_vqd );
			cur_J -= pblock->cur_vqd; // J is now just the rate part
			cur_J += cur_vqd;

			// cur_J NOT = vq_J calculation because codelens have changed for this pass

			//recall J = D + (lambda*scale) * R;

			// if codelen is >= maximum_codelen then you can't beat cur_J even at D = 0
			// assume you can get to 0 D :
			//F32 maximum_codelen = cur_J  * scale / lambda;

			// maximum_codelen is a pure termination criterion, not an approximation
			//	if we assumed that we only ever do as well as "min_vqd"
			//	rather than thinking we can get down to D = 0

			// assume you could find baseline quality : (very conservative)
			// guess at what the minimum possible VQD is for this block in any BC1 encoding
			F32 min_vqd = RR_MIN(cur_vqd,pblock->baseline_vqd);
			F32 maximum_codelen = vq_J_to_R_codelen(cur_J - min_vqd, lambda);

			// assume you can only get to cur_vqd : // is this okay? -> no, hurts
			//F32 maximum_codelen = (cur_J - cur_vqd) * scale / lambda;

			RR_ASSERT( maximum_codelen > 0.f );

			// it appears the best thing is to allow only minimal SAD increase :
			//	(remember lambda goes in steps of 10 now so this isn't tiny)
			//#define FINAL_PASS_SAD_INCREASE_TOLERANCE	10 // @@ ??
			//#define FINAL_PASS_SAD_INCREASE_TOLERANCE	20 // no difference to 10 ?
			//#define FINAL_PASS_SAD_INCREASE_TOLERANCE	5 // no difference to 10 ?
			#define FINAL_PASS_SAD_INCREASE_TOLERANCE	1 // better !? (than 0 or 2)

			// start with a criterion that you must get SAD within some proximity of "cur"
			//	then after that it must be strictly decreasing
			U32 must_beat_sad = cur_sad + lambda * FINAL_PASS_SAD_INCREASE_TOLERANCE;

			// must_beat_sad could be estimated from cur_J and a SAD-VQD ratio bound
			// VQD is ~ 22 * SAD
			// must_beat_sad ~ J / 22  (assume you find a zero rate encoding)
			// this seems okay but worse
			//U32 must_beat_sad = (U32) ( cur_J / 16 );

			final_st[block_i].cur_J = cur_J;
			final_st[block_i].min_vqd = min_vqd;
			final_st[block_i].maximum_codelen = maximum_codelen;
			final_st[block_i].must_beat_sad = must_beat_sad;
		}

		// process pairs in chunks of pair_colors that stay in L1 to minimize cache thrashing
		// this was a big speed-up!
		const int pair_chunk_size = (16 * 1024) / sizeof(pair_colors[0]);

		for (int pair_chunk_begin = 0; pair_chunk_begin < num_pairs; pair_chunk_begin += pair_chunk_size)
		{
			int pair_chunk_end = RR_MIN(pair_chunk_begin + pair_chunk_size, num_pairs);

			// for each block
			for LOOPVEC(block_i,blocks)
			{
				rrDXT1_VQ_Block * pblock = &blocks[block_i];
				const rrColorBlock4x4 & colors = pblock->colors;

				F32 & cur_J = final_st[block_i].cur_J;
				F32 min_vqd = final_st[block_i].min_vqd;
				F32 & maximum_codelen = final_st[block_i].maximum_codelen;
				U32 & must_beat_sad = final_st[block_i].must_beat_sad;

				// go over the N*M candidates
				//for LOOPVEC(pair_i,pairs)
				for (int pair_i = pair_chunk_begin; pair_i < pair_chunk_end; pair_i++)
				{
					//compute SAD

					// if SAD got better, and is now within tolerance of "cur"
					//   then compute J and see if it beats cur

					//it's a whole lot of 48-vec SADs
					//(that's with colors packed to remove A)
					//tight packing just makes it 3 sads instead of 4
					//
					//using SAD is just a way to reduce the candidates passed to VQD & J
					//it's not an approximation of J here
					//(unlike vq_reduce where it is the final approximation)

					// @@ this can just be the loop termination criterion :
					//		make maximum_codelen <= last pairs[] codelen
					// maximum_codelen is a decent truncation of the search
					//	I'm seeing it cut the 10,000 items down to 5,000 or so
					if ( pairs[pair_i].codelen >= maximum_codelen )
						break;

					// @@ does the optimizer keep the vectors of colors[] in register?
					// @@ currently doing 4 vec sads, could tight pack the RGBs and do 3 vec SADs
					U32 this_sad = ColorBlock4x4_ComputeSAD_RGBA(colors,pair_colors[pair_i]);

					if ( this_sad >= must_beat_sad )
						continue;

					// SAD went down, evaluate this block :
					
					if ( pal_mode == rrDXT1PaletteMode_Alpha )
					{
						// reject if 1-bit A flag doesn't match :
						if ( ! DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pairs[pair_i].block) )
							continue;
					}

					must_beat_sad = this_sad;

					F32 vqd = VQD(colors,pair_colors[pair_i],pblock->activity);

					F32 J = vq_J(vqd,pairs[pair_i].codelen,0,lambda);

					// often the "cur" is re-found here
					//	but J is a bit different because the codelen estimates are different now

					if ( J < cur_J )
					{
						// take it !
						cur_J = J;
						//cur_vqd = vqd;
						pblock->cur = pairs[pair_i].block;

						//*
						// as we find lower J, we might be able to lower maximum_codelen :
						// this is pretty meh, doesn't affect speed or quality
						// the J gains we make here are just typically small
						//	therefore maximum_codelen doesn't change a ton
						maximum_codelen = vq_J_to_R_codelen(cur_J - min_vqd,lambda);
						/**/
					}
				}
			}
		}
	}
	#endif
	
	//=========================================================
	
	#if 1
	{
		SIMPLEPROFILE_SCOPE(final_optimize);
	
		// now take the set of blocks assigned to each VQ entry
		//	and re-find the indexes/endpoints that optimize for that set of blocks
		//	assuming opposite endpoints/indexes fixed

		// was just done at end of iters :
		//Optimize_Endpoints_For_Assigned_Blocks(blocks,dwc_scratch);
	
		Optimize_Indices_For_Assigned_Blocks(blocks,dwc_scratch,pal_mode);
		
		// and again : does help a tiny bit :
		Optimize_Endpoints_For_Assigned_Blocks(blocks,dwc_scratch,pal_mode);
	}
	#endif
	
	//=========================================================
	// put to output surface :

	// we set up "to" to the *true* width and height
	//	it will be allocated to the next multiple of 4
	// (Alloc is a nop if already done)
	//if ( ! rrSurface_Alloc(to_dxt1Surface,from->width,from->height,rrPixelFormat_BC1) )
	//	return false;
	
	for LOOP(bi,nblocks)
	{
		U8 * outPtr = BlockSurface_Seek(to_blocks,bi);
		
		const rrDXT1_VQ_Block * pblock = &blocks[bi];

		if ( pal_mode == rrDXT1PaletteMode_Alpha )
			RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );
				
		memcpy(outPtr,&(pblock->cur),sizeof(pblock->cur));
	}
		
	return true;
}

// copy-paste town
// just the same as normal DXT1_OptimizeEndPointsFromIndices
//	but iterates over blocks
static bool DXT1_OptimizeEndPointsFromIndices_Inherit_MultiBlock(U32 * pEndPoints, bool fourc, 
	const vector<rrDXT1_VQ_Block> & blocks, 
	const vector<dword_and_count> & dcv, int dcv_i_start, int dcv_i_end)
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
	
	// find endpoints that minimize sum of errors across N blocks
	// that just means add up the terms from all the blocks
	
	// walk all blocks that use this vq entry :
	for(int dcvi=dcv_i_start;dcvi<dcv_i_end;dcvi++)
	{
		int bi = dcv[dcvi].dw;
		const rrDXT1_VQ_Block * pblock = &blocks[bi];
		//RR_ASSERT( pblock->cur.endpoints == oldep );

		const U32 indices = pblock->cur.indices;
		const rrColorBlock4x4 & colors = pblock->colors;
	
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
	} // for all blocks in link
	
	// A,B matrix is now for the sum of all blocks
	
	int det = AA*BB - AB*AB;
	if ( det == 0 )
	{
		return false;
	}
	
	// have to multiply invDet by the normalization factor that was used on weights :
	//F32 invDet = 1.0 / det;
	float invDet = pWeights[0] / (float) det;
	
	rrVec3f vA = float(  BB * invDet) * AX + float(- AB * invDet ) * BX;
	rrVec3f vB = float(- AB * invDet) * AX + float(  AA * invDet ) * BX;
	
	rrColor32BGRA cA = Vec3fToColorClamp(vA);
	rrColor32BGRA cB = Vec3fToColorClamp(vB);
	
	rrColor565Bits qA = Quantize(cA);
	rrColor565Bits qB = Quantize(cB);
	
	// @@ just quantizing here may be sub-optimal
	//	when near a boundary, should try both ways and see which gives lower error
	
	// note in degenerate cases if qA == qB , that's always 3-index
	
	// switch endpoints to maintain four-color state :
	
	// NOTE : switching endpoints makes them no longer be the lsqr solve for these indices!
	//	they are now the lsqr solve for flip(indices)
	//	re-indexing fixes this but if you don't reindex, it may be much worse
	
	if ( qA.w < qB.w )
	{
		RR_NAMESPACE::swap(qA.w,qB.w);
	}
	
	if ( ! fourc )
		RR_NAMESPACE::swap(qA,qB);
	
	rrDXT1EndPoints endpoints;
	endpoints.u.c0 = qA;
	endpoints.u.c1 = qB;
	*pEndPoints = endpoints.dw;
	return true;
}

/***

Optimize_Endpoints_For_Assigned_Blocks

done as very final pass

does not break any index or endpoint sharing
  (indices are not changed at all)
  
endpoints that were a set stay a set

so this should be close to rate-invariant

just look for Distortion gains that can be made without changing Rate

for all the blocks that are assigned to some endpoints oldep
	see if there is some endpoints newep
	that can be used on all of them (indices kept the same)
	and improve error
	
can just use SSD here (not VQD)
because this is not a rate allocation problem

***/

static void Optimize_Endpoints_For_Assigned_Blocks(
	vector<rrDXT1_VQ_Block> & blocks,
	vector<dword_and_count> & dcv,
	rrDXT1PaletteMode pal_mode)
{
	// optimize palette entries from blocks mapped to them
	
	int nblocks = blocks.size32();
	
	// sort by endpoints
	//	with block index as payload
	//	to get runs of same endpoint
	dcv.clear();
	dcv.resize(nblocks);
	
	for LOOPVEC(b,blocks)
	{
		U32 endpoints = blocks[b].cur.endpoints;
		
		dcv[b].dw = b;
		dcv[b].count = endpoints;
	}
	
	// sort by "count" (that's endpoint value) :
	sort_dword_and_count_compare_count_highest_first(&dcv);
		
	// now for each vq palette entry :
	// look at blocks that mapped to that entry
	// choose new endpoints that optimize for those blocks
	// (endpoints-from-indices on the combined matrix)

	// (if there's only one block on the entry, AND its == baseline, can skip this)
	// (if there's only one block on the entry, and its not baseline, see if replacing it with baseline is better)
	
	int dcv_i_start = 0;
	while(dcv_i_start<nblocks)
	{
		int bi_start = dcv[dcv_i_start].dw;
		U32 endpoints = blocks[bi_start].cur.endpoints;
		int dcv_i_end = dcv_i_start+1;
		while( dcv_i_end < nblocks && blocks[ dcv[dcv_i_end].dw ].cur.endpoints == endpoints )
		{
			dcv_i_end++;
		}
		
		int count = dcv_i_end - dcv_i_start;
		RR_ASSERT( count > 0 );
		
		if ( count == 1 )
		{
			// get the one block that mapped to me :
			rrDXT1_VQ_Block * pblock = &blocks[bi_start];
			
			#if 0
			if ( pblock->baseline_vqd < pblock->cur_vqd )
			{
				// block thought it was doing VQ to a shared palette p
				//	but it wound up as the only one mapped there
				// so just use baseline
			
				// can't just do this
				//	because cur indices may have been chosen to be cheaper than baseline indices
				//  this was okay when we were doin this before hte indexes phase
			
				pblock->cur = pblock->baseline;
				pblock->cur_vqd = pblock->baseline_vqd;
			}
			#endif
			
			// cur == baseline is quite common at low lambda
			if ( pblock->cur != pblock->baseline )
			{			
				U32 err = DXT1_ComputeSSD_RGBA(pblock->colors,pblock->cur,pal_mode);
		
				// changes pblock->cur.endpoints if it finds a win
				DXT1_OptimizeEndPointsFromIndices_Inherit_NoReindex(&(pblock->cur),&err,pblock->colors,pal_mode);
				
				if ( pal_mode == rrDXT1PaletteMode_FourColor )
				{
					RR_ASSERT( rrDXT1Block_IsBC3Canonical(&pblock->cur) );
					// no need, should already be so because of Inherit :
					//rrDXT1Block_BC3_Canonicalize(&(pblock->cur));
				}
				
				#if 0
				// see if baseline endpoints are better
				// this can happen because the VerySlow encoder that made baseline
				//	is better than just doing OptimizeEndPointsFromIndices
				// -> this is hit non-zero times but it is very rare
				rrDXT1Block test;
				test.endpoints = pblock->baseline.endpoints;
				test.indices = pblock->cur.indices;
				U32 test_err = DXT1_ComputeSSD_RGBA(pblock->colors,test,pal_mode);
				if ( test_err < err )
				{
					pblock->cur = test;
				}
				#endif
			}
		}
		else // count >= 2
		{
			// multi-block endpoints from indices
			
			U32 oldep = endpoints;
			bool fourc = DXT1_Is4Color(oldep,pal_mode);
			
			U32 newep;
			if ( DXT1_OptimizeEndPointsFromIndices_Inherit_MultiBlock(&newep,fourc,blocks,dcv,dcv_i_start,dcv_i_end) )
			{
			
				if ( pal_mode == rrDXT1PaletteMode_FourColor )
				{
					RR_ASSERT( rrDXT1Block_IsBC3Canonical(newep) );
				}
			
				if ( newep != oldep )
				{
					// need to compute err before & after to see if it actually helped
				
					// if it did, then walk the blocks and assign the new ep :
					
					rrColor32BGRA oldpalette[4];
					DXT1_ComputePalette(oldep,oldpalette,pal_mode);
					
					rrColor32BGRA newpalette[4];
					DXT1_ComputePalette(newep,newpalette,pal_mode);

					U32 total_ssd_before = 0;
					U32 total_ssd_after = 0;

					// walk all blocks that use this vq entry :
					for(int dcvi=dcv_i_start;dcvi<dcv_i_end;dcvi++)
					{
						int bi = dcv[dcvi].dw;
						const rrDXT1_VQ_Block * pblock = &blocks[bi];
						RR_ASSERT( pblock->cur.endpoints == oldep );
						
						U32 indices = pblock->cur.indices;
						
						/*
						// do NOT reindex
						U32 err;
						U32 indices = DXT1_FindIndices(pblock->colors,palette,pal_mode,&err);
						*/
						
						U32 oldssd = BC1_Palette_SSD_RGBA(&pblock->colors,oldpalette,indices);
						U32 newssd = BC1_Palette_SSD_RGBA(&pblock->colors,newpalette,indices);
						
						total_ssd_before += oldssd;
						total_ssd_after += newssd;						
					}
					
					// semi-random , sometimes it goes up, sometimes down
					// (except if endpoints had to flip to maintain fourc state, then error is always big)
					
					if ( total_ssd_after < total_ssd_before )
					{
						//rrprintfvar(total_ssd_before);
						//rrprintfvar(total_ssd_after);
					
						// okay, do it :
						
						for(int dcvi=dcv_i_start;dcvi<dcv_i_end;dcvi++)
						{
							int bi = dcv[dcvi].dw;
							rrDXT1_VQ_Block * pblock = &blocks[bi];
							RR_ASSERT( pblock->cur.endpoints == oldep );
						
							pblock->cur.endpoints = newep;
						}
					}
				}
			}
		}
		
		dcv_i_start = dcv_i_end;
	}
}

static bool DXT1_FindIndices_MultiBlock(U32 *p_new_indices,U32 old_indices,
	const vector<rrDXT1_VQ_Block> & blocks, 
	const vector<dword_and_count> & dcv, int dcv_i_start, int dcv_i_end,
	rrDXT1PaletteMode pal_mode)
{
	// just a normal FindIndices
	//	for each of 16 colors, compute 4 distances to palette
	// we just sum all the distances across N blocks
	// before doing the min-of-4

	// @@ this could obviously be SIMD
	//	 but it's not a significant portion of total time
	//	so don't bother with it for now

	U32 old_err = 0;
	
	U32 dsqrs[16][4] = { };
	
	// walk all blocks that use this vq entry :
	for(int dcvi=dcv_i_start;dcvi<dcv_i_end;dcvi++)
	{
		int bi = dcv[dcvi].dw;
		const rrDXT1_VQ_Block * pblock = &blocks[bi];
		RR_ASSERT( pblock->cur.indices == old_indices );
	
		if ( pal_mode == rrDXT1PaletteMode_Alpha )
			RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );

		rrColor32BGRA palette[4];
		DXT1_ComputePalette(pblock->cur.endpoints,palette,pal_mode);
		
		const rrColorBlock4x4 & colors = pblock->colors;
		for LOOP(colori,16)
		{
			rrColor32BGRA c = colors.colors[colori];
			
			// get old SSD to compare to :
			rrColor32BGRA oldc = palette[ ((old_indices >> (colori*2))&3) ];
			old_err += rrColor32BGRA_DeltaSqrRGBA(c,oldc);
			
			// accumulate 4 palette distances :
			RR_UNROLL_I_4(0, dsqrs[colori][i] += rrColor32BGRA_DeltaSqrRGBA_1BT(c, palette[i]) );
		}
	}
	
	// all blocks added up, now find best index for each pixel pos :
	
	U32 new_err = 0;
	U32 new_indices = 0;
	
	for LOOP(colori,16)
	{
		// min of 4 :
		U32 best = dsqrs[colori][0]*4;
		best = RR_MIN(best, dsqrs[colori][1]*4 + 1);
		best = RR_MIN(best, dsqrs[colori][2]*4 + 2);
		best = RR_MIN(best, dsqrs[colori][3]*4 + 3);
		
		// new_err winds up with the sum of SSD's for these indices over all blocks
		U32 besterr= best>>2;
		new_err += besterr;
		
		//U32 besti = best&3;
		//new_indices |= besti << (colori*2);
		
		new_indices >>= 2;
		new_indices |= (best<<30);
	}
	
	*p_new_indices = new_indices;
	
	RR_ASSERT( new_err <= old_err );
	
	bool better = new_err < old_err;
	return better;
}

/**

Optimize_Indices_For_Assigned_Blocks

for each index vq palette entry
	look at the blocks actually assigned to it
	optimize the index values for those blocks

only changes indices, not endpoints

only changes whole groups of index value
	all occurances of I1 change to I2
so the # of unique indices & count of each doesn't change
so it should be roughly codelen invariant
only distortion changes

**/

static void Optimize_Indices_For_Assigned_Blocks(
	vector<rrDXT1_VQ_Block> & blocks,
	vector<dword_and_count> & dcv,
	rrDXT1PaletteMode pal_mode)
{
	// optimize palette entries from blocks mapped to them
	
	int nblocks = blocks.size32();
	
	// sort by indices
	//	with block index as payload
	//	to get runs of same indices
	dcv.clear();
	dcv.resize(nblocks);
	
	for LOOPVEC(b,blocks)
	{
		dcv[b].dw = b;
		dcv[b].count = blocks[b].cur.indices;
	}
	
	// sort by "count" (that's indices value) :
	sort_dword_and_count_compare_count_highest_first(&dcv);
		
	// now for each vq palette entry :
	// look at blocks that mapped to that entry
	// choose new indices that optimize for those blocks

	// (if there's only one block on the entry, AND its == baseline, can skip this)
	// (if there's only one block on the entry, and its not baseline, see if replacing it with baseline is better)
	
	int dcv_i_start = 0;
	while(dcv_i_start<nblocks)
	{
		int bi_start = dcv[dcv_i_start].dw;
		U32 old_indices = blocks[bi_start].cur.indices;
		int dcv_i_end = dcv_i_start+1;
		while( dcv_i_end < nblocks && blocks[ dcv[dcv_i_end].dw ].cur.indices == old_indices )
		{
			dcv_i_end++;
		}
		
		int count = dcv_i_end - dcv_i_start;
		RR_ASSERT( count > 0 );
		
		if ( count == 1 )
		{
			// get the one block that mapped to me :
			rrDXT1_VQ_Block * pblock = &blocks[bi_start];
						
			// cur == baseline is quite common at low lambda
			if ( pblock->cur != pblock->baseline )
			{			
				U32 old_err = DXT1_ComputeSSD_RGBA(pblock->colors,pblock->cur,pal_mode);
		
				// changes pblock->cur.endpoints if it finds a win
				U32 new_err = RR_DXTC_INIT_ERROR;
				U32 new_indices = DXT1_FindIndices(pblock->colors,pblock->cur.endpoints,pal_mode,&new_err);
				
				RR_ASSERT( new_err <= old_err );
	
				if ( new_err < old_err )
				{
					pblock->cur.indices = new_indices;
				}
			}
		}
		else // count >= 2
		{
			// multi-block best indices
			
			U32 new_indices = old_indices;
			if ( DXT1_FindIndices_MultiBlock(&new_indices,old_indices,blocks,dcv,dcv_i_start,dcv_i_end,pal_mode) )
			{
				// yes, commit the change :
			
				for(int dcvi=dcv_i_start;dcvi<dcv_i_end;dcvi++)
				{
					int bi = dcv[dcvi].dw;
					rrDXT1_VQ_Block * pblock = &blocks[bi];
					RR_ASSERT( pblock->cur.indices == old_indices );
					pblock->cur.indices = new_indices;
					
					if ( pal_mode == rrDXT1PaletteMode_Alpha )
						RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(pblock->block_1bt_mask,pblock->cur) );
				}
			}
		}
		
		dcv_i_start = dcv_i_end;
	}
}
				
//===============================================================
// rrdxt1vqindices

// 4c indices -> 0,2,4,6
static const U32 c_bc1_indices_to_bytes_LE_4c[256] = 
{
  0x00000000,0x00000006,0x00000002,0x00000004,0x00000600,0x00000606,0x00000602,0x00000604,
  0x00000200,0x00000206,0x00000202,0x00000204,0x00000400,0x00000406,0x00000402,0x00000404,
  0x00060000,0x00060006,0x00060002,0x00060004,0x00060600,0x00060606,0x00060602,0x00060604,
  0x00060200,0x00060206,0x00060202,0x00060204,0x00060400,0x00060406,0x00060402,0x00060404,
  0x00020000,0x00020006,0x00020002,0x00020004,0x00020600,0x00020606,0x00020602,0x00020604,
  0x00020200,0x00020206,0x00020202,0x00020204,0x00020400,0x00020406,0x00020402,0x00020404,
  0x00040000,0x00040006,0x00040002,0x00040004,0x00040600,0x00040606,0x00040602,0x00040604,
  0x00040200,0x00040206,0x00040202,0x00040204,0x00040400,0x00040406,0x00040402,0x00040404,
  0x06000000,0x06000006,0x06000002,0x06000004,0x06000600,0x06000606,0x06000602,0x06000604,
  0x06000200,0x06000206,0x06000202,0x06000204,0x06000400,0x06000406,0x06000402,0x06000404,
  0x06060000,0x06060006,0x06060002,0x06060004,0x06060600,0x06060606,0x06060602,0x06060604,
  0x06060200,0x06060206,0x06060202,0x06060204,0x06060400,0x06060406,0x06060402,0x06060404,
  0x06020000,0x06020006,0x06020002,0x06020004,0x06020600,0x06020606,0x06020602,0x06020604,
  0x06020200,0x06020206,0x06020202,0x06020204,0x06020400,0x06020406,0x06020402,0x06020404,
  0x06040000,0x06040006,0x06040002,0x06040004,0x06040600,0x06040606,0x06040602,0x06040604,
  0x06040200,0x06040206,0x06040202,0x06040204,0x06040400,0x06040406,0x06040402,0x06040404,
  0x02000000,0x02000006,0x02000002,0x02000004,0x02000600,0x02000606,0x02000602,0x02000604,
  0x02000200,0x02000206,0x02000202,0x02000204,0x02000400,0x02000406,0x02000402,0x02000404,
  0x02060000,0x02060006,0x02060002,0x02060004,0x02060600,0x02060606,0x02060602,0x02060604,
  0x02060200,0x02060206,0x02060202,0x02060204,0x02060400,0x02060406,0x02060402,0x02060404,
  0x02020000,0x02020006,0x02020002,0x02020004,0x02020600,0x02020606,0x02020602,0x02020604,
  0x02020200,0x02020206,0x02020202,0x02020204,0x02020400,0x02020406,0x02020402,0x02020404,
  0x02040000,0x02040006,0x02040002,0x02040004,0x02040600,0x02040606,0x02040602,0x02040604,
  0x02040200,0x02040206,0x02040202,0x02040204,0x02040400,0x02040406,0x02040402,0x02040404,
  0x04000000,0x04000006,0x04000002,0x04000004,0x04000600,0x04000606,0x04000602,0x04000604,
  0x04000200,0x04000206,0x04000202,0x04000204,0x04000400,0x04000406,0x04000402,0x04000404,
  0x04060000,0x04060006,0x04060002,0x04060004,0x04060600,0x04060606,0x04060602,0x04060604,
  0x04060200,0x04060206,0x04060202,0x04060204,0x04060400,0x04060406,0x04060402,0x04060404,
  0x04020000,0x04020006,0x04020002,0x04020004,0x04020600,0x04020606,0x04020602,0x04020604,
  0x04020200,0x04020206,0x04020202,0x04020204,0x04020400,0x04020406,0x04020402,0x04020404,
  0x04040000,0x04040006,0x04040002,0x04040004,0x04040600,0x04040606,0x04040602,0x04040604,
  0x04040200,0x04040206,0x04040202,0x04040204,0x04040400,0x04040406,0x04040402,0x04040404
};

// 3c indices -> 0,3,6,13
static const U32 c_bc1_indices_to_bytes_LE_3c[256] = 
{
  0x00000000,0x00000006,0x00000003,0x0000000D,0x00000600,0x00000606,0x00000603,0x0000060D,
  0x00000300,0x00000306,0x00000303,0x0000030D,0x00000D00,0x00000D06,0x00000D03,0x00000D0D,
  0x00060000,0x00060006,0x00060003,0x0006000D,0x00060600,0x00060606,0x00060603,0x0006060D,
  0x00060300,0x00060306,0x00060303,0x0006030D,0x00060D00,0x00060D06,0x00060D03,0x00060D0D,
  0x00030000,0x00030006,0x00030003,0x0003000D,0x00030600,0x00030606,0x00030603,0x0003060D,
  0x00030300,0x00030306,0x00030303,0x0003030D,0x00030D00,0x00030D06,0x00030D03,0x00030D0D,
  0x000D0000,0x000D0006,0x000D0003,0x000D000D,0x000D0600,0x000D0606,0x000D0603,0x000D060D,
  0x000D0300,0x000D0306,0x000D0303,0x000D030D,0x000D0D00,0x000D0D06,0x000D0D03,0x000D0D0D,
  0x06000000,0x06000006,0x06000003,0x0600000D,0x06000600,0x06000606,0x06000603,0x0600060D,
  0x06000300,0x06000306,0x06000303,0x0600030D,0x06000D00,0x06000D06,0x06000D03,0x06000D0D,
  0x06060000,0x06060006,0x06060003,0x0606000D,0x06060600,0x06060606,0x06060603,0x0606060D,
  0x06060300,0x06060306,0x06060303,0x0606030D,0x06060D00,0x06060D06,0x06060D03,0x06060D0D,
  0x06030000,0x06030006,0x06030003,0x0603000D,0x06030600,0x06030606,0x06030603,0x0603060D,
  0x06030300,0x06030306,0x06030303,0x0603030D,0x06030D00,0x06030D06,0x06030D03,0x06030D0D,
  0x060D0000,0x060D0006,0x060D0003,0x060D000D,0x060D0600,0x060D0606,0x060D0603,0x060D060D,
  0x060D0300,0x060D0306,0x060D0303,0x060D030D,0x060D0D00,0x060D0D06,0x060D0D03,0x060D0D0D,
  0x03000000,0x03000006,0x03000003,0x0300000D,0x03000600,0x03000606,0x03000603,0x0300060D,
  0x03000300,0x03000306,0x03000303,0x0300030D,0x03000D00,0x03000D06,0x03000D03,0x03000D0D,
  0x03060000,0x03060006,0x03060003,0x0306000D,0x03060600,0x03060606,0x03060603,0x0306060D,
  0x03060300,0x03060306,0x03060303,0x0306030D,0x03060D00,0x03060D06,0x03060D03,0x03060D0D,
  0x03030000,0x03030006,0x03030003,0x0303000D,0x03030600,0x03030606,0x03030603,0x0303060D,
  0x03030300,0x03030306,0x03030303,0x0303030D,0x03030D00,0x03030D06,0x03030D03,0x03030D0D,
  0x030D0000,0x030D0006,0x030D0003,0x030D000D,0x030D0600,0x030D0606,0x030D0603,0x030D060D,
  0x030D0300,0x030D0306,0x030D0303,0x030D030D,0x030D0D00,0x030D0D06,0x030D0D03,0x030D0D0D,
  0x0D000000,0x0D000006,0x0D000003,0x0D00000D,0x0D000600,0x0D000606,0x0D000603,0x0D00060D,
  0x0D000300,0x0D000306,0x0D000303,0x0D00030D,0x0D000D00,0x0D000D06,0x0D000D03,0x0D000D0D,
  0x0D060000,0x0D060006,0x0D060003,0x0D06000D,0x0D060600,0x0D060606,0x0D060603,0x0D06060D,
  0x0D060300,0x0D060306,0x0D060303,0x0D06030D,0x0D060D00,0x0D060D06,0x0D060D03,0x0D060D0D,
  0x0D030000,0x0D030006,0x0D030003,0x0D03000D,0x0D030600,0x0D030606,0x0D030603,0x0D03060D,
  0x0D030300,0x0D030306,0x0D030303,0x0D03030D,0x0D030D00,0x0D030D06,0x0D030D03,0x0D030D0D,
  0x0D0D0000,0x0D0D0006,0x0D0D0003,0x0D0D000D,0x0D0D0600,0x0D0D0606,0x0D0D0603,0x0D0D060D,
  0x0D0D0300,0x0D0D0306,0x0D0D0303,0x0D0D030D,0x0D0D0D00,0x0D0D0D06,0x0D0D0D03,0x0D0D0D0D
};



// unpack 2 bit indices -> 16x8 byte vector
//	 also remap to linear order 0->3
static void bc1_indices_4c_unpack_to_16x8(U8 * dest,U32 indices)
{
	// low bits of indices are first in 4x4 scan order
	RR_PUT32_LE(dest   , c_bc1_indices_to_bytes_LE_4c[ (indices    )&0xFF ] );
	RR_PUT32_LE(dest+4 , c_bc1_indices_to_bytes_LE_4c[ (indices>>8 )&0xFF ] );
	RR_PUT32_LE(dest+8 , c_bc1_indices_to_bytes_LE_4c[ (indices>>16)&0xFF ] );
	RR_PUT32_LE(dest+12, c_bc1_indices_to_bytes_LE_4c[ (indices>>24)&0xFF ] );
}

static void bc1_indices_3c_unpack_to_16x8(U8 * dest,U32 indices)
{
	// low bits of indices are first in 4x4 scan order
	RR_PUT32_LE(dest   , c_bc1_indices_to_bytes_LE_3c[ (indices    )&0xFF ] );
	RR_PUT32_LE(dest+4 , c_bc1_indices_to_bytes_LE_3c[ (indices>>8 )&0xFF ] );
	RR_PUT32_LE(dest+8 , c_bc1_indices_to_bytes_LE_3c[ (indices>>16)&0xFF ] );
	RR_PUT32_LE(dest+12, c_bc1_indices_to_bytes_LE_3c[ (indices>>24)&0xFF ] );
	
	/*
	// test : make the blacks to FF for masking
	for LOOP(i,16)
	{
		if ( dest[i] > 6 )
			dest[i] = 0xFF;
	}
	/**/
}

#ifdef __RADSSE2__
	
static inline U32 sse2_u8x16_ssd( const __m128i & v1, const __m128i & v2 )
{
	__m128i sub8 = _mm_or_si128( _mm_subs_epu8(v1,v2), _mm_subs_epu8(v2,v1) );
	__m128i sub16_1 = _mm_unpacklo_epi8(sub8, _mm_setzero_si128() );
	__m128i sub16_2 = _mm_unpackhi_epi8(sub8, _mm_setzero_si128() );
	
// alternative : (SSSE3)
//  __m128i plus_minus = _mm_setr_epi8( 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1, 1,-1,1,-1 );
//	or plus_minus == _mm_set1_epi16(0x1FF);
//	__m128i sub16_1 = _mm_maddubs_epi16(_mm_unpacklo_epi8(v1, v2), plus_minus);
//	__m128i sub16_2 = _mm_maddubs_epi16(_mm_unpackhi_epi8(v1, v2), plus_minus);

	__m128i squares32_1 = _mm_madd_epi16(sub16_1,sub16_1);
	__m128i squares32_2 = _mm_madd_epi16(sub16_2,sub16_2);
	
	__m128i squares32 = _mm_add_epi32(squares32_1,squares32_2);
	
	U32 ssd = hsum_epi32_sse2(squares32);
	
	return ssd;
}

#endif
	
static RADFORCEINLINE U32 unpacked_16x8_diff( const U8 * p1, const U8 * p2 )
{
	// @@ temp not aligned
	//RR_ASSERT( rrIsAlignedPointer(p1,16) );
	//RR_ASSERT( rrIsAlignedPointer(p2,16) );
		
	// SAD :
	    
	#ifdef __RADSSE2__

	__m128i v1 = _mm_loadu_si128((const __m128i *)p1);
	__m128i v2 = _mm_loadu_si128((const __m128i *)p2);
	// SAD instruction is so nice :
	__m128i sad2x64 = _mm_sad_epu8(v1,v2);
	// add two 16-bit partial sums in the 64-bit halves :
    return _mm_cvtsi128_si32(_mm_add_epi64(sad2x64, _mm_srli_si128(sad2x64, 8)));
    
    #else
    
    // experiment with a more correct diff for 3c :
	// -> this doesn't help
    
    // can do this with xor & blend in simd
    // xor v1^v2
    // if the == black bit is not equal, top bits will be on
    // if top bits are on, instead of SAD change to 12
    
    U32 ret =0;
    for LOOP(i,16)
    {
		int t = p1[i] - p2[i];
		t = RR_ABS(t);
		
		/*
		// putting the black as value 13 on the same scale as other indices is wrong
		// there should be no difference between 0->13 vs 6->13
		// any diff >= 7 should be the same
		if ( t >= 7 )
			t = 12;
		*/
		
		ret += t;
	}
	return ret;
    
    #endif
}

//===================================================
// AnyIndexD :
//  store SSD over a set of blocks
//	for any index dword used
//
// when you do a merge of blocks, AnyIndexD is just additive

struct AnyIndexD // 256 bytes
{
	//U32 D[16][4];
	// [selector][color] layout is more convenient for AnyIndexD_add SIMD
	U32 ssd[4][16];
};

struct index_vq_entry
{
	U8 unpacked_16x8[16]; // aligned 16 @@ temp not
	U32 indices;
	rrbool is3c;
	U32 distortion_sum;
	U32 count;
	U32 count_log2_count;
	S32 block_link; // linked list of blocks with these indices
	S32 merged_onto; // negative if not merged
	U32 best_distortion; // updated throughout loop
};

// meh , for experimenting with _load_ vs _loadu_ :
//RR_COMPILER_ASSERT( sizeof(index_vq_entry) == 16*3 );

struct index_vq_heap_entry
{
	int fm,to;
	F32 dj; // positive dj = good
	int pair_count_save;
};

// normal operator less will give a heap that pops largest first :
static bool operator < (const index_vq_heap_entry &lhs, const index_vq_heap_entry &rhs)
{
	return lhs.dj < rhs.dj;
}

/**

one way :

sort indices by count
only look for merges from higher index -> lower index
start at index 0, then only compute J when delta goes down
  at equal delta you would always prefer higher count
push all those to the heap

the heap is not full (it doesn't have all dests)
so now when you pop an item :
if "from" is already merged, skip it
if "to" is already merged, you can't skip it
  instead find what to was merged onto and make a merge candidate onto that

**/

static F32 index_merge_dj( const index_vq_entry & from, const index_vq_entry & to, S32 delta_D , int lambda) // int nblocks )
{
	/*
		
	LZ scheme J is
		D + lambda * R
	D ~ 28 * SAD
	D ~ 2 * SSD
	R ~ 16 * bits

	*/
	
	// diff = sum of SSD*2's
	//	no need to multiply by count as we've already accumulated across the N blocks
	F32 D = (F32)delta_D;
	
	
	// each entry has to send 32 bit raw indices + count * selection of those indices	
	//	same rate model as vq_codelen_palette_count
	
	// when you do a merge, the main rate savings is the 32 bits for one less index value
	//	but you also save some because the selection gets cheaper
	
	// @@ indexes actually take slightly less than 32 bits
	//	should tweak what that number is ; 30 ?
	
	// massively wrong simplified rate model
	// just count the 32 bits of raw index data saved
	// does not favor higher count targets like it should (their selection is cheaper)
	// while this is wrong I like that it favors low-count merges :
	//int R = (int)( 32 * vq_codelen_one_bit ); // == 512
		
	// more accurate rate using count probability model
	// -> YES , it's a nop on most images but a big win on red_blue
	/*
	F32 bits_from = from.count * ( -rrlog2(from.count/(F32)nblocks) ) + 32;
	F32 bits_to   = to.count * ( -rrlog2(to.count/(F32)nblocks) ) + 32;
	U32 merged_count = from.count + to.count;
	F32 bits_separate = bits_from + bits_to;
	F32 bits_merged   = merged_count * ( -rrlog2(merged_count/(F32)nblocks) ) + 32;
	RR_ASSERT( bits_merged < bits_separate );
	F32 delta_bits = bits_separate - bits_merged;
	RR_ASSERT( delta_bits >= 34.0 );
	delta_bits -= 2.0; // make the minimum 32
	int R = (int)( delta_bits * vq_codelen_one_bit );
	*/
	
	// log2tabled returns values scaled up by RR_LOG2TABLE_ONE
	// the denominator terms cancel out
	U32 merged_count = from.count + to.count;
	
	// count*log2count is cached in index_vq_entry :
	U32 log2one_from = from.count_log2_count;
	U32 log2one_to = to.count_log2_count;
	
	RR_ASSERT( log2one_from == from.count * log2tabled_bk_32(from.count) );
	RR_ASSERT( log2one_to   == to.count * log2tabled_bk_32(to.count) );
	
	U32 log2one_merged   = merged_count * log2tabled_bk_32(merged_count);
	U32 log2one_delta = log2one_from + log2one_to - log2one_merged;
	
	// scale from log2 one to vq_codelen fixed point :
	// @@ round? or just truncate
	RR_COMPILER_ASSERT( RR_LOG2TABLE_ONE_SHIFT >= vq_codelen_one_bit_log2 );
	//U32 rate_delta = log2one_delta >> (RR_LOG2TABLE_ONE_SHIFT - vq_codelen_one_bit_log2);
	F32 rate_delta = log2one_delta * (vq_codelen_one_bit / (F32)RR_LOG2TABLE_ONE);
	
	rate_delta += 30 * vq_codelen_one_bit; // 32 raw index bits - 2 bias
	
	RR_ASSERT( rate_delta >= 32 * vq_codelen_one_bit );

	F32 R  = rate_delta;
	
//prev BPB saved at vcdiff +1.0 : [ 12.688 ]

	// R and D are positive here
	//	D is the distortion cost of the merge, R is the rate savings
	//	(D can sometimes by slightly negative when an index change actually helps a block)
	
	F32 dJ = vq_J_10(- D,R,lambda);
	// dJ > 0 is a good merge
	//	the rate savings is enough to justify the distortion penalty
	//return (int)(dJ + 0.5);
	return dJ;
}

//=======================
// AnyIndexD

#ifdef __RADX86__

//static RADFORCEINLINE Vec128 DeltaSqrRGBA_SSE2(Vec128 x_br16, Vec128 x_ga16, Vec128 y_br16, Vec128 y_ga16)
static RADFORCEINLINE Vec128 DeltaSqrRGBA_SSE2(
	const Vec128 & x_br16, const Vec128 & x_ga16, const Vec128 & y_br16, const Vec128 & y_ga16)
{
	Vec128 diff_br16 = _mm_sub_epi16(y_br16, x_br16); // db, dr
	Vec128 diff_ga16 = _mm_sub_epi16(y_ga16, x_ga16); // dg, da
	
	// @@@@ NOTE :
	//	we currently do not do a penalty like rrColor32BGRA_DeltaSqrRGBA_1BT ?
	// -> could easily scale up diff_ga16 here
	// double the A difference in diff_ga16 before squaring -> 4* in the deltasqr
	// -> not necessary
	// I only consider index merges where 1bt_mask is the same
	//	note that AnyIndex can be filled with possible index changes that invalidate 1BT preservation
	//	but those values will never be looked up!
	// it can only look up indices where the A difference is 0
	// ALTERNATIVE :
	// could do the A doubling here with one mullo
	//	then you could skip the 1bt mask check and let this act as the rejector

	Vec128 sumsq_br32 = _mm_madd_epi16(diff_br16, diff_br16); // db*db + dr*dr
	Vec128 sumsq_ga32 = _mm_madd_epi16(diff_ga16, diff_ga16); // dg*dg + da*da
	
	return _mm_add_epi32(sumsq_br32, sumsq_ga32); // db*db + dg*dg + dr*dr + da*da
}

//	basically the same thing FindIndices does without the MIN
static void AnyIndexD_add(AnyIndexD * aid,const rrColorBlock4x4 & block,rrColor32BGRA palette[4])
{
	// Load all 16 pixels worth of data, and the palette
	Vec128 pal  = load128u(palette);
	Vec128 mask_br = _mm_set1_epi16(0xff);
	// change 08-18-2020: ? no need for special masking
	//	in non-Alpha mode all the A's are 255 so their delta is zero
	//Vec128 mask_ga = _mm_set1_epi32((mode == rrDXT1PaletteMode_Alpha) ? 0xff00ff : 0xff); // in non-alpha mode, zero all alpha values -> ignored
	//Vec128 mask_ga = _mm_set1_epi32(0xff); // in non-alpha mode, zero all alpha values -> ignored
	Vec128 mask_ga = _mm_set1_epi16(0xff);

	Vec128 pal_br16 = _mm_and_si128(pal, mask_br);
	Vec128 pal_ga16 = _mm_and_si128(_mm_srli_epi32(pal, 8), mask_ga);

	for (int c = 0; c < 16; c += 4)
	{
		// One row's worth of data
		Vec128 row = load128u(block.colors + c);
		Vec128 row_br16 = _mm_and_si128(row, mask_br);
		Vec128 row_ga16 = _mm_and_si128(_mm_srli_epi32(row, 8), mask_ga);

		Vec128 d0 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0x00), _mm_shuffle_epi32(pal_ga16, 0x00));
		Vec128 d1 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0x55), _mm_shuffle_epi32(pal_ga16, 0x55));
		Vec128 d2 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0xaa), _mm_shuffle_epi32(pal_ga16, 0xaa));
		Vec128 d3 = DeltaSqrRGBA_SSE2(row_br16, row_ga16, _mm_shuffle_epi32(pal_br16, 0xff), _mm_shuffle_epi32(pal_ga16, 0xff));

		/*
		// need a 4x4 transpose
		//	if we wanted [16][4] layout :
		__m128 v0 = _mm_castsi128_ps(d0);
		__m128 v1 = _mm_castsi128_ps(d1);
		__m128 v2 = _mm_castsi128_ps(d2);
		__m128 v3 = _mm_castsi128_ps(d3);
		_MM_TRANSPOSE4_PS(v0,v1,v2,v3);
		*/
		
		// more convenient to store [index][color] layout :
		
		// add on 4x4 matrix of ints :
		_mm_storeu_si128( (__m128i *)&(aid->ssd[0][c]), _mm_add_epi32( _mm_loadu_si128((__m128i *)&(aid->ssd[0][c])), d0) );
		_mm_storeu_si128( (__m128i *)&(aid->ssd[1][c]), _mm_add_epi32( _mm_loadu_si128((__m128i *)&(aid->ssd[1][c])), d1) );
		_mm_storeu_si128( (__m128i *)&(aid->ssd[2][c]), _mm_add_epi32( _mm_loadu_si128((__m128i *)&(aid->ssd[2][c])), d2) );
		_mm_storeu_si128( (__m128i *)&(aid->ssd[3][c]), _mm_add_epi32( _mm_loadu_si128((__m128i *)&(aid->ssd[3][c])), d3) );
	}
}

#else

static void AnyIndexD_add(AnyIndexD * aid,const rrColorBlock4x4 & colors,rrColor32BGRA palette[4])
{
	for LOOP(c,16)
	{
		const rrColor32BGRA & cur = colors.colors[c];
		for LOOP(p,4)
		{
			aid->ssd[p][c] += rrColor32BGRA_DeltaSqrRGBA_1BT(cur,palette[p]);
		}
	}
}

#endif

static U32 block_link_to_AnyIndexD(const index_vq_entry * entry, const rrDXT1_VQ_Block * blocks, AnyIndexD * aid, rrDXT1PaletteMode pal_mode )
{
	int link = entry->block_link;
	U32 link_count = 0;
	
	// add SSD's into aid from each block
	RR_ZERO(*aid);
	
	while( link >= 0 )
	{
		#ifdef DO_bc1_indices_vq_reduce_CHANGE_INDICES
		RR_ASSERT( blocks[link].cur.indices == entry->indices );
		#endif

		const rrColorBlock4x4 & colors = blocks[link].colors;
		//pals[link_count].block_activity_scale = blocks[link].block_activity_scale;
		
		U32 ep = blocks[link].cur.endpoints;	
		
		if ( pal_mode == rrDXT1PaletteMode_Alpha )
			RR_ASSERT( DXT1_OneBitTransparent_Mask_Same(blocks[link].block_1bt_mask,ep,entry->indices) );

		rrColor32BGRA palette[4];
		DXT1_ComputePalette(ep,palette,pal_mode);
		
		AnyIndexD_add(aid,colors,palette);
				
		link_count++;
		link = blocks[link].vq_entry_link;
	}
	RR_ASSERT( link_count == entry->count );
	return entry->count;	
}

static U32 AnyIndexD_lookup(const AnyIndexD * aid, U32 in_indices)
{
	U32 indices = in_indices;
	
	U32 ssd = 0;
	for LOOP(c,16)
	{
		U32 cur = indices&3; indices>>=2;
		//ssd += aid->D[c][cur];
		ssd += aid->ssd[cur][c];
	}
	
	U32 D = 2 * ssd;
	
	return D;
}

// for an AnyIndexD, find the indices that minimize SSD
//	and the D value
static U32 AnyIndexD_find_indices(const AnyIndexD * aid, U32 * pD)
{
	// not used in simd
	// see AnyIndexD_add_then_find_indices
	
	U32 indices = 0;
	U32 ssd = 0;
	for LOOP(c,16)
	{
		U32 d0 = (aid->ssd[0][c] << 2) + 0;
		U32 d1 = (aid->ssd[1][c] << 2) + 1;
		U32 d2 = (aid->ssd[2][c] << 2) + 2;
		U32 d3 = (aid->ssd[3][c] << 2) + 3;
		
		U32 md = RR_MIN4(d0,d1,d2,d3);
		//U32 cur_index = md & 3;
		//indices += (cur_index)<<c;
		
		ssd += (md>>2);
		
		indices >>= 2;
		indices |= (md<<30);
	}
	
	*pD = 2 * ssd;
	
	return indices;
}

static void AnyIndexD_add(AnyIndexD * to, const AnyIndexD & fm)
{
	// not used in simd
	// see AnyIndexD_add_then_find_indices
	
	U32 * pto = to->ssd[0];
	const U32 * pfm = fm.ssd[0];
	
	for LOOP(i,64) // 4*16
	{
		pto[i] += pfm[i];
	}
}

#ifdef __RADSSE2__
// SSE4 has _mm_min_epi32 & _mm_min_epu32
static RADFORCEINLINE __m128i _mm_min_epi32_sse2(__m128i a, __m128i b)
{
	__m128i a_greater = _mm_cmpgt_epi32(a, b);
	return _mm_or_si128(_mm_and_si128(b, a_greater), _mm_andnot_si128(a_greater, a));
}
#endif

// return the D of the best indices for the pair {1,2}
//	same as adding aid1+aid2 then doing find_indices
static U32 AnyIndexD_best_index_pair_D(const AnyIndexD * aid1, const AnyIndexD * aid2)
{
	#ifdef __RADSSE2__
	
	__m128i error_sum = _mm_setzero_si128();
	
	for (int c=0;c<16;c+=4)
	{
		__m128i v0 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&aid1->ssd[0][c])),
			_mm_loadu_si128((const __m128i *)(&aid2->ssd[0][c])) );
		__m128i v1 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&aid1->ssd[1][c])),
			_mm_loadu_si128((const __m128i *)(&aid2->ssd[1][c])) );
		__m128i v2 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&aid1->ssd[2][c])),
			_mm_loadu_si128((const __m128i *)(&aid2->ssd[2][c])) );
		__m128i v3 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&aid1->ssd[3][c])),
			_mm_loadu_si128((const __m128i *)(&aid2->ssd[3][c])) );
	
		__m128i mins = _mm_min_epi32_sse2(
			_mm_min_epi32_sse2(v0,v1),	
			_mm_min_epi32_sse2(v2,v3));	
	
		error_sum = _mm_add_epi32(error_sum, mins);
	}
	
	// Horizontal reduction for final error sum
	error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0xb1)); // add to one away
	error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0x4e)); // add to two away
	U32 ssd = _mm_cvtsi128_si32(error_sum);
		
	U32 D = 2 * ssd;
	
	return D;
	
	#else
	
	U32 ssd = 0;
	for LOOP(c,16)
	{
		U32 d0 = aid1->ssd[0][c] + aid2->ssd[0][c];
		U32 d1 = aid1->ssd[1][c] + aid2->ssd[1][c];
		U32 d2 = aid1->ssd[2][c] + aid2->ssd[2][c];
		U32 d3 = aid1->ssd[3][c] + aid2->ssd[3][c];
		
		U32 md = RR_MIN4(d0,d1,d2,d3);
		
		ssd += md;
	}
	
	U32 D = 2 * ssd;
	
	return D;
	
	#endif
}
		
static U32 AnyIndexD_add_then_find_indices(AnyIndexD * to, const AnyIndexD & fm, U32 * pD)
{

	#ifdef __RADSSE2__
	
	/*
	AnyIndexD test = *to;
	AnyIndexD_add(&test,fm);
	U32 test_D;
	U32 test_indices = AnyIndexD_find_indices(&test,&test_D);
	*/
	
	// very similar to DXT1_FindIndices
	//	except we fetch two precompute vecs of SSDs
	
	__m128i error_sum = _mm_setzero_si128();
	__m128i const32_1 = _mm_set1_epi32(1);
	__m128i const32_2 = _mm_set1_epi32(2);
	__m128i const32_3 = _mm_set1_epi32(3);
	__m128i magic_mul = _mm_set1_epi16(0x4080);
	U32 indices = 0;
	
	for (int c=0;c<16;c+=4)
	{
		__m128i d0 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&to->ssd[0][c])),
			_mm_loadu_si128((const __m128i *)(&fm.ssd[0][c])) );
		__m128i d1 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&to->ssd[1][c])),
			_mm_loadu_si128((const __m128i *)(&fm.ssd[1][c])) );
		__m128i d2 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&to->ssd[2][c])),
			_mm_loadu_si128((const __m128i *)(&fm.ssd[2][c])) );
		__m128i d3 = _mm_add_epi32( 
			_mm_loadu_si128((const __m128i *)(&to->ssd[3][c])),
			_mm_loadu_si128((const __m128i *)(&fm.ssd[3][c])) );
	
		// store the sums :
		_mm_storeu_si128( (__m128i *)(&to->ssd[0][c]),d0);
		_mm_storeu_si128( (__m128i *)(&to->ssd[1][c]),d1);
		_mm_storeu_si128( (__m128i *)(&to->ssd[2][c]),d2);
		_mm_storeu_si128( (__m128i *)(&to->ssd[3][c]),d3);
		
		__m128i best = _mm_slli_epi32(d0, 2);
		best = _mm_min_epi32_sse2(best, _mm_add_epi32(_mm_slli_epi32(d1, 2), const32_1));
		best = _mm_min_epi32_sse2(best, _mm_add_epi32(_mm_slli_epi32(d2, 2), const32_2));
		best = _mm_min_epi32_sse2(best, _mm_add_epi32(_mm_slli_epi32(d3, 2), const32_3));

		// Now have 2b best color index in low bits of every lane
		// and squared error in top 30 bits; accumulate error first
		error_sum = _mm_add_epi32(error_sum, _mm_srli_epi32(best, 2));
		
		// Now extract index bits fancily (NOTE slightly wasteful, should do this for two rows at once)
		__m128i best_inds = _mm_and_si128(best, const32_3);
		__m128i packed = _mm_packs_epi32(best_inds, _mm_setzero_si128()); // now have 16-bit fields
		__m128i magicked = _mm_mullo_epi16(packed, magic_mul); // move bit 0 of index into bit 7 of 16b lane, and bit 1 of index into bit 15
		U32 bits = _mm_movemask_epi8(magicked); // and poof, movemask does the rest

		indices = (indices >> 8) | (bits << 24);
	}
	
	// Horizontal reduction for final error sum
	error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0xb1)); // add to one away
	error_sum = _mm_add_epi32(error_sum, _mm_shuffle_epi32(error_sum, 0x4e)); // add to two away
	U32 ssd = _mm_cvtsi128_si32(error_sum);
		
	*pD = 2 * ssd;
	
	//RR_ASSERT( *pD == test_D );
	RR_ASSERT( AnyIndexD_lookup(to,indices) == *pD );
	
	return indices;	
	
	#else
	
	AnyIndexD_add(to,fm);
	return AnyIndexD_find_indices(to,pD);
	
	#endif
}

static void bc1_indices_vq_reduce(vector<dword_and_count> * indices_out, const vector<dword_and_count> & total_indices_in, 
							int lambda, int nblocks_total ,
							rrDXT1_VQ_Block * blocks_in,
							rrDXT1PaletteMode pal_mode)
{
	SIMPLEPROFILE_SCOPE(indices_vq_reduce);
	
	RR_ASSERT( lambda > 0 );

	const rrDXT1_VQ_Block * blocks = blocks_in;

	// input indices are uniqued already
	// indices_in should be sorted by count already :
	//RR_ASSERT( indices_in[0].count >= indices_in.back().count );
	int indices_in_count = total_indices_in.size32();
	
	vector<index_vq_entry> entries;
	entries.reserve( indices_in_count );
	
	indices_out->reserve(indices_in_count);
	indices_out->clear();
	U32 total_count_out = 0;
	
	vector<U32> indices_nc_raw;
	vector<dword_and_count> indices_nc_sorted;
	indices_nc_sorted.reserve(indices_in_count);
	
	entries.clear();
	
	// build separate entries lists for 4c then for 3c
	// but then just stick them all in entries[]
	//	and do the N^2 merge letting them merge together
	//	this seems to be better than keeping them segregated
	for LOOP(loop_is3c,2)
	{
		
		indices_nc_raw.clear();
		indices_nc_sorted.clear();
		
		
		for LOOP(b,nblocks_total)
		{
			// filter for blocks that are in the desired 3c4c mode :
			bool cur_is3c = ! DXT1_Is4Color( blocks[b].cur, pal_mode );
			if ( (int)cur_is3c != loop_is3c )
				continue;
			
			U32 indices = blocks[b].cur.indices;
			indices_nc_raw.push_back(indices);
		}
		
		// number of blocks of this 3c4c mode :
		int nblocks_nc = indices_nc_raw.size32();
		nblocks_nc;
		//rrprintfvar(nblocks_nc);
		if ( indices_nc_raw.empty() ) // can happen with 3c
			continue;
		
		sort_and_count_uniques(&indices_nc_sorted,indices_nc_raw);
		sort_dword_and_count_compare_count_highest_first(&indices_nc_sorted);
		RR_ASSERT( indices_nc_sorted[0].count >= indices_nc_sorted.back().count );
		
		int cur_entries_count = indices_nc_sorted.size32();
		//rrprintfvar(cur_entries_count);
		int entries_base = entries.size32();
		entries.resize(entries_base+cur_entries_count);
		index_vq_entry * cur_entries = entries.data() + entries_base;
		
		U32 total_count_in = 0;
		for LOOPVEC(i,indices_nc_sorted)
		{
			U32 indices = indices_nc_sorted[i].dw;		
			int count = indices_nc_sorted[i].count;	
			
			cur_entries[i].is3c = loop_is3c;
			cur_entries[i].indices = indices;
			cur_entries[i].count = count;		
			cur_entries[i].count_log2_count = count * log2tabled_bk_32( count );
			if ( loop_is3c )
				bc1_indices_3c_unpack_to_16x8(cur_entries[i].unpacked_16x8,indices);
			else
				bc1_indices_4c_unpack_to_16x8(cur_entries[i].unpacked_16x8,indices);
			cur_entries[i].merged_onto = -1;
			cur_entries[i].block_link = -1;
			total_count_in += cur_entries[i].count;
		}
		RR_ASSERT( total_count_in == (U32)nblocks_nc );
		
		// make block linked lists :
		//	each index has list of all blocks that use it
		for LOOP(b,nblocks_total)
		{
			// filter for blocks that are in the desired 3c4c mode :
			bool cur_is3c = ! DXT1_Is4Color( blocks[b].cur, pal_mode );
			if ( (int)cur_is3c != loop_is3c )
				continue;
				
			U32 indices = blocks[b].cur.indices;
			
			// find in cur_entries :
			int entry_i = entries_base;
			while( entries[entry_i].indices != indices ) entry_i++;

			// add to linked list :
			const_cast<rrDXT1_VQ_Block *>(blocks)[b].vq_entry_link = entries[entry_i].block_link;
			entries[entry_i].block_link = b;
		}
	
	} // 3c4c loop
	
	int num_entries = entries.size32();
	//rrprintfvar(num_entries);
	
	// use unpacked_16x8_diff as an early out for speed?
	// so a valid merge has (diff*count) <= 4*lambda
	// this should be higher than we think the limit is
	//	I want this to only rule out candidates that are almost certainly junk
	//	use the block distortion metric for real decisions, not index distance
	// @@ is this high enough? try it higher
	//	 when you bump it up it should not help rate much at all
	// -> this is a big speed savings
	//	 pretty decent way to trade quality for speed at the moment
	//	-> be careful and test this on a variety of images!

	U32 conservative_index_diff_limit = (EARLY_OUT_INDEX_DIFF_TIMES_LAMBDA*lambda + EARLY_OUT_INDEX_DIFF_PLUS_CONSTANT)/10; // lambda scaled up by 10
	
	// if distortion increases by this much, it won't be a J win
	S32 rate_savings_32_bits = ( 32 * vq_codelen_one_bit ); // 512
	F32 max_distortion_increase_f = vq_J_10(0,(F32)rate_savings_32_bits,lambda);
	//S32 max_distortion_increase = (S32)(max_distortion_increase_f + 0.5f);
	S32 max_distortion_increase = (S32)(max_distortion_increase_f + 1.5f);
	
	vector<index_vq_heap_entry> heap;
	heap.reserve( num_entries*16 );

	// NOTE: keeping AIDs in a separate array since they're quite large and quite cold;
	// keeping them in the main index_vq_entry really tanks perf of vq_reduce pass
	vector<AnyIndexD> aids;

	{
	SIMPLEPROFILE_SCOPE(indices_vq_reduce_make_heap); // all the time is here

	aids.resize(num_entries);
		
	for LOOP(fm,num_entries)
	{
		block_link_to_AnyIndexD(&entries[fm],blocks,&aids[fm],pal_mode);
		
		entries[fm].distortion_sum = AnyIndexD_lookup(&aids[fm],entries[fm].indices);

		// only bother looking if within max_distortion_increase
		entries[fm].best_distortion = entries[fm].distortion_sum + max_distortion_increase;
	}
	
	// process chunks of the entries that fit in L1 to minimize cache thrashing
	const int entry_chunk_size = (16 * 1024) / sizeof(entries[0]);

	for (int to_chunk_begin = 0; to_chunk_begin < num_entries; to_chunk_begin += entry_chunk_size)
	{
		const int to_chunk_end = RR_MIN(to_chunk_begin + entry_chunk_size, num_entries);

		// this is N^2 on the number of unique indices
		//	(actually N*M with N = # of unique indices and M = # of blocks)
		for LOOP(fm,num_entries)
		{
			// no need to consider a flat changing into non-flat
			if ( indices_are_flat(entries[fm].indices) )
				continue;

			// verify links :
			//RR_ASSERT( assert_block_link_check(&entries[fm],blocks) );

			// get my starting distortion with initial indices :
			//	(I may have this in blocks[] already)

			const AnyIndexD & aid = aids[fm];
			//block_link_to_AnyIndexD(&entries[fm],blocks,&aid,pal_mode);

			U32 fm_base_distortion = entries[fm].distortion_sum;
			U32 best_distortion = entries[fm].best_distortion;

			U32 fm_1bt_mask = DXT1_OneBitTransparent_Mask_FromIndices( entries[fm].is3c, entries[fm].indices );

			// try all merge targets
			for (int to = to_chunk_begin; to < to_chunk_end; to++)
			{
				//if ( fm == to ) continue; // now caught below
				//RR_ASSERT( entries[fm].indices != entries[to].indices );

				// this also catches fm == to :
				if ( entries[fm].indices == entries[to].indices )
				{
					RR_ASSERT( fm == to || entries[fm].is3c != entries[to].is3c );
					// indices equality can happen for 3c to 4c
					// error there would be zero, don't allow it
					continue;
				}
				

				//*
				// no 4c -> 3c , but 3c -> 4c is allowed
				// yes helps a bit
				if ( entries[to].is3c && ! entries[fm].is3c )
					break;
					// this can be a break (rather than continue) because 3c's are always at the end.
				/**/

				// @@ exclude flat indices from merge-to candidates for now
				//  -> doesn't make much difference if you do or not
				//	   (assuming 2nd pass picks them up)
				if ( indices_are_flat(entries[to].indices) )
					continue;

				if ( pal_mode == rrDXT1PaletteMode_Alpha )
				{
					// no merges that change 1bt bits :
					U32 to_1bt_mask = DXT1_OneBitTransparent_Mask_FromIndices( entries[to].is3c, entries[to].indices );
					if ( fm_1bt_mask != to_1bt_mask )
						continue;
				}


				#if 0
				// no need to consider high count -> low count merges
				// doesn't do anything for speed really
				if ( entries[fm].count >= 3 && entries[to].count == 1 )
					break;
				if ( entries[fm].count >= 5 && entries[to].count <= 2 )
					break;
				#endif

				#if 1
				// fast early out with index diff :
				//
				// on "red_blue" it's much better to NOT do this
				//	(and doesn't hurt any speed)
				// on the rest of the images this is good (quality nop and speed savings)

				U32 index_diff = unpacked_16x8_diff(entries[fm].unpacked_16x8, entries[to].unpacked_16x8 );

				// meh
				//if ( entries[to].is3c != entries[fm].is3c ) index_diff += 2;

				index_diff *= entries[fm].count;
				if ( index_diff > conservative_index_diff_limit )
					continue;
				#endif

				// all the time is here :
				//SIMPLEPROFILE_SCOPE(indices_vq_reduce_make_heap_inner);

				// distortion of changing FM blocks to TO indices :
				//	this is the D of an asymmetric fm->to move
				// if it is best we will re-evaluate as a {fm,to} symmetric pair move
				U32 distortion = AnyIndexD_lookup(&aid,entries[to].indices);
				// max_distortion_increase no longer used for early out in AnyIndexD_lookup

				// typically the original distortion_sum is quite small
				//	and the modified distortion_sum is huge
				//	can early out as soon as the difference will make dj > 0
				//	 -> quite quick, that's a strong early out
				//		well it's strong at low lambda but get weaker as lambda goes up

				// entries are sorted by count so I only have to look at J when D gets better
				//  (this is an approximation, tests indicate it's okay)
				//		(it's exact for the first merge step, but not later as things get merged up)
				//	(to be exact you should go ahead and add everything with dj > 0 to the heap)
				//	(if we only did one merge this would be fine, it would not be an approximation
				//	 the issue is that later on, all your desired merge targets may be gone
				//	 so the best thing left may be one of the ones that we ruled out here)
				if ( distortion <= best_distortion )
				{
					best_distortion = distortion;

					S32 delta_D = distortion - fm_base_distortion;
					RR_ASSERT( delta_D <= max_distortion_increase ); // gauranteed by initial value of best_distortion

					// @@ now compute delta_D if we found the best indices over the set :
					U32 before_pair_D = fm_base_distortion + entries[to].distortion_sum;
					U32 after_D = AnyIndexD_best_index_pair_D(&aid,&aids[to]);
					S32 new_index_delta_D = after_D - before_pair_D;
					RR_ASSERT( new_index_delta_D <= delta_D );
					delta_D = new_index_delta_D;

					F32 dj = index_merge_dj( entries[fm], entries[to], delta_D , lambda); //,nblocks);
					if ( dj > 0 )
					{
						// make a heap entry :
						heap.push_back();
						heap.back().fm = fm;
						heap.back().pair_count_save = entries[fm].count + entries[to].count;
						heap.back().to = to;
						heap.back().dj = dj;
					}
				}
			}

			entries[fm].best_distortion = best_distortion; // update state
		}
	}

	make_heap(heap.begin(),heap.end());
	}
	
	{
	//it's weird how fast this part is :
	//SIMPLEPROFILE_SCOPE(indices_vq_reduce_pop_heap);
	
	// heap is sorted by dj, largest first
	while( ! heap.empty() )
	{
		index_vq_heap_entry heap_entry = heap[0];
		popped_heap(heap.begin(),heap.end());
		heap.pop_back();
		
		// if from entry is gone, ignore me
		int fm = heap_entry.fm;
		if ( entries[ fm ].merged_onto >= 0 )
			continue;

		bool dirty = false;
		
		int to = heap_entry.to;
		if ( entries[ to ].merged_onto >= 0 )
		{
			// if my dest was merged, chase where he went
			do
			{
				to = entries[ to ].merged_onto;
			} while( entries[to].merged_onto >= 0 );
			if ( to == fm ) // I'm considering A->C , but C already did C->A or C->B->A
				continue;
				
			dirty = true;
		}
		
		// pair count changes makes the merge dirty
		//	block set must be the same we originally considered
		dirty = dirty || ( heap_entry.pair_count_save != (int)(entries[ fm ].count + entries[ to ].count) );
		
		if ( dirty )
		{
			// make a new candidate for me to merge onto merged_to

			U32 index_diff = unpacked_16x8_diff(entries[fm].unpacked_16x8, entries[to].unpacked_16x8 );
			index_diff *= entries[fm].count;
			if ( index_diff > conservative_index_diff_limit )
				continue;

			U32 distortion_base = entries[fm].distortion_sum + entries[to].distortion_sum;
			U32 after_D = AnyIndexD_best_index_pair_D(&aids[fm],&aids[to]);
			S32 delta_D = after_D - distortion_base;
			//if ( delta_D <= max_distortion_increase ) // no, hurts
			{			
				F32 dj = index_merge_dj( entries[fm], entries[to], delta_D , lambda); //,nblocks);
				if ( dj > 0 )
				{
					// make a heap entry :
					heap.push_back();
					heap.back().fm = fm;
					heap.back().pair_count_save = entries[fm].count + entries[to].count;
					//heap.back().fm_distortion_onto = distortion;
					heap.back().to = to;
					heap.back().dj = dj;
					push_heap(heap.begin(),heap.end());
				}
			}
			
			continue;
		}
		
		// fm and to are both alive
		// do the merge
		
		entries[fm].merged_onto = to;
		entries[to].count += entries[fm].count;
		
		entries[to].count_log2_count = entries[to].count * log2tabled_bk_32( entries[to].count );
		
		// merge the block linked list :
		// all [fm] indices change to [to]
		// find the tail of the link to connect them
		{
			int link = entries[fm].block_link;
			U32 link_count = 0;
			for(;;)
			{
				link_count++;
				RR_ASSERT( link_count <= entries[fm].count );
				if ( blocks[link].vq_entry_link < 0 )
				{
					// end of the chain
					// link the [to] chain onto it :
					const_cast<rrDXT1_VQ_Block *>(blocks)[link].vq_entry_link = entries[to].block_link;
					entries[to].block_link = entries[fm].block_link;
					break;
				}
				link = blocks[link].vq_entry_link;
			}
			RR_ASSERT( link_count == entries[fm].count );
		}
		
		U32 new_D;
		U32 new_indices = AnyIndexD_add_then_find_indices(&aids[to],aids[fm],&new_D);
		entries[to].distortion_sum = new_D;
		entries[to].indices = new_indices;
		// [to] is made dirty by to count changing
		
	}
	} //profile scope
	
	// scan out just the un-merged entries :
	// indices_out and total_count_out are accumulated from both the 3c and 4c loop
	for LOOPVEC(entry_i,entries)
	{
		if ( entries[entry_i].merged_onto >= 0 ) continue;
		
		RR_ASSERT( entries[entry_i].count > 0 );
		
		#ifdef DO_bc1_indices_vq_reduce_CHANGE_INDICES
		// change the actual block indices :
		int link = entries[entry_i].block_link;
		U32 link_count = 0;
		while( link >= 0 )
		{
			link_count++;
			const_cast<rrDXT1_VQ_Block *>(blocks)[link].cur.indices = entries[entry_i].indices;
			link = blocks[link].vq_entry_link;
		}
		RR_ASSERT( link_count == entries[entry_i].count );
		#endif

		indices_out->push_back();
		indices_out->back().count = entries[entry_i].count;
		indices_out->back().dw = entries[entry_i].indices;
		total_count_out += entries[entry_i].count;
	}
	
	RR_ASSERT( total_count_out == (U32)nblocks_total );
	
	// because of 3c/4c split indices_out can have dupe entries
	//	need to merge those again :
	
	sort_dword_and_count_compare_dword(indices_out);
	
	// condense :
	//	this is more than just the 3c/4c dupes
	//	because of the find-indexes-after-merge
	//	we can accidentally combine sets
	//	eg. {A,B}->C and {D,E}->C
	//	you wind up with two entries for {C}
	//	ideally we'd find and merge those as we go
	int toi = 1;
	S32 indices_out_size = indices_out->size32();
	for(int fmi=1;fmi<indices_out_size;fmi++)
	{
		if ( indices_out->at(fmi).dw == indices_out->at(toi-1).dw )
		{		
			indices_out->at(toi-1).count += indices_out->at(fmi).count;			
		}
		else
		{
			indices_out->at(toi) = indices_out->at(fmi);
			toi++;
		}
	}
	indices_out->resize(toi);	
	
	//indices_out now has the unique indices & their count but is not yet sorted (by count)
			
	//int bc1_indices_vq_reduce_num_out = indices_out->size32();
	//rrprintfvar(bc1_indices_vq_reduce_num_out);
}

RR_NAMESPACE_END
