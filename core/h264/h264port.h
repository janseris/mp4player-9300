/*
 * MP4 Player 9300: platform glue for the h264bsd decoder.
 * On the phone (GCC 2.9, __EPOC32__) there is no C library: malloc/free go to h264port.cpp
 * (User::Alloc on the current thread heap; the player switches to its decoder heap around calls),
 * memcpy/memset come from EUSER. On a PC the C library is used.
 */
#ifndef H264PORT_H
#define H264PORT_H

#ifdef __EPOC32__

typedef unsigned int size_t;
typedef unsigned long uintptr_t;
#ifdef __cplusplus
extern "C" {
#endif
void* h264port_malloc(unsigned int aSize);
void  h264port_free(void* aPtr);
void* memcpy(void* aDst, const void* aSrc, unsigned int aLen);
void* memset(void* aDst, int aVal, unsigned int aLen);
#ifdef __cplusplus
}
#endif
#define malloc(n) h264port_malloc(n)
#define free(p) h264port_free(p)

#else

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#endif

#endif
