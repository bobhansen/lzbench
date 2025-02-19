/*
   LZ5 - Fast LZ compression algorithm
   Copyright (C) 2011-2015, Yann Collet.

   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - LZ5 source repository : https://github.com/inikep/lz5
   - LZ5 public forum : https://groups.google.com/forum/#!forum/lz5c
*/


/**************************************
*  Tuning parameters
**************************************/
/*
 * HEAPMODE :
 * Select how default compression functions will allocate memory for their hash table,
 * in memory stack (0:default, fastest), or in memory heap (1:requires malloc()).
 */
#define HEAPMODE 0

/*
 * ACCELERATION_DEFAULT :
 * Select "acceleration" for LZ5_compress_fast() when parameter value <= 0
 */
#define ACCELERATION_DEFAULT 1


/**************************************
*  CPU Feature Detection
**************************************/
/* LZ5_FORCE_MEMORY_ACCESS
 * By default, access to unaligned memory is controlled by `memcpy()`, which is safe and portable.
 * Unfortunately, on some target/compiler combinations, the generated assembly is sub-optimal.
 * The below switch allow to select different access method for improved performance.
 * Method 0 (default) : use `memcpy()`. Safe and portable.
 * Method 1 : `__packed` statement. It depends on compiler extension (ie, not portable).
 *            This method is safe if your compiler supports it, and *generally* as fast or faster than `memcpy`.
 * Method 2 : direct access. This method is portable but violate C standard.
 *            It can generate buggy code on targets which generate assembly depending on alignment.
 *            But in some circumstances, it's the only known way to get the most performance (ie GCC + ARMv6)
 * See http://fastcompression.blogspot.fr/2015/08/accessing-unaligned-memory.html for details.
 * Prefer these methods in priority order (0 > 1 > 2)
 */
#ifndef LZ5_FORCE_MEMORY_ACCESS   /* can be defined externally, on command line for example */
#  if defined(__GNUC__) && ( defined(__ARM_ARCH_6__) || defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) || defined(__ARM_ARCH_6T2__) )
#    define LZ5_FORCE_MEMORY_ACCESS 2
#  elif defined(__INTEL_COMPILER) || \
  (defined(__GNUC__) && ( defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7S__) ))
#    define LZ5_FORCE_MEMORY_ACCESS 1
#  endif
#endif

/*
 * LZ5_FORCE_SW_BITCOUNT
 * Define this parameter if your target system or compiler does not support hardware bit count
 */
#if defined(_MSC_VER) && defined(_WIN32_WCE)   /* Visual Studio for Windows CE does not support Hardware bit count */
#  define LZ5_FORCE_SW_BITCOUNT
#endif


/**************************************
*  Includes
**************************************/
#include "lz5.h"


/**************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define FORCE_INLINE static __forceinline
#  include <intrin.h>
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4293)        /* disable: C4293: too large shift (32-bits) */
#else
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
#    if defined(__GNUC__) || defined(__clang__)
#      define FORCE_INLINE static inline __attribute__((always_inline))
#    else
#      define FORCE_INLINE static inline
#    endif
#  else
#    define FORCE_INLINE static
#  endif   /* __STDC_VERSION__ */
#endif  /* _MSC_VER */

/* LZ5_GCC_VERSION is defined into lz5.h */
#if (LZ5_GCC_VERSION >= 302) || (__INTEL_COMPILER >= 800) || defined(__clang__)
#  define expect(expr,value)    (__builtin_expect ((expr),(value)) )
#else
#  define expect(expr,value)    (expr)
#endif

#define likely(expr)     expect((expr) != 0, 1)
#define unlikely(expr)   expect((expr) != 0, 0)


/**************************************
*  Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(n,s) calloc(n,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy */
#define MEM_INIT       memset


/**************************************
*  Basic Types
**************************************/
#if defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
# include <stdint.h>
  typedef  uint8_t BYTE;
  typedef uint16_t U16;
  typedef uint32_t U32;
  typedef  int32_t S32;
  typedef uint64_t U64;
#else
  typedef unsigned char       BYTE;
  typedef unsigned short      U16;
  typedef unsigned int        U32;
  typedef   signed int        S32;
  typedef unsigned long long  U64;
#endif


/**************************************
*  Reading and writing into memory
**************************************/
#define STEPSIZE sizeof(size_t)

static unsigned LZ5_64bits(void) { return sizeof(void*)==8; }

static unsigned LZ5_isLittleEndian(void)
{
    const union { U32 i; BYTE c[4]; } one = { 1 };   // don't use static : performance detrimental
    return one.c[0];
}


#if defined(LZ5_FORCE_MEMORY_ACCESS) && (LZ5_FORCE_MEMORY_ACCESS==2)

static U16 LZ5_read16(const void* memPtr) { return *(const U16*) memPtr; }
static U32 LZ5_read32(const void* memPtr) { return *(const U32*) memPtr; }
static size_t LZ5_read_ARCH(const void* memPtr) { return *(const size_t*) memPtr; }

static void LZ5_write16(void* memPtr, U16 value) { *(U16*)memPtr = value; }

#elif defined(LZ5_FORCE_MEMORY_ACCESS) && (LZ5_FORCE_MEMORY_ACCESS==1)

/* __pack instructions are safer, but compiler specific, hence potentially problematic for some compilers */
/* currently only defined for gcc and icc */
typedef union { U16 u16; U32 u32; size_t uArch; } __attribute__((packed)) unalign;

static U16 LZ5_read16(const void* ptr) { return ((const unalign*)ptr)->u16; }
static U32 LZ5_read32(const void* ptr) { return ((const unalign*)ptr)->u32; }
static size_t LZ5_read_ARCH(const void* ptr) { return ((const unalign*)ptr)->uArch; }

static void LZ5_write16(void* memPtr, U16 value) { ((unalign*)memPtr)->u16 = value; }

#else

static U16 LZ5_read16(const void* memPtr)
{
    U16 val; memcpy(&val, memPtr, sizeof(val)); return val;
}

static U32 LZ5_read32(const void* memPtr)
{
    U32 val; memcpy(&val, memPtr, sizeof(val)); return val;
}

static size_t LZ5_read_ARCH(const void* memPtr)
{
    size_t val; memcpy(&val, memPtr, sizeof(val)); return val;
}

static void LZ5_write16(void* memPtr, U16 value)
{
    memcpy(memPtr, &value, sizeof(value));
}

#endif // LZ5_FORCE_MEMORY_ACCESS


static U16 LZ5_readLE16(const void* memPtr)
{
    if (LZ5_isLittleEndian())
    {
        return LZ5_read16(memPtr);
    }
    else
    {
        const BYTE* p = (const BYTE*)memPtr;
        return (U16)((U16)p[0] + (p[1]<<8));
    }
}

static U32 LZ5_readLE24(const void* memPtr)
{
    if (LZ5_isLittleEndian())
    {
        U32 val32 = 0;
        memcpy(&val32, memPtr, 3);
        return val32;
    }
    else
    {
        const BYTE* p = (const BYTE*)memPtr;
        return (U32)(p[0] + (p[1]<<8) + (p[2]<<16));
    }
}

static void LZ5_writeLE16(void* memPtr, U16 value)
{
    if (LZ5_isLittleEndian())
    {
        LZ5_write16(memPtr, value);
    }
    else
    {
        BYTE* p = (BYTE*)memPtr;
        p[0] = (BYTE) value;
        p[1] = (BYTE)(value>>8);
    }
}

