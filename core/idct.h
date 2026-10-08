#ifndef IDCT_H
#define IDCT_H
#include "mpcommon.h"
/* block: 64 coefficients in raster order (int). On return the block is all zero again. */
void idct_put(u8* dest, int stride, int* block);
void idct_add(u8* dest, int stride, int* block);
/* rows not set in rowmask (bit r = row r) must be zero */
void idct_add_rows(u8* dest, int stride, int* block, int rowmask);
#endif
