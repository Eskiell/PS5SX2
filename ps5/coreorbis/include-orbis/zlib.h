#pragma once
// Orbis zlib parse shim: PCSX2 compressed-image readers (CSO/GZIP) need the
// z_stream type to compile. There is no zlib in ps5-payload-sdk, so those
// readers are stubbed (ProsperoCDVD.cpp) and nothing calls zlib functions.
#include <stddef.h>

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;

typedef struct z_stream_s
{
  Bytef* next_in;
  uInt avail_in;
  uLong total_in;
  Bytef* next_out;
  uInt avail_out;
  uLong total_out;
  char* msg;
  void* state;
  void* zalloc;
  void* zfree;
  void* opaque;
  int data_type;
  uLong adler;
  uLong reserved;
} z_stream;

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NEED_DICT 2
#define Z_ERRNO (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)
#define Z_NO_FLUSH 0
#define Z_SYNC_FLUSH 2
#define Z_FINISH 4
#define Z_BLOCK 5
#define Z_DEFAULT_COMPRESSION (-1)
#define Z_DEFLATED 8
#define Z_DEFAULT_STRATEGY 0
#define Z_NULL 0

#ifdef __cplusplus
extern "C" {
#endif

int inflateInit2(z_stream* strm, int windowBits);
int inflate(z_stream* strm, int flush);
int inflateEnd(z_stream* strm);
int inflatePrime(z_stream* strm, int bits, int value);
int inflateSetDictionary(z_stream* strm, const Bytef* dictionary, uInt dictLength);

#ifdef __cplusplus
}
#endif