static void LZ5_writeLE24(void* memPtr, U32 value)
{
    if (LZ5_isLittleEndian())
    {
        memcpy(memPtr, &value, 3);
    }
    else
    {
        BYTE* p = (BYTE*)memPtr;
        p[0] = (BYTE) value;
        p[1] = (BYTE)(value>>8);
        p[2] = (BYTE)(value>>16);
    }
}


static void LZ5_copy8(void* dst, const void* src)
{
    memcpy(dst,src,8);
}

/* customized variant of memcpy, which can overwrite up to 7 bytes beyond dstEnd */
static void LZ5_wildCopy(void* dstPtr, const void* srcPtr, void* dstEnd)
{
    BYTE* d = (BYTE*)dstPtr;
    const BYTE* s = (const BYTE*)srcPtr;
    BYTE* const e = (BYTE*)dstEnd;

#if 0
    const size_t l2 = 8 - (((size_t)d) & (sizeof(void*)-1));
    LZ5_copy8(d,s); if (d>e-9) return;
    d+=l2; s+=l2;
#endif /* join to align */

    do { LZ5_copy8(d,s); d+=8; s+=8; } while (d<e);
}


/**************************************
*  Common Constants
**************************************/
#define MINMATCH 4

#define WILDCOPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT (WILDCOPYLENGTH+MINMATCH)
static const int LZ5_minLength = (MFLIMIT+1);

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define MAXD_LOG 22
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)
#define LZ5_DICT_SIZE (1 << MAXD_LOG)

#define ML_BITS  3
#define ML_MASK  ((1U<<ML_BITS)-1)
#define RUN_BITS 3
#define RUN_MASK ((1U<<RUN_BITS)-1)
#define RUN_BITS2 2
#define RUN_MASK2 ((1U<<RUN_BITS2)-1)
#define ML_RUN_BITS (ML_BITS + RUN_BITS)
#define ML_RUN_BITS2 (ML_BITS + RUN_BITS2)



/**************************************
*  Common Utils
**************************************/
#define LZ5_STATIC_ASSERT(c)    { enum { LZ5_static_assert = 1/(int)(!!(c)) }; }   /* use only *after* variable declarations */


/**************************************
*  Common functions
**************************************/
static unsigned LZ5_NbCommonBytes (register size_t val)
{
    if (LZ5_isLittleEndian())
    {
        if (LZ5_64bits())
        {
#       if defined(_MSC_VER) && defined(_WIN64) && !defined(LZ5_FORCE_SW_BITCOUNT)
            unsigned long r = 0;
            _BitScanForward64( &r, (U64)val );
            return (int)(r>>3);
#       elif (defined(__clang__) || (LZ5_GCC_VERSION >= 304)) && !defined(LZ5_FORCE_SW_BITCOUNT)
            return (__builtin_ctzll((U64)val) >> 3);
#       else
            static const int DeBruijnBytePos[64] = { 0, 0, 0, 0, 0, 1, 1, 2, 0, 3, 1, 3, 1, 4, 2, 7, 0, 2, 3, 6, 1, 5, 3, 5, 1, 3, 4, 4, 2, 5, 6, 7, 7, 0, 1, 2, 3, 3, 4, 6, 2, 6, 5, 5, 3, 4, 5, 6, 7, 1, 2, 4, 6, 4, 4, 5, 7, 2, 6, 5, 7, 6, 7, 7 };
            return DeBruijnBytePos[((U64)((val & -(long long)val) * 0x0218A392CDABBD3FULL)) >> 58];
#       endif
        }
        else /* 32 bits */
        {
#       if defined(_MSC_VER) && !defined(LZ5_FORCE_SW_BITCOUNT)
            unsigned long r;
            _BitScanForward( &r, (U32)val );
            return (int)(r>>3);
#       elif (defined(__clang__) || (LZ5_GCC_VERSION >= 304)) && !defined(LZ5_FORCE_SW_BITCOUNT)
            return (__builtin_ctz((U32)val) >> 3);
#       else
            static const int DeBruijnBytePos[32] = { 0, 0, 3, 0, 3, 1, 3, 0, 3, 2, 2, 1, 3, 2, 0, 1, 3, 3, 1, 2, 2, 2, 2, 0, 3, 1, 2, 0, 1, 0, 1, 1 };
            return DeBruijnBytePos[((U32)((val & -(S32)val) * 0x077CB531U)) >> 27];
#       endif
        }
    }
    else   /* Big Endian CPU */
    {
        if (LZ5_64bits())
        {
#       if defined(_MSC_VER) && defined(_WIN64) && !defined(LZ5_FORCE_SW_BITCOUNT)
            unsigned long r = 0;
            _BitScanReverse64( &r, val );
            return (unsigned)(r>>3);
#       elif (defined(__clang__) || (LZ5_GCC_VERSION >= 304)) && !defined(LZ5_FORCE_SW_BITCOUNT)
            return (__builtin_clzll((U64)val) >> 3);
#       else
            unsigned r;
            if (!(val>>32)) { r=4; } else { r=0; val>>=32; }
            if (!(val>>16)) { r+=2; val>>=8; } else { val>>=24; }
            r += (!val);
            return r;
#       endif
        }
        else /* 32 bits */
        {
#       if defined(_MSC_VER) && !defined(LZ5_FORCE_SW_BITCOUNT)
            unsigned long r = 0;
            _BitScanReverse( &r, (unsigned long)val );
            return (unsigned)(r>>3);
#       elif (defined(__clang__) || (LZ5_GCC_VERSION >= 304)) && !defined(LZ5_FORCE_SW_BITCOUNT)
            return (__builtin_clz((U32)val) >> 3);
#       else
            unsigned r;
            if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
            r += (!val);
            return r;
#       endif
        }
    }
}

static unsigned LZ5_count(const BYTE* pIn, const BYTE* pMatch, const BYTE* pInLimit)
{
    const BYTE* const pStart = pIn;

    while (likely(pIn<pInLimit-(STEPSIZE-1)))
    {
        size_t diff = LZ5_read_ARCH(pMatch) ^ LZ5_read_ARCH(pIn);
        if (!diff) { pIn+=STEPSIZE; pMatch+=STEPSIZE; continue; }
        pIn += LZ5_NbCommonBytes(diff);
        return (unsigned)(pIn - pStart);
    }

    if (LZ5_64bits()) if ((pIn<(pInLimit-3)) && (LZ5_read32(pMatch) == LZ5_read32(pIn))) { pIn+=4; pMatch+=4; }
    if ((pIn<(pInLimit-1)) && (LZ5_read16(pMatch) == LZ5_read16(pIn))) { pIn+=2; pMatch+=2; }
    if ((pIn<pInLimit) && (*pMatch == *pIn)) pIn++;
    return (unsigned)(pIn - pStart);
}


#ifndef LZ5_COMMONDEFS_ONLY
/**************************************
*  Local Constants
**************************************/
#define LZ5_HASHLOG   (LZ5_MEMORY_USAGE-2)
#define HASHTABLESIZE (1 << LZ5_MEMORY_USAGE)
#define HASH_SIZE_U32 (1 << LZ5_HASHLOG)       /* required as macro for static allocation */

static const int LZ5_64Klimit = ((64 KB) + (MFLIMIT-1));
static const U32 LZ5_skipTrigger = 6;  /* Increase this value ==> compression run slower on incompressible data */


/**************************************
*  Local Structures and types
**************************************/
typedef struct {
    U32 hashTable[HASH_SIZE_U32];
    U32 currentOffset;
    U32 initCheck;
    const BYTE* dictionary;
    BYTE* bufferStart;   /* obsolete, used for slideInputBuffer */
    U32 dictSize;
} LZ5_stream_t_internal;

typedef enum { notLimited = 0, limitedOutput = 1 } limitedOutput_directive;
typedef enum { byPtr, byU32, byU16 } tableType_t;

