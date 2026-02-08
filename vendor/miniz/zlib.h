#pragma once

// We want to control the compatibility names ourselves to match GDSTK
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

// Basic types
typedef unsigned char  Byte;
typedef unsigned char  Bytef;
typedef size_t         uInt;
typedef unsigned int   uIntf;
typedef unsigned long  uLong;
typedef uLong          uLongf;
typedef void*          voidpf;
typedef void*          voidp;

#define Z_NULL 0

// Flush values
#define Z_NO_FLUSH      MZ_NO_FLUSH
#define Z_PARTIAL_FLUSH MZ_PARTIAL_FLUSH
#define Z_SYNC_FLUSH    MZ_SYNC_FLUSH
#define Z_FULL_FLUSH    MZ_FULL_FLUSH
#define Z_FINISH        MZ_FINISH
#define Z_BLOCK         MZ_BLOCK

// Return codes
#define Z_OK            MZ_OK
#define Z_STREAM_END    MZ_STREAM_END
#define Z_NEED_DICT     MZ_NEED_DICT
#define Z_ERRNO         MZ_ERRNO
#define Z_STREAM_ERROR  MZ_STREAM_ERROR
#define Z_DATA_ERROR    MZ_DATA_ERROR
#define Z_MEM_ERROR     MZ_MEM_ERROR
#define Z_BUF_ERROR     MZ_BUF_ERROR
#define Z_VERSION_ERROR MZ_VERSION_ERROR

// Compression levels
#define Z_NO_COMPRESSION      MZ_NO_COMPRESSION
#define Z_BEST_SPEED          MZ_BEST_SPEED
#define Z_BEST_COMPRESSION    MZ_BEST_COMPRESSION
#define Z_DEFAULT_COMPRESSION MZ_DEFAULT_COMPRESSION

// Strategies
#define Z_FILTERED            MZ_FILTERED
#define Z_HUFFMAN_ONLY        MZ_HUFFMAN_ONLY
#define Z_RLE                 MZ_RLE
#define Z_FIXED               MZ_FIXED
#define Z_DEFAULT_STRATEGY    MZ_DEFAULT_STRATEGY

#define Z_DEFLATED            MZ_DEFLATED

// GDSTK uses these for its callbacks
typedef voidpf (*alloc_func) (voidpf opaque, uInt items, uInt size);
typedef void   (*free_func)  (voidpf opaque, voidpf address);

// Redefine z_stream to match miniz's mz_stream but with uInt as size_t
// and mapping to miniz.
#define internal_state mz_internal_state
#define z_stream       mz_stream
#define z_streamp      mz_streamp

// Bridge functions
#define deflateInit2(strm, level, method, windowBits, memLevel, strategy) \
    mz_deflateInit2(strm, level, method, windowBits, memLevel, strategy)
#define deflate(strm, flush) mz_deflate(strm, flush)
#define deflateEnd(strm)     mz_deflateEnd(strm)
#define deflateBound(strm, sourceLen) mz_deflateBound(strm, sourceLen)

#define inflateInit2(strm, windowBits) mz_inflateInit2(strm, windowBits)
#define inflate(strm, flush)           mz_inflate(strm, flush)
#define inflateEnd(strm)               mz_inflateEnd(strm)

#define crc32(crc, ptr, len)           mz_crc32(crc, ptr, len)
#define adler32(adler, ptr, len)       mz_adler32(adler, ptr, len)

// Fix fread issue (const cast)
#include <stdio.h>
#define fread(ptr, size, count, stream) fread((void*)(ptr), (size), (count), (stream))
