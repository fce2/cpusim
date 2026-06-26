/**
 * compress.h - Unified compression library
 *
 *   - DEFLATE decompression (RFC 1951)
 *   - DEFLATE compression (RFC 1951)
 *   - gzip decompression (RFC 1952)
 *   - ZIP format reader/writer
 *   - zlib wrapper (RFC 1950)
 *   - LZ77 compression / decompression
 *
 */

#ifndef RETROLIB_COMPRESS_H
#define RETROLIB_COMPRESS_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* C89 compatibility: provide stdint types when <stdint.h> is unavailable */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
#include <stdint.h>
#else
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;
#endif

/* 64-bit file seek portability */
#if defined(_MSC_VER)
  #define COMP_FSEEK _fseeki64
  #define COMP_FTELL _ftelli64
#elif defined(__MINGW32__) || defined(__MINGW64__)
  #define COMP_FSEEK fseeko64
  #define COMP_FTELL ftello64
#else
  #define COMP_FSEEK fseeko
  #define COMP_FTELL ftello
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  DEFLATE / inflate / gzip                                           */
/* ------------------------------------------------------------------ */

/** Decompress DEFLATE data (RFC 1951). Returns uncompressed size, or -1. */
int comp_inflate(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/** Decompress gzip data (RFC 1952). Returns uncompressed size, or -1. */
int comp_gunzip(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/** Compress into gzip format (10-byte header + DEFLATE body + 8-byte
    CRC32/ISIZE trailer). dst_cap must cover the fixed-Huffman worst case
    (~1.125x src + overhead) — pass at least src_len + src_len/8 + 64, since
    comp_deflate has no stored-block fallback and silently truncates past cap.
    Returns compressed size, or -1. */
int comp_gzip(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/** Compress data using DEFLATE (RFC 1951). Returns compressed size, or -1. */
int comp_deflate(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/* ------------------------------------------------------------------ */
/*  Zlib (RFC 1950)                                                    */
/* ------------------------------------------------------------------ */

/** Decompress zlib data (CMF+FLG header + deflate + adler32). */
int comp_zlib_decompress(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/** Compress into zlib format (CMF+FLG header + deflate + adler32). */
int comp_zlib_compress(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap);

/* ------------------------------------------------------------------ */
/*  ZIP format                                                         */
/* ------------------------------------------------------------------ */

#ifndef ZIP_MAX_ENTRIES
#define ZIP_MAX_ENTRIES 256
#endif
#define ZIP_NAME_LEN    255

#define ZIP_LFH_SIZE   30
#define ZIP_CD_SIZE     46
#define ZIP_EOCD_SIZE   22

#define SIG_LOCAL      0x04034B50u
#define SIG_CENTRAL    0x02014B50u
#define SIG_EOCD       0x06054B50u
#define SIG_Z64_EOCD   0x06064B50u   /* Zip64 End of Central Directory */
#define SIG_Z64_LOC    0x07064B50u   /* Zip64 EOCD Locator */

#define ZIP_Z64_EOCD_SIZE  56   /* fixed-size part of Zip64 EOCD record */
#define ZIP_Z64_LOC_SIZE   20   /* Zip64 EOCD Locator record size */

typedef struct ZipEntry {
    char name[ZIP_NAME_LEN + 1];
    int compress_type;        /* 0=stored, 8=deflated */
    int64_t compress_size;
    int64_t uncompressed_size;
    int64_t offset;           /* absolute file offset of Local File Header */
    uint32_t crc;
    uint16_t mod_time;
    uint16_t mod_date;
} ZipEntry;

typedef struct ZipFile {
    uint8_t* data;          /* NULL for file-backed, heap buffer for mem-backed */
    int64_t data_len;       /* 0 for file-backed; length for mem-backed */
    FILE* fp;               /* non-NULL for file-backed, NULL for mem-backed */
    ZipEntry* entries;      /* dynamic array (malloc'd) */
    int entry_count;
    int entries_cap;         /* capacity of entries array */
} ZipFile;

typedef struct ZipWriterEntry {
    char name[ZIP_NAME_LEN + 1];
    uint8_t* data;
    int data_len;
    uint32_t crc;
    uint16_t mod_time;
    uint16_t mod_date;
} ZipWriterEntry;

typedef struct ZipWriter {
    ZipWriterEntry entries[ZIP_MAX_ENTRIES];
    int entry_count;
} ZipWriter;

int     comp_zip_open(ZipFile* z, const char* path);
int     comp_zip_open_mem(ZipFile* z, const uint8_t* data, int data_len);
void    comp_zip_init(ZipFile* z);
void    comp_zip_close(ZipFile* z);
int     comp_zip_read_entry(ZipFile* z, int idx, uint8_t* buf, int buf_cap);
int     comp_zip_find(ZipFile* z, const char* suffix);

ZipWriter* comp_zip_writer_open(void);
int     comp_zip_writer_add(ZipWriter* w, const char* name, const char* path);
int     comp_zip_writer_add_data(ZipWriter* w, const char* name, const uint8_t* data, int len);
int     comp_zip_writer_add_data_ex(ZipWriter* w, const char* name, const uint8_t* data, int len, uint16_t mod_time, uint16_t mod_date);
void    comp_zip_dos_time(time_t t, uint16_t* out_time, uint16_t* out_date);
int     comp_zip_writer_close(ZipWriter* w, const char* path);
uint32_t comp_crc32(const uint8_t* data, int len);
uint32_t comp_crc32_update(uint32_t crc, const uint8_t* data, int len);
uint32_t comp_crc32_file(const char* path);

/* ------------------------------------------------------------------ */
/*  LZ77 compression / decompression                                   */
/* ------------------------------------------------------------------ */

/** Decompress LZ-compressed data. Returns uncompressed size, or 0 on error. */
int comp_lz_uncompress(const unsigned char *in, int insize, unsigned char *out, int out_hint);

/** Compress using LZ77. Returns compressed size, or 0. */
int comp_lz_compress(const unsigned char *in, int insize, unsigned char *out, int outsize);

/* Backward compatibility: old pack.h/inflate.h API names */
#define zip_open            comp_zip_open
#define zip_close           comp_zip_close
#define zip_find            comp_zip_find
#define zip_read_entry      comp_zip_read_entry
#define zip_writer_open     comp_zip_writer_open
#define zip_writer_add_data comp_zip_writer_add_data
#define zip_writer_close    comp_zip_writer_close
#define zip_crc32           comp_crc32
#define gunzip              comp_gunzip

#ifdef __cplusplus
}
#endif

#endif /* RETROLIB_COMPRESS_H */