typedef enum { noDict = 0, withPrefix64k, usingExtDict } dict_directive;
typedef enum { noDictIssue = 0, dictSmall } dictIssue_directive;

typedef enum { endOnOutputSize = 0, endOnInputSize = 1 } endCondition_directive;
typedef enum { full = 0, partial = 1 } earlyEnd_directive;


/**************************************
*  Local Utils
**************************************/
int LZ5_versionNumber (void) { return LZ5_VERSION_NUMBER; }
int LZ5_compressBound(int isize)  { return LZ5_COMPRESSBOUND(isize); }
int LZ5_sizeofState() { return LZ5_STREAMSIZE; }



/********************************
*  Compression functions
********************************/

static U32 LZ5_hashSequence(U32 sequence, tableType_t const tableType)
{
    if (tableType == byU16)
        return (((sequence) * 2654435761U) >> ((MINMATCH*8)-(LZ5_HASHLOG+1)));
    else
        return (((sequence) * 2654435761U) >> ((MINMATCH*8)-LZ5_HASHLOG));
}

static const U64 prime5bytes = 889523592379ULL;
static U32 LZ5_hashSequence64(size_t sequence, tableType_t const tableType)
{
    const U32 hashLog = (tableType == byU16) ? LZ5_HASHLOG+1 : LZ5_HASHLOG;
    const U32 hashMask = (1<<hashLog) - 1;
    return ((sequence * prime5bytes) >> (40 - hashLog)) & hashMask;
}

static U32 LZ5_hashSequenceT(size_t sequence, tableType_t const tableType)
{
    if (LZ5_64bits())
        return LZ5_hashSequence64(sequence, tableType);
    return LZ5_hashSequence((U32)sequence, tableType);
}

static U32 LZ5_hashPosition(const void* p, tableType_t tableType) { return LZ5_hashSequenceT(LZ5_read_ARCH(p), tableType); }

static void LZ5_putPositionOnHash(const BYTE* p, U32 h, void* tableBase, tableType_t const tableType, const BYTE* srcBase)
{
    switch (tableType)
    {
    case byPtr: { const BYTE** hashTable = (const BYTE**)tableBase; hashTable[h] = p; return; }
    case byU32: { U32* hashTable = (U32*) tableBase; hashTable[h] = (U32)(p-srcBase); return; }
    case byU16: { U16* hashTable = (U16*) tableBase; hashTable[h] = (U16)(p-srcBase); return; }
    }
}

static void LZ5_putPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ5_hashPosition(p, tableType);
    LZ5_putPositionOnHash(p, h, tableBase, tableType, srcBase);
}

static const BYTE* LZ5_getPositionOnHash(U32 h, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    if (tableType == byPtr) { const BYTE** hashTable = (const BYTE**) tableBase; return hashTable[h]; }
    if (tableType == byU32) { U32* hashTable = (U32*) tableBase; return hashTable[h] + srcBase; }
    { U16* hashTable = (U16*) tableBase; return hashTable[h] + srcBase; }   /* default, to ensure a return */
}

static const BYTE* LZ5_getPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ5_hashPosition(p, tableType);
    return LZ5_getPositionOnHash(h, tableBase, tableType, srcBase);
}

