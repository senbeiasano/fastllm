/*
range.h - v1.01

Copyright 2020 Alec Dee - MIT license - SPDX: MIT
deegen1.github.io - akdee144@gmail.com
*/

#if defined(RC_INCLUDE_H)==0
#define RC_INCLUDE_H

typedef unsigned      char  u8;
typedef unsigned     short u16;
typedef unsigned       int u32;
typedef unsigned long long u64;

#define RC_ENCODE     1
#define RC_DECODE     2
#define RC_FINISHED   4
#define RC_MASK     127

typedef struct rangeencoder rangeencoder;

struct rangeencoder
{
	u32 flags;
	// Range state.
	u32 bits;
	u64 norm;
	u64 half;
	u64 low;
	u64 range;
	// Bit queue for data we're ready to input or output.
	u32 qcount[RC_MASK+1];
	u32 qlen;
	u32 qpos;
};

void range_decode(u8* buffer, int insize, void* out, int outsize, int l2_num, u32* prob);

//---------------------------------------------------------------------------------
// Management
//---------------------------------------------------------------------------------

rangeencoder* rccreate(u32 mode);
void rcfree(rangeencoder* rc);
void rcinit(rangeencoder* rc,u32 mode);

//---------------------------------------------------------------------------------
// Encoding
//---------------------------------------------------------------------------------

void rcencode(rangeencoder* rc,u32 intlow,u32 inthigh,u32 intden);
void rcfinish(rangeencoder* rc);
u32  rcget(rangeencoder* rc);

//---------------------------------------------------------------------------------
// Decoding
//---------------------------------------------------------------------------------

u32  rcdecode(rangeencoder* rc,u32 intden);
void rcscale(rangeencoder* rc,u32 intlow,u32 inthigh,u32 intden);
void rcadd(rangeencoder* rc,u8 in);

#endif
