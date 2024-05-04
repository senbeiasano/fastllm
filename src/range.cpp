/*
range.c - v1.01

Copyright 2020 Alec Dee - MIT license - SPDX: MIT
deegen1.github.io - akdee144@gmail.com
*/

#include "range.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

u32 findsym(u32* prob,u32 code)
{
	// Find the symbol who's cumulative interval encapsulates the given code.
	u32 sym=0;
	while (prob[sym+1]<=code) {sym++;}
	return sym;
}

void range_decode(u8* buffer, int insize, void* out, int outsize, int l2_num, u32* prob) {
	
    int inpos = 0, outpos = 0;
	rangeencoder* dec=rccreate(RC_DECODE);
    while (outpos < outsize) {
		u32 decode=rcdecode(dec, prob[l2_num]);
		if (decode != ((u32)-1)) {
			// We are ready to decode a symbol.
			u32 sym = findsym(prob, decode);
			rcscale(dec, prob[sym], prob[sym + 1], prob[l2_num]);
			// incprob(prob,sym);
			// fputc(sym, out);
			if (l2_num <= 256) {
				((u_int8_t*)out)[outpos] = u_int8_t(sym);
			} else {
				((u_int16_t*)out)[outpos] = u_int16_t(sym);
			}
			outpos++;
		} else if (inpos < insize) {
			// We need more input data.
			rcadd(dec, buffer[inpos]);
			inpos++;
		} else {
			// Signal that we have no more input data.
			rcfinish(dec);
		}
	}
}

//---------------------------------------------------------------------------------
// Management
//---------------------------------------------------------------------------------

rangeencoder* rccreate(u32 mode)
{
	rangeencoder* rc=(rangeencoder*)malloc(sizeof(rangeencoder));
	rcinit(rc,mode);
	return rc;
}

void rcfree(rangeencoder* rc)
{
	free(rc);
}

void rcinit(rangeencoder* rc,u32 mode)
{
	// If encoding=True, intialize and support encoding operations. Otherwise, support
	// decoding operations. More state bits will give better encoding accuracy at the
	// cost of speed.
	u32 bits=32;
	assert(0<bits && bits<33);
	assert(mode==RC_ENCODE || mode==RC_DECODE);
	assert((RC_MASK&(RC_MASK+1))==0);
	assert(sizeof(rc->norm)*8*2<=RC_MASK+1);
	memset(rc,0,sizeof(rangeencoder));
	rc->flags=mode;
	rc->bits=bits;
	rc->norm=1ULL<<bits;
	rc->half=rc->norm/2;
	rc->range=mode==RC_ENCODE?rc->norm:1;
}

//---------------------------------------------------------------------------------
// Encoding
//---------------------------------------------------------------------------------

void rcencode(rangeencoder* rc,u32 intlow,u32 inthigh,u32 intden)
{
	// Encode an interval into the range.
	assert((rc->flags&(RC_ENCODE|RC_FINISHED))==RC_ENCODE);
	assert(intlow<inthigh && inthigh<=intden && (u64)intden<=rc->half+1);
	assert(rc->qlen<=RC_MASK/2);
	u32* qcount=rc->qcount;
	u32 qpos=rc->qpos;
	u32 qlen=rc->qlen;
	// Shift the range.
	u64 half=rc->half;
	u64 low=rc->low;
	u64 range=rc->range;
	while (range<=half)
	{
		// Push a settled state bit the to queue.
		u32 dif=qpos^((low&half)!=0);
		qpos=(qpos+(dif&1))&RC_MASK;
		qlen+=qcount[qpos]++==0;
		low+=low;
		range+=range;
	}
	u64 norm=rc->norm;
	low&=norm-1;
	// Scale the range to fit in the interval.
	u64 off=(range*intlow)/intden;
	low+=off;
	range=(range*inthigh)/intden-off;
	// If we need to carry.
	if (low>=norm)
	{
		// Propagate a carry up our queue. If the previous bits were 0's, flip one to 1.
		// Otherwise, flip all 1's to 0's.
		low-=norm;
		// If we're on an odd parity, align us with an even parity.
		u32 odd=qpos&1;
		u32 ones=qcount[qpos]&-odd;
		qcount[qpos]-=ones;
		qpos-=odd;
		// Even parity carry operation.
		qcount[qpos]--;
		u32 inc=qcount[qpos]==0?-1:1;
		qpos=(qpos+inc)&RC_MASK;
		qcount[qpos]++;
		// Length correction.
		qlen+=inc;
		qlen+=qlen<=odd;
		// If we were on an odd parity, add in the 1's-turned-0's.
		qpos=(qpos+odd)&RC_MASK;
		qcount[qpos]+=ones;
	}
	rc->low=low;
	rc->range=range;
	rc->qpos=qpos;
	rc->qlen=qlen;
}