FORCE_INLINE int LZ5_compress_generic(
                 void* const ctx,
                 const char* const source,
                 char* const dest,
                 const int inputSize,
                 const int maxOutputSize,
                 const limitedOutput_directive outputLimited,
                 const tableType_t tableType,
                 const dict_directive dict,
                 const dictIssue_directive dictIssue,
                 const U32 acceleration)
{
    LZ5_stream_t_internal* const dictPtr = (LZ5_stream_t_internal*)ctx;

    const BYTE* ip = (const BYTE*) source;
    const BYTE* base;
    const BYTE* lowLimit;
    const BYTE* const lowRefLimit = ip - dictPtr->dictSize;
    const BYTE* const dictionary = dictPtr->dictionary;
    const BYTE* const dictEnd = dictionary + dictPtr->dictSize;
    const size_t dictDelta = dictEnd - (const BYTE*)source;
    const BYTE* anchor = (const BYTE*) source;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;

    BYTE* op = (BYTE*) dest;
    BYTE* const olimit = op + maxOutputSize;

    U32 forwardH;
    size_t refDelta=0;

    /* Init conditions */
    if ((U32)inputSize > (U32)LZ5_MAX_INPUT_SIZE) return 0;   /* Unsupported input size, too large (or negative) */
    switch(dict)
    {
    case noDict:
    default:
        base = (const BYTE*)source;
        lowLimit = (const BYTE*)source;
        break;
    case withPrefix64k:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source - dictPtr->dictSize;
        break;
    case usingExtDict:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source;
        break;
    }
    if ((tableType == byU16) && (inputSize>=LZ5_64Klimit)) return 0;   /* Size too large (not within 64K limit) */
    if (inputSize<LZ5_minLength) goto _last_literals;                  /* Input too small, no compression (all literals) */

    /* First Byte */
    LZ5_putPosition(ip, ctx, tableType, base);
    ip++; forwardH = LZ5_hashPosition(ip, tableType);

    /* Main Loop */
    for ( ; ; )
    {
        const BYTE* match;
        BYTE* token;
        {
            const BYTE* forwardIp = ip;
            unsigned step = 1;
            unsigned searchMatchNb = acceleration << LZ5_skipTrigger;

            /* Find a match */
            do {
                U32 h = forwardH;
                ip = forwardIp;
                forwardIp += step;
                step = (searchMatchNb++ >> LZ5_skipTrigger);

                if (unlikely(forwardIp > mflimit)) goto _last_literals;

                match = LZ5_getPositionOnHash(h, ctx, tableType, base);
                if (dict==usingExtDict)
                {
                    if (match<(const BYTE*)source)
                    {
                        refDelta = dictDelta;
                        lowLimit = dictionary;
                    }
                    else
                    {
                        refDelta = 0;
                        lowLimit = (const BYTE*)source;
                    }
                }
                forwardH = LZ5_hashPosition(forwardIp, tableType);
                LZ5_putPositionOnHash(ip, h, ctx, tableType, base);

            } while ( ((dictIssue==dictSmall) ? (match < lowRefLimit) : 0)
                || ((tableType==byU16) ? 0 : (match + MAX_DISTANCE < ip))
                || (LZ5_read32(match+refDelta) != LZ5_read32(ip)) );
        }

        /* Catch up */
        while ((ip>anchor) && (match+refDelta > lowLimit) && (unlikely(ip[-1]==match[refDelta-1]))) { ip--; match--; }

        {
            /* Encode Literal length */
            unsigned litLength = (unsigned)(ip - anchor);
            token = op++;
            if ((outputLimited) && (unlikely(op + litLength + (2 + 1 + LASTLITERALS) + (litLength/255) > olimit)))
                return 0;   /* Check output limit */

            if (ip-match < (1<<10))
            {
                if (litLength>=RUN_MASK2)
                {
                    int len = (int)litLength-RUN_MASK2;
                    *token=(RUN_MASK2<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }
            else
            {
                if (litLength>=RUN_MASK)
                {
                    int len = (int)litLength-RUN_MASK;
                    *token=(RUN_MASK<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }

            /* Copy Literals */
            LZ5_wildCopy(op, anchor, op+litLength);
            op+=litLength;
        }

_next_match:
        /* Encode Offset */
        if (ip-match < (1<<10))
        {
            *token+=((4+((ip-match)>>8))<<ML_RUN_BITS2);
            *op++=(ip-match);
        }
        else
        if (ip-match < (1<<16))
        {
            LZ5_writeLE16(op, (U16)(ip-match)); op+=2;
        }
        else
        {
            *token+=(1<<ML_RUN_BITS);
            LZ5_writeLE24(op, (U32)(ip-match)); op+=3;
        }

        /* Encode MatchLength */
        {
            unsigned matchLength;

            if ((dict==usingExtDict) && (lowLimit==dictionary))
            {
                const BYTE* limit;
                match += refDelta;
                limit = ip + (dictEnd-match);
                if (limit > matchlimit) limit = matchlimit;
                matchLength = LZ5_count(ip+MINMATCH, match+MINMATCH, limit);
                ip += MINMATCH + matchLength;
                if (ip==limit)
                {
                    unsigned more = LZ5_count(ip, (const BYTE*)source, matchlimit);
                    matchLength += more;
                    ip += more;
                }
            }
            else
            {
                matchLength = LZ5_count(ip+MINMATCH, match+MINMATCH, matchlimit);
                ip += MINMATCH + matchLength;
            }

            if ((outputLimited) && (unlikely(op + (1 + LASTLITERALS) + (matchLength>>8) > olimit)))
                return 0;    /* Check output limit */
            if (matchLength>=ML_MASK)
            {
                *token += ML_MASK;
                matchLength -= ML_MASK;
                for (; matchLength >= 510 ; matchLength-=510) { *op++ = 255; *op++ = 255; }
                if (matchLength >= 255) { matchLength-=255; *op++ = 255; }
                *op++ = (BYTE)matchLength;
            }
            else *token += (BYTE)(matchLength);
        }

        anchor = ip;

        /* Test end of chunk */
        if (ip > mflimit) break;

        /* Fill table */
        LZ5_putPosition(ip-2, ctx, tableType, base);

        /* Test next position */
        match = LZ5_getPosition(ip, ctx, tableType, base);
        if (dict==usingExtDict)
        {
            if (match<(const BYTE*)source)
            {
                refDelta = dictDelta;
                lowLimit = dictionary;
            }
            else
            {
                refDelta = 0;
                lowLimit = (const BYTE*)source;
            }
        }
        LZ5_putPosition(ip, ctx, tableType, base);
        if ( ((dictIssue==dictSmall) ? (match>=lowRefLimit) : 1)
            && (match+MAX_DISTANCE>=ip)
            && (LZ5_read32(match+refDelta)==LZ5_read32(ip)) )
        { token=op++; *token=0; goto _next_match; }

        /* Prepare next loop */
        forwardH = LZ5_hashPosition(++ip, tableType);
    }

_last_literals:
    /* Encode Last Literals */
    {
        const size_t lastRun = (size_t)(iend - anchor);
        if ((outputLimited) && ((op - (BYTE*)dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize))
            return 0;   /* Check output limit */
        if (lastRun >= RUN_MASK)
        {
            size_t accumulator = lastRun - RUN_MASK;
            *op++ = RUN_MASK << ML_BITS;
            for(; accumulator >= 255 ; accumulator-=255) *op++ = 255;
            *op++ = (BYTE) accumulator;
        }
        else
        {
            *op++ = (BYTE)(lastRun<<ML_BITS);
        }
        memcpy(op, anchor, lastRun);
        op += lastRun;
    }

    /* End */
    return (int) (((char*)op)-dest);
}


int LZ5_compress_fast_extState(void* state, const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ5_resetStream((LZ5_stream_t*)state);
    if (acceleration < 1) acceleration = ACCELERATION_DEFAULT;

    if (maxOutputSize >= LZ5_compressBound(inputSize))
    {
        if (inputSize < LZ5_64Klimit)
            return LZ5_compress_generic(state, source, dest, inputSize, 0, notLimited, byU16,                        noDict, noDictIssue, acceleration);
        else
            return LZ5_compress_generic(state, source, dest, inputSize, 0, notLimited, LZ5_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
    }
    else
    {
        if (inputSize < LZ5_64Klimit)
            return LZ5_compress_generic(state, source, dest, inputSize, maxOutputSize, limitedOutput, byU16,                        noDict, noDictIssue, acceleration);
        else
            return LZ5_compress_generic(state, source, dest, inputSize, maxOutputSize, limitedOutput, LZ5_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
    }
}


int LZ5_compress_fast(const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
#if (HEAPMODE)
    void* ctxPtr = ALLOCATOR(1, sizeof(LZ5_stream_t));   /* malloc-calloc always properly aligned */
#else
    LZ5_stream_t ctx;
    void* ctxPtr = &ctx;
#endif

    int result = LZ5_compress_fast_extState(ctxPtr, source, dest, inputSize, maxOutputSize, acceleration);

#if (HEAPMODE)
    FREEMEM(ctxPtr);
#endif
    return result;
}


int LZ5_compress_default(const char* source, char* dest, int inputSize, int maxOutputSize)
{
    return LZ5_compress_fast(source, dest, inputSize, maxOutputSize, 1);
}


/* hidden debug function */
/* strangely enough, gcc generates faster code when this function is uncommented, even if unused */
int LZ5_compress_fast_force(const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ5_stream_t ctx;

    LZ5_resetStream(&ctx);

    if (inputSize < LZ5_64Klimit)
        return LZ5_compress_generic(&ctx, source, dest, inputSize, maxOutputSize, limitedOutput, byU16,                        noDict, noDictIssue, acceleration);
    else
        return LZ5_compress_generic(&ctx, source, dest, inputSize, maxOutputSize, limitedOutput, LZ5_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
}


/********************************
*  destSize variant
********************************/

static int LZ5_compress_destSize_generic(
                       void* const ctx,
                 const char* const src,
                       char* const dst,
                       int*  const srcSizePtr,
                 const int targetDstSize,
                 const tableType_t tableType)
{
    const BYTE* ip = (const BYTE*) src;
    const BYTE* base = (const BYTE*) src;
    const BYTE* lowLimit = (const BYTE*) src;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + *srcSizePtr;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;

    BYTE* op = (BYTE*) dst;
    BYTE* const oend = op + targetDstSize;
    BYTE* const oMaxLit = op + targetDstSize - 2 /* offset */ - 8 /* because 8+MINMATCH==MFLIMIT */ - 1 /* token */;
    BYTE* const oMaxMatch = op + targetDstSize - (LASTLITERALS + 1 /* token */);
    BYTE* const oMaxSeq = oMaxLit - 1 /* token */;

    U32 forwardH;


    /* Init conditions */
    if (targetDstSize < 1) return 0;                                     /* Impossible to store anything */
    if ((U32)*srcSizePtr > (U32)LZ5_MAX_INPUT_SIZE) return 0;            /* Unsupported input size, too large (or negative) */
    if ((tableType == byU16) && (*srcSizePtr>=LZ5_64Klimit)) return 0;   /* Size too large (not within 64K limit) */
    if (*srcSizePtr<LZ5_minLength) goto _last_literals;                  /* Input too small, no compression (all literals) */

    /* First Byte */
    *srcSizePtr = 0;
    LZ5_putPosition(ip, ctx, tableType, base);
    ip++; forwardH = LZ5_hashPosition(ip, tableType);

    /* Main Loop */
    for ( ; ; )
    {
        const BYTE* match;
        BYTE* token;
        {
            const BYTE* forwardIp = ip;
            unsigned step = 1;
            unsigned searchMatchNb = 1 << LZ5_skipTrigger;

            /* Find a match */
            do {
                U32 h = forwardH;
                ip = forwardIp;
                forwardIp += step;
                step = (searchMatchNb++ >> LZ5_skipTrigger);

                if (unlikely(forwardIp > mflimit))
                    goto _last_literals;

                match = LZ5_getPositionOnHash(h, ctx, tableType, base);
                forwardH = LZ5_hashPosition(forwardIp, tableType);
                LZ5_putPositionOnHash(ip, h, ctx, tableType, base);

            } while ( ((tableType==byU16) ? 0 : (match + MAX_DISTANCE < ip))
                || (LZ5_read32(match) != LZ5_read32(ip)) );
        }

        /* Catch up */
        while ((ip>anchor) && (match > lowLimit) && (unlikely(ip[-1]==match[-1]))) { ip--; match--; }

        {
            /* Encode Literal length */
            unsigned litLength = (unsigned)(ip - anchor);
            token = op++;
            if (op + ((litLength+240)/255) + litLength > oMaxLit)
            {
                /* Not enough space for a last match */
                op--;
                goto _last_literals;
            }
            
            if (ip-match < (1<<10))
            {
                if (litLength>=RUN_MASK2)
                {
                    int len = (int)litLength-RUN_MASK2;
                    *token=(RUN_MASK2<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }
            else
            {
                if (litLength>=RUN_MASK)
                {
                    int len = (int)litLength-RUN_MASK;
                    *token=(RUN_MASK<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }

            /* Copy Literals */
            LZ5_wildCopy(op, anchor, op+litLength);
            op += litLength;
        }

_next_match:
        /* Encode Offset */
        if (ip-match < (1<<10))
        {
            *token+=((4+((ip-match)>>8))<<ML_RUN_BITS2);
            *op++=(ip-match);
        }
        else
        if (ip-match < (1<<16))
        {
            LZ5_writeLE16(op, (U16)(ip-match)); op+=2;
        }
        else
        {
            *token+=(1<<ML_RUN_BITS);
            LZ5_writeLE24(op, (U32)(ip-match)); op+=3;
        }

        /* Encode MatchLength */
        {
            size_t matchLength;

            matchLength = LZ5_count(ip+MINMATCH, match+MINMATCH, matchlimit);

            if (op + ((matchLength+240)/255) > oMaxMatch)
            {
                /* Match description too long : reduce it */
                matchLength = (15-1) + (oMaxMatch-op) * 255;
            }
            ip += MINMATCH + matchLength;

            if (matchLength>=ML_MASK)
            {
                *token += ML_MASK;
                matchLength -= ML_MASK;
                while (matchLength >= 255) { matchLength-=255; *op++ = 255; }
                *op++ = (BYTE)matchLength;
            }
            else *token += (BYTE)(matchLength);
        }

        anchor = ip;

        /* Test end of block */
        if (ip > mflimit) break;
        if (op > oMaxSeq) break;

        /* Fill table */
        LZ5_putPosition(ip-2, ctx, tableType, base);

        /* Test next position */
        match = LZ5_getPosition(ip, ctx, tableType, base);
        LZ5_putPosition(ip, ctx, tableType, base);
        if ( (match+MAX_DISTANCE>=ip)
            && (LZ5_read32(match)==LZ5_read32(ip)) )
        { token=op++; *token=0; goto _next_match; }

        /* Prepare next loop */
        forwardH = LZ5_hashPosition(++ip, tableType);
    }

_last_literals:
    /* Encode Last Literals */
    {
        size_t lastRunSize = (size_t)(iend - anchor);
        if (op + 1 /* token */ + ((lastRunSize+240)/255) /* litLength */ + lastRunSize /* literals */ > oend)
        {
            /* adapt lastRunSize to fill 'dst' */
            lastRunSize  = (oend-op) - 1;
            lastRunSize -= (lastRunSize+240)/255;
        }
        ip = anchor + lastRunSize;

        if (lastRunSize >= RUN_MASK)
        {
            size_t accumulator = lastRunSize - RUN_MASK;
            *op++ = RUN_MASK << ML_BITS;
            for(; accumulator >= 255 ; accumulator-=255) *op++ = 255;
            *op++ = (BYTE) accumulator;
        }
        else
        {
            *op++ = (BYTE)(lastRunSize<<ML_BITS);
        }
        memcpy(op, anchor, lastRunSize);
        op += lastRunSize;
    }

    /* End */
    *srcSizePtr = (int) (((const char*)ip)-src);
    return (int) (((char*)op)-dst);
}


static int LZ5_compress_destSize_extState (void* state, const char* src, char* dst, int* srcSizePtr, int targetDstSize)
{
    LZ5_resetStream((LZ5_stream_t*)state);

    if (targetDstSize >= LZ5_compressBound(*srcSizePtr))   /* compression success is guaranteed */
    {
        return LZ5_compress_fast_extState(state, src, dst, *srcSizePtr, targetDstSize, 1);
    }
    else
    {
        if (*srcSizePtr < LZ5_64Klimit)
            return LZ5_compress_destSize_generic(state, src, dst, srcSizePtr, targetDstSize, byU16);
        else
            return LZ5_compress_destSize_generic(state, src, dst, srcSizePtr, targetDstSize, LZ5_64bits() ? byU32 : byPtr);
    }
}


int LZ5_compress_destSize(const char* src, char* dst, int* srcSizePtr, int targetDstSize)
{
#if (HEAPMODE)
    void* ctx = ALLOCATOR(1, sizeof(LZ5_stream_t));   /* malloc-calloc always properly aligned */
#else
    LZ5_stream_t ctxBody;
    void* ctx = &ctxBody;
#endif

    int result = LZ5_compress_destSize_extState(ctx, src, dst, srcSizePtr, targetDstSize);

#if (HEAPMODE)
    FREEMEM(ctx);
#endif
    return result;
}



/********************************
*  Streaming functions
********************************/

LZ5_stream_t* LZ5_createStream(void)
{
    LZ5_stream_t* lz5s = (LZ5_stream_t*)ALLOCATOR(8, LZ5_STREAMSIZE_U64);
    LZ5_STATIC_ASSERT(LZ5_STREAMSIZE >= sizeof(LZ5_stream_t_internal));    /* A compilation error here means LZ5_STREAMSIZE is not large enough */
    LZ5_resetStream(lz5s);
    return lz5s;
}

void LZ5_resetStream (LZ5_stream_t* LZ5_stream)
{
    MEM_INIT(LZ5_stream, 0, sizeof(LZ5_stream_t));
}

int LZ5_freeStream (LZ5_stream_t* LZ5_stream)
{
    FREEMEM(LZ5_stream);
    return (0);
}


#define HASH_UNIT sizeof(size_t)
int LZ5_loadDict (LZ5_stream_t* LZ5_dict, const char* dictionary, int dictSize)
{
    LZ5_stream_t_internal* dict = (LZ5_stream_t_internal*) LZ5_dict;
    const BYTE* p = (const BYTE*)dictionary;
    const BYTE* const dictEnd = p + dictSize;
    const BYTE* base;

    if ((dict->initCheck) || (dict->currentOffset > 1 GB))  /* Uninitialized structure, or reuse overflow */
        LZ5_resetStream(LZ5_dict);

    if (dictSize < (int)HASH_UNIT)
    {
        dict->dictionary = NULL;
        dict->dictSize = 0;
        return 0;
    }

    if ((dictEnd - p) > LZ5_DICT_SIZE) p = dictEnd - LZ5_DICT_SIZE;
    dict->currentOffset += LZ5_DICT_SIZE;
    base = p - dict->currentOffset;
    dict->dictionary = p;
    dict->dictSize = (U32)(dictEnd - p);
    dict->currentOffset += dict->dictSize;

    while (p <= dictEnd-HASH_UNIT)
    {
        LZ5_putPosition(p, dict->hashTable, byU32, base);
        p+=3;
    }

    return dict->dictSize;
}


static void LZ5_renormDictT(LZ5_stream_t_internal* LZ5_dict, const BYTE* src)
{
    if ((LZ5_dict->currentOffset > 0x80000000) ||
        ((size_t)LZ5_dict->currentOffset > (size_t)src))   /* address space overflow */
    {
        /* rescale hash table */
        U32 delta = LZ5_dict->currentOffset - LZ5_DICT_SIZE;
        const BYTE* dictEnd = LZ5_dict->dictionary + LZ5_dict->dictSize;
        int i;
        for (i=0; i<HASH_SIZE_U32; i++)
        {
            if (LZ5_dict->hashTable[i] < delta) LZ5_dict->hashTable[i]=0;
            else LZ5_dict->hashTable[i] -= delta;
        }
        LZ5_dict->currentOffset = LZ5_DICT_SIZE;
        if (LZ5_dict->dictSize > LZ5_DICT_SIZE) LZ5_dict->dictSize = LZ5_DICT_SIZE;
        LZ5_dict->dictionary = dictEnd - LZ5_dict->dictSize;
    }
}


int LZ5_compress_fast_continue (LZ5_stream_t* LZ5_stream, const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ5_stream_t_internal* streamPtr = (LZ5_stream_t_internal*)LZ5_stream;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = (const BYTE*) source;
    if (streamPtr->initCheck) return 0;   /* Uninitialized structure detected */
    if ((streamPtr->dictSize>0) && (smallest>dictEnd)) smallest = dictEnd;
    LZ5_renormDictT(streamPtr, smallest);
    if (acceleration < 1) acceleration = ACCELERATION_DEFAULT;

    /* Check overlapping input/dictionary space */
    {
        const BYTE* sourceEnd = (const BYTE*) source + inputSize;
        if ((sourceEnd > streamPtr->dictionary) && (sourceEnd < dictEnd))
        {
            streamPtr->dictSize = (U32)(dictEnd - sourceEnd);
            if (streamPtr->dictSize > LZ5_DICT_SIZE) streamPtr->dictSize = LZ5_DICT_SIZE;
            if (streamPtr->dictSize < 4) streamPtr->dictSize = 0;
            streamPtr->dictionary = dictEnd - streamPtr->dictSize;
        }
    }

    /* prefix mode : source data follows dictionary */
    if (dictEnd == (const BYTE*)source)
    {
        int result;
        if ((streamPtr->dictSize < LZ5_DICT_SIZE) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ5_compress_generic(LZ5_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, withPrefix64k, dictSmall, acceleration);
        else
            result = LZ5_compress_generic(LZ5_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, withPrefix64k, noDictIssue, acceleration);
        streamPtr->dictSize += (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }

    /* external dictionary mode */
    {
        int result;
        if ((streamPtr->dictSize < LZ5_DICT_SIZE) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ5_compress_generic(LZ5_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, usingExtDict, dictSmall, acceleration);
        else
            result = LZ5_compress_generic(LZ5_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, usingExtDict, noDictIssue, acceleration);
        streamPtr->dictionary = (const BYTE*)source;
        streamPtr->dictSize = (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }
}


/* Hidden debug function, to force external dictionary mode */
int LZ5_compress_forceExtDict (LZ5_stream_t* LZ5_dict, const char* source, char* dest, int inputSize)
{
    LZ5_stream_t_internal* streamPtr = (LZ5_stream_t_internal*)LZ5_dict;
    int result;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = dictEnd;
    if (smallest > (const BYTE*) source) smallest = (const BYTE*) source;
    LZ5_renormDictT((LZ5_stream_t_internal*)LZ5_dict, smallest);

    result = LZ5_compress_generic(LZ5_dict, source, dest, inputSize, 0, notLimited, byU32, usingExtDict, noDictIssue, 1);

    streamPtr->dictionary = (const BYTE*)source;
    streamPtr->dictSize = (U32)inputSize;
    streamPtr->currentOffset += (U32)inputSize;

    return result;
}


int LZ5_saveDict (LZ5_stream_t* LZ5_dict, char* safeBuffer, int dictSize)
{
    LZ5_stream_t_internal* dict = (LZ5_stream_t_internal*) LZ5_dict;
    const BYTE* previousDictEnd = dict->dictionary + dict->dictSize;

    if ((U32)dictSize > LZ5_DICT_SIZE) dictSize = LZ5_DICT_SIZE;   /* useless to define a dictionary > LZ5_DICT_SIZE */
    if ((U32)dictSize > dict->dictSize) dictSize = dict->dictSize;

    memmove(safeBuffer, previousDictEnd - dictSize, dictSize);

    dict->dictionary = (const BYTE*)safeBuffer;
    dict->dictSize = (U32)dictSize;

    return dictSize;
}



/*******************************
*  Decompression functions
*******************************/
/*
 * This generic decompression function cover all use cases.
 * It shall be instantiated several times, using different sets of directives
 * Note that it is essential this generic function is really inlined,
 * in order to remove useless branches during compilation optimization.
 */
FORCE_INLINE int LZ5_decompress_generic(
                 const char* const source,
                 char* const dest,
                 int inputSize,
                 int outputSize,         /* If endOnInput==endOnInputSize, this value is the max size of Output Buffer. */

                 int endOnInput,         /* endOnOutputSize, endOnInputSize */
                 int partialDecoding,    /* full, partial */
                 int targetOutputSize,   /* only used if partialDecoding==partial */
                 int dict,               /* noDict, withPrefix64k, usingExtDict */
                 const BYTE* const lowPrefix,  /* == dest if dict == noDict */
                 const BYTE* const dictStart,  /* only if dict==usingExtDict */
                 const size_t dictSize         /* note : = 0 if noDict */
                 )
{
    /* Local Variables */
    const BYTE* ip = (const BYTE*) source;
    const BYTE* const iend = ip + inputSize;

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + outputSize;
    BYTE* cpy;
    BYTE* oexit = op + targetOutputSize;
    const BYTE* const lowLimit = lowPrefix - dictSize;

    const BYTE* const dictEnd = (const BYTE*)dictStart + dictSize;
    const unsigned dec32table[] = {4, 1, 2, 1, 4, 4, 4, 4};
    const int dec64table[] = {0, 0, 0, -1, 0, 1, 2, 3};

    const int safeDecode = (endOnInput==endOnInputSize);
    const int checkOffset = ((safeDecode) && (dictSize < (int)(LZ5_DICT_SIZE)));


    /* Special cases */
    if ((partialDecoding) && (oexit> oend-MFLIMIT)) oexit = oend-MFLIMIT;                         /* targetOutputSize too high => decode everything */
    if ((endOnInput) && (unlikely(outputSize==0))) return ((inputSize==1) && (*ip==0)) ? 0 : -1;  /* Empty output buffer */
    if ((!endOnInput) && (unlikely(outputSize==0))) return (*ip==0?1:-1);


    /* Main Loop */
    while (1)
    {
        unsigned token;
        size_t length;
        const BYTE* match;
        size_t offset;

        /* get literal length */
        token = *ip++;
        if (token>>7)
        {
            if ((length=(token>>ML_BITS)&RUN_MASK2) == RUN_MASK2)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    length += s;
                }
                while (likely((endOnInput)?ip<iend-RUN_MASK2:1) && (s==255));
                if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)(op))) goto _output_error;   /* overflow detection */
                if ((safeDecode) && unlikely((size_t)(ip+length)<(size_t)(ip))) goto _output_error;   /* overflow detection */
            }
        }
        else
        {
            if ((length=(token>>ML_BITS)&RUN_MASK) == RUN_MASK)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    length += s;
                }
                while (likely((endOnInput)?ip<iend-RUN_MASK:1) && (s==255));
                if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)(op))) goto _output_error;   /* overflow detection */
                if ((safeDecode) && unlikely((size_t)(ip+length)<(size_t)(ip))) goto _output_error;   /* overflow detection */
            }
        }

        /* copy literals */
        cpy = op+length;
        if (((endOnInput) && ((cpy>(partialDecoding?oexit:oend-MFLIMIT)) || (ip+length>iend-(1+1+LASTLITERALS))) )
            || ((!endOnInput) && (cpy>oend-WILDCOPYLENGTH)))
        {
            if (partialDecoding)
            {
                if (cpy > oend) goto _output_error;                           /* Error : write attempt beyond end of output buffer */
                if ((endOnInput) && (ip+length > iend)) goto _output_error;   /* Error : read attempt beyond end of input buffer */
            }
            else
            {
                if ((!endOnInput) && (cpy != oend)) goto _output_error;       /* Error : block decoding must stop exactly there */
                if ((endOnInput) && ((ip+length != iend) || (cpy > oend))) goto _output_error;   /* Error : input must be consumed */
            }
            memcpy(op, ip, length);
            ip += length;
            op += length;
            break;     /* Necessarily EOF, due to parsing restrictions */
        }
        LZ5_wildCopy(op, ip, cpy);
        ip += length; op = cpy;

        /* get offset */
        if (token>>7)
        {
            offset = *ip + (((token>>ML_RUN_BITS2)&3)<<8); ip++;
        }
        else 
        if ((token>>ML_RUN_BITS) == 0)
        {
            offset = LZ5_readLE16(ip); ip+=2;
        }
        else // length == 1
        {
            offset = LZ5_readLE24(ip); ip+=3;
        }
        match = op - offset;
        if ((checkOffset) && (unlikely(match < lowLimit))) goto _output_error;   /* Error : offset outside buffers */

        /* get matchlength */
        length = token & ML_MASK;
        if (length == ML_MASK)
        {
            unsigned s;
            do
            {
                if ((endOnInput) && (ip > iend-LASTLITERALS)) goto _output_error;
                s = *ip++;
                length += s;
            } while (s==255);
            if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)op)) goto _output_error;   /* overflow detection */
        }
        length += MINMATCH;

        /* check external dictionary */
        if ((dict==usingExtDict) && (match < lowPrefix))
        {
            if (unlikely(op+length > oend-LASTLITERALS)) goto _output_error;   /* doesn't respect parsing restriction */

            if (length <= (size_t)(lowPrefix-match))
            {
                /* match can be copied as a single segment from external dictionary */
                match = dictEnd - (lowPrefix-match);
                memmove(op, match, length); op += length;
            }
            else
            {
                /* match encompass external dictionary and current block */
                size_t copySize = (size_t)(lowPrefix-match);
                memcpy(op, dictEnd - copySize, copySize);
                op += copySize;
                copySize = length - copySize;
                if (copySize > (size_t)(op-lowPrefix))   /* overlap copy */
                {
                    BYTE* const endOfMatch = op + copySize;
                    const BYTE* copyFrom = lowPrefix;
                    while (op < endOfMatch) *op++ = *copyFrom++;
                }
                else
                {
                    memcpy(op, lowPrefix, copySize);
                    op += copySize;
                }
            }
            continue;
        }

        /* copy match within block */
        cpy = op + length;
        if (unlikely(offset<8))
        {
            const int dec64 = dec64table[offset];
            op[0] = match[0];
            op[1] = match[1];
            op[2] = match[2];
            op[3] = match[3];
            match += dec32table[offset];
            memcpy(op+4, match, 4);
            match -= dec64;
        } else { LZ5_copy8(op, match); match+=8; }
        op += 8;

        if (unlikely(cpy>oend-12))
        {
            BYTE* const oCopyLimit = oend-(WILDCOPYLENGTH-1);
            if (cpy > oend-LASTLITERALS) goto _output_error;    /* Error : last LASTLITERALS bytes must be literals (uncompressed) */
            if (op < oCopyLimit)
            {
                LZ5_wildCopy(op, match, oCopyLimit);
                match += oCopyLimit - op;
                op = oCopyLimit;
            }
            while (op<cpy) *op++ = *match++;
        }
        else
            LZ5_wildCopy(op, match, cpy);
        op=cpy;   /* correction */
    }

    /* end of decoding */
    if (endOnInput)
       return (int) (((char*)op)-dest);     /* Nb of output bytes decoded */
    else
       return (int) (((const char*)ip)-source);   /* Nb of input bytes read */

    /* Overflow error detected */
_output_error:
    return (int) (-(((const char*)ip)-source))-1;
}


int LZ5_decompress_safe(const char* source, char* dest, int compressedSize, int maxDecompressedSize)
{
    return LZ5_decompress_generic(source, dest, compressedSize, maxDecompressedSize, endOnInputSize, full, 0, noDict, (BYTE*)dest, NULL, 0);
}

int LZ5_decompress_safe_partial(const char* source, char* dest, int compressedSize, int targetOutputSize, int maxDecompressedSize)
{
    return LZ5_decompress_generic(source, dest, compressedSize, maxDecompressedSize, endOnInputSize, partial, targetOutputSize, noDict, (BYTE*)dest, NULL, 0);
}

int LZ5_decompress_fast(const char* source, char* dest, int originalSize)
{
    return LZ5_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, withPrefix64k, (BYTE*)(dest - LZ5_DICT_SIZE), NULL, LZ5_DICT_SIZE);
}


/* streaming decompression functions */

typedef struct
{
    const BYTE* externalDict;
    size_t extDictSize;
    const BYTE* prefixEnd;
    size_t prefixSize;
} LZ5_streamDecode_t_internal;

/*
 * If you prefer dynamic allocation methods,
 * LZ5_createStreamDecode()
 * provides a pointer (void*) towards an initialized LZ5_streamDecode_t structure.
 */
LZ5_streamDecode_t* LZ5_createStreamDecode(void)
{
    LZ5_streamDecode_t* lz5s = (LZ5_streamDecode_t*) ALLOCATOR(1, sizeof(LZ5_streamDecode_t));
    return lz5s;
}

int LZ5_freeStreamDecode (LZ5_streamDecode_t* LZ5_stream)
{
    FREEMEM(LZ5_stream);
    return 0;
}

/*
 * LZ5_setStreamDecode
 * Use this function to instruct where to find the dictionary
 * This function is not necessary if previous data is still available where it was decoded.
 * Loading a size of 0 is allowed (same effect as no dictionary).
 * Return : 1 if OK, 0 if error
 */
int LZ5_setStreamDecode (LZ5_streamDecode_t* LZ5_streamDecode, const char* dictionary, int dictSize)
{
    LZ5_streamDecode_t_internal* lz5sd = (LZ5_streamDecode_t_internal*) LZ5_streamDecode;
    lz5sd->prefixSize = (size_t) dictSize;
    lz5sd->prefixEnd = (const BYTE*) dictionary + dictSize;
    lz5sd->externalDict = NULL;
    lz5sd->extDictSize  = 0;
    return 1;
}

/*
*_continue() :
    These decoding functions allow decompression of multiple blocks in "streaming" mode.
    Previously decoded blocks must still be available at the memory position where they were decoded.
    If it's not possible, save the relevant part of decoded data into a safe buffer,
    and indicate where it stands using LZ5_setStreamDecode()
*/
int LZ5_decompress_safe_continue (LZ5_streamDecode_t* LZ5_streamDecode, const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    LZ5_streamDecode_t_internal* lz5sd = (LZ5_streamDecode_t_internal*) LZ5_streamDecode;
    int result;

    if (lz5sd->prefixEnd == (BYTE*)dest)
    {
        result = LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize,
                                        endOnInputSize, full, 0,
                                        usingExtDict, lz5sd->prefixEnd - lz5sd->prefixSize, lz5sd->externalDict, lz5sd->extDictSize);
        if (result <= 0) return result;
        lz5sd->prefixSize += result;
        lz5sd->prefixEnd  += result;
    }
    else
    {
        lz5sd->extDictSize = lz5sd->prefixSize;
        lz5sd->externalDict = lz5sd->prefixEnd - lz5sd->extDictSize;
        result = LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize,
                                        endOnInputSize, full, 0,
                                        usingExtDict, (BYTE*)dest, lz5sd->externalDict, lz5sd->extDictSize);
        if (result <= 0) return result;
        lz5sd->prefixSize = result;
        lz5sd->prefixEnd  = (BYTE*)dest + result;
    }

    return result;
}

int LZ5_decompress_fast_continue (LZ5_streamDecode_t* LZ5_streamDecode, const char* source, char* dest, int originalSize)
{
    LZ5_streamDecode_t_internal* lz5sd = (LZ5_streamDecode_t_internal*) LZ5_streamDecode;
    int result;

    if (lz5sd->prefixEnd == (BYTE*)dest)
    {
        result = LZ5_decompress_generic(source, dest, 0, originalSize,
                                        endOnOutputSize, full, 0,
                                        usingExtDict, lz5sd->prefixEnd - lz5sd->prefixSize, lz5sd->externalDict, lz5sd->extDictSize);
        if (result <= 0) return result;
        lz5sd->prefixSize += originalSize;
        lz5sd->prefixEnd  += originalSize;
    }
    else
    {
        lz5sd->extDictSize = lz5sd->prefixSize;
        lz5sd->externalDict = (BYTE*)dest - lz5sd->extDictSize;
        result = LZ5_decompress_generic(source, dest, 0, originalSize,
                                        endOnOutputSize, full, 0,
                                        usingExtDict, (BYTE*)dest, lz5sd->externalDict, lz5sd->extDictSize);
        if (result <= 0) return result;
        lz5sd->prefixSize = originalSize;
        lz5sd->prefixEnd  = (BYTE*)dest + originalSize;
    }

    return result;
}