void rcfinish(rangeencoder* rc)
{
	// Flush the remaining data from the range.
	if ((rc->flags&RC_FINISHED)!=0)
	{
		return;
	}
	rc->flags|=RC_FINISHED;
	if ((rc->flags&RC_ENCODE)!=0)
	{
		assert(rc->qlen<=RC_MASK/2);
		// We have no more data to encode. Flush out the minimum number of bits necessary
		// to satisfy low<=flush+1's<low+range. Then pad with 1's till we're byte aligned.
		u32* qcount=rc->qcount;
		u32 qpos=rc->qpos;
		u32 qlen=rc->qlen;
		u64 low=rc->low;
		u64 norm=rc->norm;
		u64 dif=low^(low+rc->range);
		while (dif<norm)
		{
			low+=low;
			dif+=dif;
			u32 flip=qpos^((low&norm)!=0);
			qpos=(qpos+(flip&1))&RC_MASK;
			qlen+=qcount[qpos]++==0;
		}
		// Calculate how many bits need to be appended to be byte aligned.
		u32 pad=0;
		for (u32 i=0;i<qlen;i++)
		{
			pad-=qcount[(qpos-i)&RC_MASK];
		}
		pad&=7;
		// If we're not byte aligned.
		if (pad!=0)
		{
			// Align us with an odd parity and add the pad. Add 1 to qlen if qpos&1=0.
			qlen-=qpos;
			qpos|=1;
			qlen+=qpos;
			qcount[qpos]+=pad;
		}
		rc->qpos=qpos;
		rc->qlen=qlen;
	}
}

u32 rcget(rangeencoder* rc)
{
	// If data is ready to be output, returns an integer in the interval [0,256).
	// Otherwise, returns -1.
	assert((rc->flags&RC_ENCODE)!=0);
	u32 qlen=rc->qlen;
	if (qlen<10 && ((rc->flags&RC_FINISHED)==0 || qlen==0))
	{
		return -1;
	}
	// Go back from the end of the queue and shift bits into ret. If we use all bits at
	// a position, advance the position.
	u32 orig=rc->qpos+1;
	u32 qpos=orig-qlen;
	u32* qcount=rc->qcount;
	u32 ret=0;
	for (u32 i=0;i<8;i++)
	{
		ret+=ret+(qpos&1);
		qpos+=--qcount[qpos&RC_MASK]==0;
	}
	rc->qlen=orig-qpos;
	return ret;
}

//---------------------------------------------------------------------------------
// Decoding
//---------------------------------------------------------------------------------

u32 rcdecode(rangeencoder* rc,u32 intden)
{
	// Given an interval denominator, find a value in [0,intden) that will fall in to
	// some interval. Returns -1 if more data is needed.
	assert((rc->flags&RC_DECODE)!=0);
	assert((u64)intden<=rc->half+1);
	u32 qpos=rc->qpos;
	u32 qlen=(rc->qlen-qpos)&RC_MASK;
	u32* qcount=rc->qcount;
	if (qlen<rc->bits)
	{
		// If the input has not signaled it is finished, request more bits.
		if ((rc->flags&RC_FINISHED)==0)
		{
			return -1;
		}
		// If we are reading from a finished stream, pad the entire queue with 1's.
		qlen=rc->qlen;
		do
		{
			qcount[qlen]=1;
			qlen=(qlen+1)&RC_MASK;
		}
		while (qlen!=qpos);
		rc->qlen=(qpos-1)&RC_MASK;
	}
	// Shift the range.
	u64 half=rc->half;
	u64 low=rc->low;
	u64 range=rc->range;
	while (range<=half)
	{
		low+=low+qcount[qpos];
		qpos=(qpos+1)&RC_MASK;
		range+=range;
	}
	rc->qpos=qpos;
	rc->low=low;
	rc->range=range;
	// Scale low to yield our desired code value.
	return (low*intden+intden-1)/range;
}

void rcscale(rangeencoder* rc,u32 intlow,u32 inthigh,u32 intden)
{
	// Given an interval, scale the range to fit in the interval.
	assert((rc->flags&RC_DECODE)!=0);
	assert(intlow<inthigh && inthigh<=intden && (u64)intden<=rc->half+1);
	u64 range=rc->range;
	u64 off=(range*intlow)/intden;
	assert(rc->low>=off);
	rc->low-=off;
	rc->range=(range*inthigh)/intden-off;
}

void rcadd(rangeencoder* rc,u8 in)
{
	// Add an input byte to the decoding queue.
	assert((rc->flags&(RC_DECODE|RC_FINISHED))==RC_DECODE);
	assert(((rc->qlen-rc->qpos)&RC_MASK)<=RC_MASK-8);
	u32 pos=rc->qlen;
	u32* qcount=rc->qcount;
	for (u32 i=7;i<8;i--)
	{
		qcount[pos]=(in>>i)&1;
		pos=(pos+1)&RC_MASK;
	}
	rc->qlen=pos;
}