/*
Advanced decoding functions :
*_usingDict() :
    These decoding functions work the same as "_continue" ones,
    the dictionary must be explicitly provided within parameters
*/

FORCE_INLINE int LZ5_decompress_usingDict_generic(const char* source, char* dest, int compressedSize, int maxOutputSize, int safe, const char* dictStart, int dictSize)
{
    if (dictSize==0)
        return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, noDict, (BYTE*)dest, NULL, 0);
    if (dictStart+dictSize == dest)
    {
        if (dictSize >= (int)(LZ5_DICT_SIZE - 1))
            return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, withPrefix64k, (BYTE*)dest-LZ5_DICT_SIZE, NULL, 0);
        return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, noDict, (BYTE*)dest-dictSize, NULL, 0);
    }
    return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, usingExtDict, (BYTE*)dest, (const BYTE*)dictStart, dictSize);
}

int LZ5_decompress_safe_usingDict(const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize)
{
    return LZ5_decompress_usingDict_generic(source, dest, compressedSize, maxOutputSize, 1, dictStart, dictSize);
}

int LZ5_decompress_fast_usingDict(const char* source, char* dest, int originalSize, const char* dictStart, int dictSize)
{
    return LZ5_decompress_usingDict_generic(source, dest, 0, originalSize, 0, dictStart, dictSize);
}

/* debug function */
int LZ5_decompress_safe_forceExtDict(const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize)
{
    return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, usingExtDict, (BYTE*)dest, (const BYTE*)dictStart, dictSize);
}


/***************************************************
*  Obsolete Functions
***************************************************/
/* obsolete compression functions */
int LZ5_compress_limitedOutput(const char* source, char* dest, int inputSize, int maxOutputSize) { return LZ5_compress_default(source, dest, inputSize, maxOutputSize); }
int LZ5_compress(const char* source, char* dest, int inputSize) { return LZ5_compress_default(source, dest, inputSize, LZ5_compressBound(inputSize)); }
int LZ5_compress_limitedOutput_withState (void* state, const char* src, char* dst, int srcSize, int dstSize) { return LZ5_compress_fast_extState(state, src, dst, srcSize, dstSize, 1); }
int LZ5_compress_withState (void* state, const char* src, char* dst, int srcSize) { return LZ5_compress_fast_extState(state, src, dst, srcSize, LZ5_compressBound(srcSize), 1); }
int LZ5_compress_limitedOutput_continue (LZ5_stream_t* LZ5_stream, const char* src, char* dst, int srcSize, int maxDstSize) { return LZ5_compress_fast_continue(LZ5_stream, src, dst, srcSize, maxDstSize, 1); }
int LZ5_compress_continue (LZ5_stream_t* LZ5_stream, const char* source, char* dest, int inputSize) { return LZ5_compress_fast_continue(LZ5_stream, source, dest, inputSize, LZ5_compressBound(inputSize), 1); }

/*
These function names are deprecated and should no longer be used.
They are only provided here for compatibility with older user programs.
- LZ5_uncompress is totally equivalent to LZ5_decompress_fast
- LZ5_uncompress_unknownOutputSize is totally equivalent to LZ5_decompress_safe
*/
int LZ5_uncompress (const char* source, char* dest, int outputSize) { return LZ5_decompress_fast(source, dest, outputSize); }
int LZ5_uncompress_unknownOutputSize (const char* source, char* dest, int isize, int maxOutputSize) { return LZ5_decompress_safe(source, dest, isize, maxOutputSize); }


/* Obsolete Streaming functions */

int LZ5_sizeofStreamState() { return LZ5_STREAMSIZE; }

static void LZ5_init(LZ5_stream_t_internal* lz5ds, BYTE* base)
{
    MEM_INIT(lz5ds, 0, LZ5_STREAMSIZE);
    lz5ds->bufferStart = base;
}

int LZ5_resetStreamState(void* state, char* inputBuffer)
{
    if ((((size_t)state) & 3) != 0) return 1;   /* Error : pointer is not aligned on 4-bytes boundary */
    LZ5_init((LZ5_stream_t_internal*)state, (BYTE*)inputBuffer);
    return 0;
}

void* LZ5_create (char* inputBuffer)
{
    void* lz5ds = ALLOCATOR(8, LZ5_STREAMSIZE_U64);
    LZ5_init ((LZ5_stream_t_internal*)lz5ds, (BYTE*)inputBuffer);
    return lz5ds;
}

char* LZ5_slideInputBuffer (void* LZ5_Data)
{
    LZ5_stream_t_internal* ctx = (LZ5_stream_t_internal*)LZ5_Data;
    int dictSize = LZ5_saveDict((LZ5_stream_t*)LZ5_Data, (char*)ctx->bufferStart, LZ5_DICT_SIZE);
    return (char*)(ctx->bufferStart + dictSize);
}

/* Obsolete streaming decompression functions */

int LZ5_decompress_safe_withPrefix64k(const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    return LZ5_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, withPrefix64k, (BYTE*)dest - LZ5_DICT_SIZE, NULL, LZ5_DICT_SIZE);
}

int LZ5_decompress_fast_withPrefix64k(const char* source, char* dest, int originalSize)
{
    return LZ5_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, withPrefix64k, (BYTE*)dest - LZ5_DICT_SIZE, NULL, LZ5_DICT_SIZE);
}

#endif   /* LZ5_COMMONDEFS_ONLY */

