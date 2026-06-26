/**
 * compress.c - Unified compression library
 *
 * Single-source implementation of:
 *   DEFLATE decompression (RFC 1951), DEFLATE compression,
 *   gzip decompression (RFC 1952), ZIP format reader/writer,
 *   zlib wrapper (RFC 1950), LZ77 compression / decompression.
 *
 * All public symbols use the comp_ prefix. See compress.h for API.
 * Pure C89.
 */

#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ================================================================== */
/*  Shared DEFLATE tables (RFC 1951 sec 3.2.5)                         */
/* ================================================================== */

static const uint16_t def_len_base[29] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const uint8_t def_len_extra[29] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};
static const uint16_t def_dist_base[30] = {
    1,2,3,4,5,7,9,13, 17,25,33,49,65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289,
    16385,24577
};
static const uint8_t def_dist_extra[30] = {
    0,0,0,0,1,1,2,2, 3,3,4,4,5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

/* ================================================================== */
/*  CRC-32 (shared by ZIP reader/writer)                               */
/* ================================================================== */

static uint32_t crc_table[256];
static int crc_table_init = 0;

static void crc_init(void) {
    uint32_t i, c;
    int j;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_init = 1;
}

uint32_t comp_crc32(const uint8_t* data, int len) {
    uint32_t crc;
    int i;
    if (!crc_table_init) crc_init();
    crc = 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/** Incremental CRC32: update an existing CRC with more data.
 *  Start with crc = 0xFFFFFFFFu, call for each chunk, then XOR with 0xFFFFFFFFu. */
uint32_t comp_crc32_update(uint32_t crc, const uint8_t* data, int len) {
    int i;
    if (!crc_table_init) crc_init();
    for (i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

/** Stream CRC32 of a file without loading it all into memory.
 *  Reads in 64KB chunks. Returns 0 on error. */
uint32_t comp_crc32_file(const char* path) {
    FILE* fp;
    uint32_t crc;
    uint8_t buf[65536];
    int n;

    if (!crc_table_init) crc_init();
    fp = fopen(path, "rb");
    if (!fp) return 0;
    crc = 0xFFFFFFFFu;
    while ((n = (int)fread(buf, 1, sizeof(buf), fp)) > 0)
        crc = comp_crc32_update(crc, buf, n);
    fclose(fp);
    return crc ^ 0xFFFFFFFFu;
}

/* ================================================================== */
/*  DEFLATE decompressor (RFC 1951)                                    */
/* ================================================================== */

typedef struct {
    const uint8_t* src;
    int src_pos;
    int src_len;
    uint32_t bit_buf;
    int bit_cnt;
    uint8_t* dst;
    int dst_pos;
    int dst_cap;
} Inf;

static void inf_init(Inf* s, const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    s->src = src; s->src_pos = 0; s->src_len = src_len;
    s->bit_buf = 0; s->bit_cnt = 0;
    s->dst = dst; s->dst_pos = 0; s->dst_cap = dst_cap;
}

static void inf_fill(Inf* s) {
    while (s->bit_cnt <= 24 && s->src_pos < s->src_len) {
        s->bit_buf |= (uint32_t)s->src[s->src_pos++] << s->bit_cnt;
        s->bit_cnt += 8;
    }
}

static uint32_t inf_bits(Inf* s, int n) {
    uint32_t v;
    while (s->bit_cnt < n) {
        if (s->src_pos >= s->src_len) return 0;
        s->bit_buf |= (uint32_t)s->src[s->src_pos++] << s->bit_cnt;
        s->bit_cnt += 8;
    }
    v = s->bit_buf & ((1u << n) - 1);
    s->bit_buf >>= n;
    s->bit_cnt -= n;
    return v;
}

static void inf_output(Inf* s, uint8_t byte) {
    if (s->dst_pos < s->dst_cap)
        s->dst[s->dst_pos++] = byte;
}

static void inf_copy(Inf* s, int src, int length) {
    int end, avail, i;
    end = s->dst_pos + length;
    if (end > s->dst_cap) end = s->dst_cap;
    avail = end - s->dst_pos;
    if (avail <= 0) { s->dst_pos += length; return; }
    if (src + length <= s->dst_pos) {
        memcpy(s->dst + s->dst_pos, s->dst + src, avail);
        s->dst_pos += length;
    } else {
        for (i = 0; i < avail; i++)
            s->dst[s->dst_pos + i] = s->dst[src + i];
        s->dst_pos += length;
    }
}

/* ------------------------------------------------------------------ */
/*  Huffman tables (9-bit fast lookup + canonical fallback)             */
/* ------------------------------------------------------------------ */

#define HUFF_BITS 9
#define HUFF_SIZE (1 << HUFF_BITS)

typedef struct {
    uint16_t fast[HUFF_SIZE];
    uint16_t counts[16];
    uint16_t symbols[288];
} Huff;

#define HUFF_PACK(sym, len) ((uint16_t)((sym) << 5) | (uint16_t)(len))
#define HUFF_SYM(e) ((e) >> 5)
#define HUFF_LEN(e) ((e) & 0x1F)

static const uint8_t rev8[256] = {
    0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
    0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
    0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
    0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
    0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
    0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
    0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
    0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
    0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
    0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
    0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
    0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
    0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
    0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
    0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
    0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF
};

static uint32_t bit_rev(uint32_t v, int bits) {
    uint32_t r;
    r = rev8[v & 0xFF];
    if (bits <= 8) return r >> (8 - bits);
    r = (r << 8) | rev8[(v >> 8) & 0xFF];
    return r >> (16 - bits);
}

static void huff_build(Huff* h, const uint8_t* lengths, int count) {
    uint16_t offsets[16], fill_off[16];
    int first[16];
    int i, bits, sym, len, code, rv, fill;
    uint16_t packed;

    memset(h->counts, 0, sizeof(h->counts));
    for (i = 0; i < count; i++)
        if (lengths[i] > 0) h->counts[lengths[i]]++;

    offsets[0] = 0;
    for (bits = 1; bits < 16; bits++)
        offsets[bits] = offsets[bits - 1] + h->counts[bits - 1];

    memcpy(fill_off, offsets, sizeof(fill_off));

    for (i = 0; i < count; i++)
        if (lengths[i] > 0)
            h->symbols[fill_off[lengths[i]]++] = (uint16_t)i;

    memset(h->fast, 0, sizeof(h->fast));

    for (i = 0; i < 16; i++) first[i] = 0;
    for (bits = 1; bits <= 15; bits++)
        first[bits] = (first[bits - 1] + h->counts[bits - 1]) << 1;

    for (sym = 0; sym < count; sym++) {
        len = lengths[sym];
        if (len == 0 || len > HUFF_BITS) continue;
        code = first[len]++;
        rv = (int)bit_rev((uint32_t)code, len);
        fill = 1 << (HUFF_BITS - len);
        packed = HUFF_PACK(sym, len);
        for (i = 0; i < fill; i++)
            h->fast[rv | (i << len)] = packed;
    }
}

static int huff_decode(Inf* s, const Huff* h) {
    uint16_t entry;
    int len, code, first_code, index, bits, cnt;

    if (s->bit_cnt < 15) inf_fill(s);

    entry = h->fast[s->bit_buf & (HUFF_SIZE - 1)];
    len = HUFF_LEN(entry);

    if (len > 0) {
        s->bit_buf >>= len;
        s->bit_cnt -= len;
        return HUFF_SYM(entry);
    }

    code = 0; first_code = 0; index = 0;
    for (bits = 1; bits <= 15; bits++) {
        code = (code << 1) | (int)((s->bit_buf >> (bits - 1)) & 1);
        cnt = h->counts[bits];
        if (code - first_code < cnt) {
            s->bit_buf >>= bits;
            s->bit_cnt -= bits;
            return h->symbols[index + (code - first_code)];
        }
        index += cnt;
        first_code = (first_code + cnt) << 1;
    }
    return -1;
}

static Huff fixed_lit, fixed_dist;
static int fixed_ready = 0;

static void huff_fixed_init(void) {
    uint8_t lengths[288], dlen[32];
    int i;
    if (fixed_ready) return;
    for (i = 0; i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(&fixed_lit, lengths, 288);
    for (i = 0; i < 32; i++) dlen[i] = 5;
    huff_build(&fixed_dist, dlen, 32);
    fixed_ready = 1;
}

static const uint8_t cl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int inf_decode_block(Inf* s, Huff* lt, Huff* dt) {
    int sym, li, length, dsym, distance, src_off;
    for (;;) {
        sym = huff_decode(s, lt);
        if (sym < 0) return -1;
        if (sym < 256) {
            inf_output(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            li = sym - 257;
            if (li >= 29) return -1;
            length = def_len_base[li] + (int)inf_bits(s, def_len_extra[li]);
            dsym = huff_decode(s, dt);
            if (dsym < 0 || dsym >= 30) return -1;
            distance = def_dist_base[dsym] + (int)inf_bits(s, def_dist_extra[dsym]);
            src_off = s->dst_pos - distance;
            if (src_off < 0) return -1;
            inf_copy(s, src_off, length);
        }
    }
}

int comp_inflate(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    Inf s;
    Huff lt, dt;
    int bfinal, btype, align, len, nlen, hlit, hdist, hclen, pos, total;
    uint8_t cl_lengths[19];
    uint8_t lengths[320];
    int i, rep, sym;
    uint8_t prev;

    inf_init(&s, src, src_len, dst, dst_cap);

    for (;;) {
        bfinal = (int)inf_bits(&s, 1);
        btype = (int)inf_bits(&s, 2);

        if (btype == 0) {
            align = s.bit_cnt & 7;
            s.bit_buf >>= align; s.bit_cnt -= align;
            s.src_pos -= s.bit_cnt / 8;
            s.bit_cnt = 0; s.bit_buf = 0;

            if (s.src_pos + 4 > s.src_len) return -1;
            len = s.src[s.src_pos] | (s.src[s.src_pos + 1] << 8);
            nlen = s.src[s.src_pos + 2] | (s.src[s.src_pos + 3] << 8);
            s.src_pos += 4;
            if ((len ^ nlen) != 0xFFFF) return -1;
            if (s.src_pos + len > s.src_len) return -1;
            for (i = 0; i < len; i++)
                inf_output(&s, s.src[s.src_pos + i]);
            s.src_pos += len;
        } else if (btype == 1) {
            huff_fixed_init();
            if (inf_decode_block(&s, &fixed_lit, &fixed_dist) < 0) return -1;
        } else if (btype == 2) {
            hlit = (int)inf_bits(&s, 5) + 257;
            hdist = (int)inf_bits(&s, 5) + 1;
            hclen = (int)inf_bits(&s, 4) + 4;

            for (i = 0; i < 19; i++) cl_lengths[i] = 0;
            for (i = 0; i < hclen; i++)
                cl_lengths[cl_order[i]] = (uint8_t)inf_bits(&s, 3);

            {
                Huff cl;
                huff_build(&cl, cl_lengths, 19);

                for (pos = 0; pos < (int)sizeof(lengths); pos++) lengths[pos] = 0;
                total = hlit + hdist;
                pos = 0;
                while (pos < total) {
                    sym = huff_decode(&s, &cl);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lengths[pos++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        rep = (int)inf_bits(&s, 2) + 3;
                        if (pos == 0 || pos + rep > total) return -1;
                        prev = lengths[pos - 1];
                        for (i = 0; i < rep; i++) lengths[pos++] = prev;
                    } else if (sym == 17) {
                        rep = (int)inf_bits(&s, 3) + 3;
                        if (pos + rep > total) return -1;
                        for (i = 0; i < rep; i++) lengths[pos++] = 0;
                    } else if (sym == 18) {
                        rep = (int)inf_bits(&s, 7) + 11;
                        if (pos + rep > total) return -1;
                        for (i = 0; i < rep; i++) lengths[pos++] = 0;
                    } else {
                        return -1;
                    }
                }

                huff_build(&lt, lengths, hlit);
                huff_build(&dt, lengths + hlit, hdist);
                if (inf_decode_block(&s, &lt, &dt) < 0) return -1;
            }
        } else {
            return -1;
        }

        if (bfinal) break;
    }

    return s.dst_pos;
}

int comp_gunzip(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    uint8_t flags;
    int pos, xlen, def_len, out;

    if (src_len < 10 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8)
        return -1;

    flags = src[3];
    pos = 10;

    if (flags & 0x04) {
        if (pos + 2 > src_len) return -1;
        xlen = src[pos] | (src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { while (pos < src_len && src[pos] != 0) pos++; pos++; }
    if (flags & 0x10) { while (pos < src_len && src[pos] != 0) pos++; pos++; }
    if (flags & 0x02) pos += 2;

    if (pos >= src_len) return -1;

    def_len = src_len - pos - 8;
    if (def_len <= 0) return -1;

    out = comp_inflate(src + pos, def_len, dst, dst_cap);
    return out;
}

/* ================================================================== */
/*  DEFLATE compressor (RFC 1951)                                      */
/* ================================================================== */

typedef struct Def {
    uint8_t* dst;
    int dst_cap;
    int dst_pos;
    uint32_t bit_buf;
    int bit_cnt;
} Def;

static void def_init(Def* d, uint8_t* dst, int dst_cap) {
    d->dst = dst; d->dst_cap = dst_cap; d->dst_pos = 0;
    d->bit_buf = 0; d->bit_cnt = 0;
}

static void def_emit(Def* d, uint32_t value, int nbits) {
    d->bit_buf |= value << d->bit_cnt;
    d->bit_cnt += nbits;
    while (d->bit_cnt >= 8) {
        if (d->dst_pos < d->dst_cap)
            d->dst[d->dst_pos++] = (uint8_t)(d->bit_buf & 0xFF);
        d->bit_buf >>= 8;
        d->bit_cnt -= 8;
    }
}

static void def_finish(Def* d) {
    if (d->bit_cnt > 0) {
        if (d->dst_pos < d->dst_cap)
            d->dst[d->dst_pos++] = (uint8_t)(d->bit_buf & 0xFF);
        d->bit_buf = 0; d->bit_cnt = 0;
    }
}

static uint16_t enc_lit_code[288];
static uint8_t  enc_lit_len[288];
static uint16_t enc_dist_code[32];
static uint8_t  enc_dist_len[32];
static int enc_tables_ready = 0;

static uint32_t def_bit_reverse(uint32_t v, int n) {
    uint32_t r;
    int i;
    r = 0;
    for (i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void build_canonical(uint16_t* codes, const uint8_t* lengths, int count) {
    uint16_t bl_count[16], next_code[16], code;
    int i, bits;

    for (i = 0; i < 16; i++) bl_count[i] = 0;
    for (i = 0; i < count; i++)
        if (lengths[i] > 0) bl_count[lengths[i]]++;

    code = 0;
    next_code[0] = 0;
    for (bits = 1; bits < 16; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (i = 0; i < count; i++) {
        if (lengths[i] > 0)
            codes[i] = (uint16_t)def_bit_reverse(next_code[lengths[i]]++, lengths[i]);
        else
            codes[i] = 0;
    }
}

static void build_enc_tables(void) {
    int i;
    for (i = 0; i <= 143; i++) enc_lit_len[i] = 8;
    for (i = 144; i <= 255; i++) enc_lit_len[i] = 9;
    for (i = 256; i <= 279; i++) enc_lit_len[i] = 7;
    for (i = 280; i <= 287; i++) enc_lit_len[i] = 8;
    for (i = 0; i < 32; i++) enc_dist_len[i] = 5;

    build_canonical(enc_lit_code, enc_lit_len, 288);
    build_canonical(enc_dist_code, enc_dist_len, 32);
    enc_tables_ready = 1;
}

static void emit_length(Def* d, int length) {
    int i;
    for (i = 28; i >= 0; i--) {
        if (length >= def_len_base[i]) {
            def_emit(d, enc_lit_code[257 + i], enc_lit_len[257 + i]);
            if (def_len_extra[i] > 0)
                def_emit(d, (uint32_t)(length - def_len_base[i]), def_len_extra[i]);
            return;
        }
    }
}

static void emit_distance(Def* d, int distance) {
    int i;
    for (i = 29; i >= 0; i--) {
        if (distance >= def_dist_base[i]) {
            def_emit(d, enc_dist_code[i], enc_dist_len[i]);
            if (def_dist_extra[i] > 0)
                def_emit(d, (uint32_t)(distance - def_dist_base[i]), def_dist_extra[i]);
            return;
        }
    }
}

#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)
#define HASH_MASK  (HASH_SIZE - 1)
#define WINDOW     32768
#define MAX_CHAIN  64
#define MAX_MATCH  258
#define MIN_MATCH  3

static uint32_t hash3(const uint8_t* p) {
    return ((uint32_t)p[0] * 2654435761u ^ (uint32_t)p[1] ^ ((uint32_t)p[2] << 4)) & HASH_MASK;
}

int comp_deflate(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    Def d;
    int *head, *prev;
    int pos, best_len, best_dist, mp, chain, dist, max_len, len, pi, i;
    uint32_t h;

    if (!enc_tables_ready) build_enc_tables();

    def_init(&d, dst, dst_cap);

    def_emit(&d, 1, 1);  /* BFINAL */
    def_emit(&d, 1, 2);  /* BTYPE = fixed Huffman */

    if (src_len == 0) {
        def_emit(&d, enc_lit_code[256], enc_lit_len[256]);
        def_finish(&d);
        return d.dst_pos;
    }

    head = (int*)malloc(HASH_SIZE * sizeof(int));
    prev = (int*)malloc(WINDOW * sizeof(int));
    if (!head || !prev) { free(head); free(prev); return -1; }
    for (i = 0; i < HASH_SIZE; i++) head[i] = -1;
    for (i = 0; i < WINDOW; i++) prev[i] = -1;

    pos = 0;
    while (pos < src_len) {
        best_len = MIN_MATCH - 1; best_dist = 0;

        if (pos + 2 < src_len) {
            h = hash3(&src[pos]);
            mp = head[h]; chain = 0;

            while (mp >= 0 && chain < MAX_CHAIN) {
                dist = pos - mp;
                if (dist > WINDOW || dist <= 0) break;

                max_len = src_len - pos;
                if (max_len > MAX_MATCH) max_len = MAX_MATCH;

                len = 0;
                while (len < max_len && src[mp + len] == src[pos + len]) len++;

                if (len > best_len) {
                    best_len = len; best_dist = dist;
                    if (len >= MAX_MATCH) break;
                }

                pi = mp % WINDOW;
                mp = prev[pi];
                if (mp >= pos) break;
                chain++;
            }

            prev[pos % WINDOW] = head[h];
            head[h] = pos;
        }

        if (best_len >= MIN_MATCH) {
            emit_length(&d, best_len);
            emit_distance(&d, best_dist);
            for (i = 1; i < best_len && pos + i + 2 < src_len; i++) {
                h = hash3(&src[pos + i]);
                prev[(pos + i) % WINDOW] = head[h];
                head[h] = pos + i;
            }
            pos += best_len;
        } else {
            def_emit(&d, enc_lit_code[src[pos]], enc_lit_len[src[pos]]);
            pos++;
        }
    }

    def_emit(&d, enc_lit_code[256], enc_lit_len[256]);
    def_finish(&d);

    free(head); free(prev);
    return d.dst_pos;
}

/* ================================================================== */
/*  ZIP format reader/writer                                           */
/* ================================================================== */

static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
static uint16_t get16(const uint8_t* p, int off) { return (uint16_t)((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8)); }
static uint32_t get32(const uint8_t* p, int off) { return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24); }

static int read_file(const char* path, uint8_t** out_data, int* out_len) {
    FILE* f;
    int len;
    uint8_t* data;
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    len = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(len > 0 ? (size_t)len : 1);
    if (!data) { fclose(f); return -1; }
    if (len > 0 && (int)fread(data, 1, len, f) != len) { free(data); fclose(f); return -1; }
    fclose(f);
    *out_data = data; *out_len = len;
    return 0;
}

void comp_zip_dos_time(time_t t, uint16_t* out_time, uint16_t* out_date) {
    struct tm* tm = localtime(&t);
    *out_time = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    *out_date = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
}

static uint64_t get64(const uint8_t* p, int off) {
    return (uint64_t)get32(p, off) | ((uint64_t)get32(p, off + 4) << 32);
}

/* Parse Zip64 extra field (ID 0x0001) in a central directory entry.
 * buf/len point to the start of the extra field (after name).
 * Modifies e->uncompressed_size, e->compress_size, e->offset if sentinel
 * values (0xFFFFFFFF) are found. */
static void parse_zip64_extra(ZipEntry* e, const uint8_t* buf, int elen) {
    int pos = 0;
    while (pos + 4 <= elen) {
        uint16_t id = get16(buf, pos);
        uint16_t sz = get16(buf, pos + 2);
        pos += 4;
        if (id == 0x0001) {  /* Zip64 extended information */
            if (e->uncompressed_size == (int64_t)0xFFFFFFFF && sz >= 8) {
                e->uncompressed_size = (int64_t)get64(buf, pos);
                pos += 8; sz -= 8;
            }
            if (e->compress_size == (int64_t)0xFFFFFFFF && sz >= 8) {
                e->compress_size = (int64_t)get64(buf, pos);
                pos += 8; sz -= 8;
            }
            if (e->offset == (int64_t)0xFFFFFFFF && sz >= 8) {
                e->offset = (int64_t)get64(buf, pos);
                pos += 8; sz -= 8;
            }
        }
        pos += sz;
    }
}

void comp_zip_init(ZipFile* z) {
    memset(z, 0, sizeof(*z));
    z->entries = (ZipEntry*)malloc(ZIP_MAX_ENTRIES * sizeof(ZipEntry));
    z->entries_cap = z->entries ? ZIP_MAX_ENTRIES : 0;
    z->entry_count = 0;
}

void comp_zip_free(ZipFile* z) {
    if (z->entries) { free(z->entries); z->entries = NULL; }
    if (z->data) { free(z->data); z->data = NULL; }
    if (z->fp) { fclose(z->fp); z->fp = NULL; }
    z->data_len = 0;
    z->entry_count = 0;
    z->entries_cap = 0;
}

/* Resolve Zip64 sentinel values from the standard EOCD by finding and parsing
 * the Zip64 EOCD Locator and Zip64 EOCD Record, which appear immediately
 * before the standard EOCD in Zip64 archives.
 *
 * Layout at end of Zip64 file:
 *   [Zip64 EOCD Record]  (56+ bytes, signature 0x06064B50)
 *   [Zip64 EOCD Locator] (20 bytes,  signature 0x07064B50)
 *   [Standard EOCD]      (22 bytes,  signature 0x06054B50)
 *
 * buf:     buffer containing the EOCD (tail_buf for file-backed, z->data for mem)
 * buf_len: length of buf
 * eocd_pos: offset of standard EOCD within buf
 * cd_entries, cd_offset, cd_size: in/out — replaced with 64-bit values if sentinels
 * fp:      for file-backed mode, seek to Zip64 EOCD; NULL for memory-backed
 */
static void resolve_zip64(const uint8_t* buf, int64_t buf_len, int eocd_pos,
                          int* cd_entries, int64_t* cd_offset, int64_t* cd_size,
                          FILE* fp)
{
    /* Only resolve if at least one field is a Zip64 sentinel */
    if (*cd_entries < 0xFFFF && *cd_offset < 0xFFFFFFFFLL && *cd_size < 0xFFFFFFFFLL)
        return;

    /* Find Zip64 EOCD Locator (20 bytes, immediately before standard EOCD) */
    int loc_pos = eocd_pos - ZIP_Z64_LOC_SIZE;
    if (loc_pos < 0 || get32(buf, loc_pos) != SIG_Z64_LOC)
        return;  /* No Zip64 locator found — use 32-bit values as-is */

    /* Locator fields: disk = get32(buf, loc_pos+4), offset = get64(buf, loc_pos+8), total = get32(buf, loc_pos+16) */
    int64_t z64_eocd_offset = (int64_t)get64(buf, loc_pos + 8);

    /* Two paths: the Zip64 EOCD record may be in our buffer, or we may need to seek */
    const uint8_t* z64_buf = NULL;
    uint8_t* z64_alloc = NULL;

    if (fp) {
        /* File-backed: seek to the Zip64 EOCD record */
        int64_t z64_size = ZIP_Z64_EOCD_SIZE;
        z64_alloc = (uint8_t*)malloc((size_t)z64_size);
        if (!z64_alloc) return;
        COMP_FSEEK(fp, z64_eocd_offset, SEEK_SET);
        if ((int64_t)fread(z64_alloc, 1, (size_t)z64_size, fp) != z64_size) {
            free(z64_alloc);
            return;
        }
        z64_buf = z64_alloc;
    } else {
        /* Memory-backed: record is within z->data (offset from start of buf) */
        if (z64_eocd_offset < buf_len - ZIP_Z64_EOCD_SIZE) {
            z64_buf = buf + z64_eocd_offset;
        } else {
            return;  /* Offset out of bounds */
        }
    }

    /* Verify Zip64 EOCD signature */
    if (get32(z64_buf, 0) != SIG_Z64_EOCD) {
        free(z64_alloc);
        return;
    }

    /* Zip64 EOCD record layout (per APPNOTE 4.3.6 and 4.5.3):
     *   +0:   signature         (4 bytes) 0x06064B50
     *   +4:   record_size       (8 bytes) size of remaining record
     *   +12:  version_made      (2 bytes)
     *   +14:  version_needed    (2 bytes)
     *   +16:  disk_num          (4 bytes)
     *   +20:  disk_start        (4 bytes)
     *   +24:  cd_entries_disk   (8 bytes) entries on this disk
     *   +32:  cd_entries_total  (8 bytes) total entries
     *   +40:  cd_size           (8 bytes) central directory size
     *   +48:  cd_offset         (8 bytes) central directory offset
     */
    if (*cd_entries >= 0xFFFF)
        *cd_entries = (int)get64(z64_buf, 32);
    if (*cd_size >= 0xFFFFFFFFLL)
        *cd_size = (int64_t)get64(z64_buf, 40);
    if (*cd_offset >= 0xFFFFFFFFLL)
        *cd_offset = (int64_t)get64(z64_buf, 48);

    free(z64_alloc);
}

/* Parse central directory entries from a memory buffer (buf, buf_len).
 * Shared by both file-backed and memory-backed open paths. */
static int zip_parse_cd_buf(ZipFile* z, const uint8_t* buf, int64_t buf_len,
                            int64_t cd_offset, int cd_entries)
{
    int pos, i, name_len, extra_len, comment_len, name_copy;
    ZipEntry* e;

    if (cd_offset < 0 || cd_offset >= buf_len) return -1;

    /* Grow entries array if needed */
    if (cd_entries > z->entries_cap) {
        ZipEntry* ne = (ZipEntry*)realloc(z->entries, (size_t)cd_entries * sizeof(ZipEntry));
        if (!ne) return -1;
        z->entries = ne;
        z->entries_cap = cd_entries;
    }

    z->entry_count = 0;
    pos = (int)cd_offset;
    for (i = 0; i < cd_entries && z->entry_count < z->entries_cap; i++) {
        if (pos + ZIP_CD_SIZE > (int)buf_len) break;
        if (get32(buf, pos) != SIG_CENTRAL) break;

        name_len    = get16(buf, pos + 28);
        extra_len   = get16(buf, pos + 30);
        comment_len = get16(buf, pos + 32);
        if (pos + ZIP_CD_SIZE + name_len + extra_len + comment_len > (int)buf_len) break;

        e = &z->entries[z->entry_count];
        e->compress_type      = get16(buf, pos + 10);
        e->mod_time           = get16(buf, pos + 12);
        e->mod_date           = get16(buf, pos + 14);
        e->crc                = get32(buf, pos + 16);
        e->compress_size      = (int64_t)get32(buf, pos + 20);
        e->uncompressed_size  = (int64_t)get32(buf, pos + 24);
        e->offset             = (int64_t)get32(buf, pos + 42);

        /* Parse Zip64 extra fields if present */
        if (extra_len > 0) {
            parse_zip64_extra(e, &buf[pos + ZIP_CD_SIZE + name_len], extra_len);
        }

        if (e->compress_type != 0 && e->compress_type != 8) {
            pos += ZIP_CD_SIZE + name_len + extra_len + comment_len;
            continue;
        }

        name_copy = name_len < ZIP_NAME_LEN ? name_len : ZIP_NAME_LEN;
        memcpy(e->name, &buf[pos + ZIP_CD_SIZE], name_copy);
        e->name[name_copy] = '\0';
        pos += ZIP_CD_SIZE + name_len + extra_len + comment_len;
        z->entry_count++;
    }
    return 0;
}

/* Memory-backed: parse EOCD and central directory from z->data */
static int zip_parse_cd(ZipFile* z)
{
    int eocd, cd_entries;
    int64_t cd_offset, cd_size;

    eocd = -1;
    for (int i = (int)z->data_len - ZIP_EOCD_SIZE; i >= 0; i--)
        if (get32(z->data, i) == SIG_EOCD) { eocd = i; break; }
    if (eocd < 0 || eocd + ZIP_EOCD_SIZE > z->data_len) return -1;

    cd_entries = get16(z->data, eocd + 10);
    cd_size    = (int64_t)get32(z->data, eocd + 12);
    cd_offset  = (int64_t)get32(z->data, eocd + 16);

    /* Resolve Zip64 sentinel values (NULL fp = memory-backed mode) */
    resolve_zip64(z->data, z->data_len, eocd, &cd_entries, &cd_offset, &cd_size, NULL);

    return zip_parse_cd_buf(z, z->data, z->data_len, cd_offset, cd_entries);
}

int comp_zip_open(ZipFile* z, const char* path)
{
    FILE* fp;
    int64_t file_size, tail_offset, cd_offset;
    int64_t tail_size;
    uint8_t* tail_buf = NULL;
    uint8_t* cd_buf = NULL;
    int eocd_pos, cd_entries, search_start;
    int64_t cd_size;

    comp_zip_init(z);

    fp = fopen(path, "rb");
    if (!fp) { comp_zip_free(z); return -1; }

    /* Get file size using 64-bit seek */
    COMP_FSEEK(fp, 0, SEEK_END);
    file_size = COMP_FTELL(fp);
    if (file_size < ZIP_EOCD_SIZE) { fclose(fp); comp_zip_free(z); return -1; }

    /* Read last min(128KB, filesize) bytes to find EOCD */
    #define TAIL_MAX (128 * 1024)
    tail_size = file_size < TAIL_MAX ? file_size : TAIL_MAX;
    tail_offset = file_size - tail_size;
    tail_buf = (uint8_t*)malloc((size_t)tail_size);
    if (!tail_buf) { fclose(fp); comp_zip_free(z); return -1; }

    COMP_FSEEK(fp, tail_offset, SEEK_SET);
    if ((int64_t)fread(tail_buf, 1, (size_t)tail_size, fp) != tail_size) {
        free(tail_buf); fclose(fp); comp_zip_free(z); return -1;
    }

    /* Find EOCD signature in tail buffer (scan backwards) */
    eocd_pos = -1;
    search_start = (int)(tail_size - ZIP_EOCD_SIZE);
    for (int i = search_start; i >= 0; i--) {
        if (get32(tail_buf, i) == SIG_EOCD) { eocd_pos = i; break; }
    }
    if (eocd_pos < 0) { free(tail_buf); fclose(fp); comp_zip_free(z); return -1; }

    /* Parse EOCD fields */
    cd_entries = get16(tail_buf, eocd_pos + 10);
    cd_offset  = (int64_t)get32(tail_buf, eocd_pos + 16);
    cd_size    = (int64_t)get32(tail_buf, eocd_pos + 12);

    /* Resolve Zip64 sentinel values (fp provided for seek if needed) */
    resolve_zip64(tail_buf, tail_size, eocd_pos, &cd_entries, &cd_offset, &cd_size, fp);

    /* Parse central directory: either from tail_buf or via separate read */
    if (cd_offset >= tail_offset) {
        /* CD is within the tail buffer we already read */
        int64_t cd_buf_offset = cd_offset - tail_offset;
        if (zip_parse_cd_buf(z, tail_buf, tail_size, cd_buf_offset, cd_entries) < 0) {
            free(tail_buf); fclose(fp); comp_zip_free(z); return -1;
        }
    } else {
        /* CD is before our tail buffer — seek and read it */
        cd_buf = (uint8_t*)malloc((size_t)cd_size);
        if (!cd_buf) { free(tail_buf); fclose(fp); comp_zip_free(z); return -1; }
        COMP_FSEEK(fp, cd_offset, SEEK_SET);
        if ((int64_t)fread(cd_buf, 1, (size_t)cd_size, fp) != cd_size) {
            free(cd_buf); free(tail_buf); fclose(fp); comp_zip_free(z); return -1;
        }
        if (zip_parse_cd_buf(z, cd_buf, cd_size, 0, cd_entries) < 0) {
            free(cd_buf); free(tail_buf); fclose(fp); comp_zip_free(z); return -1;
        }
        free(cd_buf);
    }

    free(tail_buf);
    z->fp = fp;  /* keep file open for streaming entry extraction */
    return 0;
}

int comp_zip_open_mem(ZipFile* z, const uint8_t* data, int data_len)
{
    uint8_t* copy;
    if (!data || data_len <= 0) return -1;

    comp_zip_init(z);

    copy = (uint8_t*)malloc((size_t)data_len);
    if (!copy) { comp_zip_free(z); return -1; }
    memcpy(copy, data, (size_t)data_len);
    z->data = copy;
    z->data_len = (int64_t)data_len;
    /* fp remains NULL — memory-backed mode */

    if (zip_parse_cd(z) < 0) { comp_zip_free(z); return -1; }
    return 0;
}

void comp_zip_close(ZipFile* z) {
    comp_zip_free(z);
}

int comp_zip_read_entry(ZipFile* z, int idx, uint8_t* buf, int buf_cap) {
    ZipEntry* e;
    int sz;

    if (idx < 0 || idx >= z->entry_count) return -1;
    e = &z->entries[idx];

    if (z->fp) {
        /* ---- File-backed (streaming) mode ---- */
        int64_t data_start;
        int name_len, extra_len;
        uint8_t lfh[ZIP_LFH_SIZE];

        /* Seek to Local File Header */
        if (COMP_FSEEK(z->fp, e->offset, SEEK_SET) != 0) return -1;
        if (fread(lfh, 1, ZIP_LFH_SIZE, z->fp) != ZIP_LFH_SIZE) return -1;

        /* Verify LFH signature */
        if (get32(lfh, 0) != SIG_LOCAL) return -1;

        name_len  = get16(lfh, 26);
        extra_len = get16(lfh, 28);
        data_start = e->offset + ZIP_LFH_SIZE + name_len + extra_len;

        /* Seek to compressed data */
        if (COMP_FSEEK(z->fp, data_start, SEEK_SET) != 0) return -1;

        if (e->compress_type == 0) {
            /* Stored: read directly into caller's buffer */
            int64_t to_read = e->compress_size < buf_cap ? e->compress_size : buf_cap;
            if ((int64_t)fread(buf, 1, (size_t)to_read, z->fp) != to_read) return -1;
            sz = (int)to_read;
        } else if (e->compress_type == 8) {
            /* Deflated: read compressed data into temp buffer, then inflate */
            uint8_t* cbuf;
            int64_t csz64 = e->compress_size;
            int csz;
            if (csz64 <= 0 || csz64 > 256 * 1024 * 1024) return -1;  /* safety cap: 256MB */
            csz = (int)csz64;
            cbuf = (uint8_t*)malloc((size_t)csz);
            if (!cbuf) return -1;
            if ((int)fread(cbuf, 1, (size_t)csz, z->fp) != csz) {
                free(cbuf);
                return -1;
            }
            sz = comp_inflate(cbuf, csz, buf, buf_cap);
            free(cbuf);
            if (sz < 0) return -1;
        } else {
            return -1;
        }
    } else {
        /* ---- Memory-backed (existing) mode ---- */
        int64_t lh, data_start;

        lh = e->offset;
        if (lh < 0 || lh + ZIP_LFH_SIZE > z->data_len) return -1;

        {
            int name_len  = get16(z->data, (int)lh + 26);
            int extra_len = get16(z->data, (int)lh + 28);
            data_start = lh + ZIP_LFH_SIZE + name_len + extra_len;
        }

        if (data_start > z->data_len || data_start + e->compress_size > z->data_len) return -1;

        if (e->compress_type == 0) {
            int64_t to_copy = e->compress_size < buf_cap ? e->compress_size : buf_cap;
            sz = (int)to_copy;
            if (sz > 0) memcpy(buf, &z->data[data_start], sz);
        } else if (e->compress_type == 8) {
            int csz = (int)e->compress_size;
            if (csz < 0) return -1;
            sz = comp_inflate(&z->data[data_start], csz, buf, buf_cap);
            if (sz < 0) return -1;
        } else {
            return -1;
        }
    }

    if (sz >= e->uncompressed_size && comp_crc32(buf, sz) != e->crc) return -1;
    return sz;
}

int comp_zip_find(ZipFile* z, const char* suffix) {
    int slen, i, nlen;
    slen = (int)strlen(suffix);
    for (i = 0; i < z->entry_count; i++) {
        nlen = (int)strlen(z->entries[i].name);
        if (nlen >= slen && strcmp(z->entries[i].name + nlen - slen, suffix) == 0)
            return i;
    }
    return -1;
}

static int write_all(FILE* f, const void* buf, int len) {
    return (int)fwrite(buf, 1, len, f) == len ? 0 : -1;
}

ZipWriter* comp_zip_writer_open(void) {
    return (ZipWriter*)calloc(1, sizeof(ZipWriter));
}

int comp_zip_writer_add(ZipWriter* w, const char* name, const char* path) {
    struct stat st;
    int have_stat, len, rc;
    uint8_t* data;
    uint16_t mtime, mdate;

    if (w->entry_count >= ZIP_MAX_ENTRIES) return -1;

    have_stat = (stat(path, &st) == 0);
    if (read_file(path, &data, &len) != 0) return -1;

    if (have_stat)
        comp_zip_dos_time(st.st_mtime, &mtime, &mdate);
    else
        comp_zip_dos_time(time(NULL), &mtime, &mdate);

    rc = comp_zip_writer_add_data_ex(w, name, data, len, mtime, mdate);
    free(data);
    return rc;
}

int comp_zip_writer_add_data(ZipWriter* w, const char* name, const uint8_t* data, int len) {
    uint16_t mtime, mdate;
    comp_zip_dos_time(time(NULL), &mtime, &mdate);
    return comp_zip_writer_add_data_ex(w, name, data, len, mtime, mdate);
}

int comp_zip_writer_add_data_ex(ZipWriter* w, const char* name, const uint8_t* data, int len,
                                 uint16_t mod_time, uint16_t mod_date) {
    struct ZipWriterEntry* e;
    uint8_t* copy;
    int name_len;

    if (w->entry_count >= ZIP_MAX_ENTRIES) return -1;

    copy = NULL;
    if (len > 0) { copy = (uint8_t*)malloc(len); if (!copy) return -1; memcpy(copy, data, len); }

    e = &w->entries[w->entry_count];
    name_len = (int)strlen(name); if (name_len > ZIP_NAME_LEN) name_len = ZIP_NAME_LEN;
    memcpy(e->name, name, name_len); e->name[name_len] = '\0';
    e->data = copy; e->data_len = len;
    e->crc = comp_crc32(data, len);
    e->mod_time = mod_time; e->mod_date = mod_date;
    w->entry_count++;
    return 0;
}

int comp_zip_writer_close(ZipWriter* w, const char* path) {
    int *comp_sizes, *comp_types, *offsets;
    uint8_t** comp_data;
    int i, name_len, cd_start, cd_size, cap, sz;
    struct ZipWriterEntry* e;
    uint8_t hdr[ZIP_LFH_SIZE], cd[ZIP_CD_SIZE], eocd[ZIP_EOCD_SIZE];
    uint8_t* buf;
    FILE* f;

    if (!crc_table_init) crc_init();
    f = fopen(path, "wb");
    if (!f) return -1;

    comp_sizes = (int*)malloc(w->entry_count * sizeof(int));
    comp_types = (int*)malloc(w->entry_count * sizeof(int));
    comp_data = (uint8_t**)malloc(w->entry_count * sizeof(uint8_t*));
    if (!comp_sizes || !comp_types || !comp_data) {
        free(comp_sizes); free(comp_types); free(comp_data);
        fclose(f); remove(path); return -1;
    }

    for (i = 0; i < w->entry_count; i++) {
        e = &w->entries[i];
        if (e->data_len == 0) { comp_types[i] = 0; comp_sizes[i] = 0; comp_data[i] = NULL; continue; }

        cap = e->data_len + (e->data_len >> 6) + 64;
        buf = (uint8_t*)malloc(cap);
        if (!buf) { comp_types[i] = 0; comp_sizes[i] = e->data_len; comp_data[i] = NULL; continue; }

        sz = comp_deflate(e->data, e->data_len, buf, cap);
        if (sz > 0 && sz < e->data_len) { comp_types[i] = 8; comp_sizes[i] = sz; comp_data[i] = buf; }
        else { free(buf); comp_types[i] = 0; comp_sizes[i] = e->data_len; comp_data[i] = NULL; }
    }

    offsets = (int*)malloc(w->entry_count * sizeof(int));
    if (!offsets) {
        for (i = 0; i < w->entry_count; i++) free(comp_data[i]);
        free(comp_sizes); free(comp_types); free(comp_data);
        fclose(f); remove(path); return -1;
    }

    for (i = 0; i < w->entry_count; i++) {
        e = &w->entries[i];
        name_len = (int)strlen(e->name);
        offsets[i] = (int)ftell(f);

        put32(hdr + 0, SIG_LOCAL); put16(hdr + 4, 20); put16(hdr + 6, 0);
        put16(hdr + 8, (uint16_t)comp_types[i]);
        put16(hdr + 10, e->mod_time); put16(hdr + 12, e->mod_date);
        put32(hdr + 14, e->crc); put32(hdr + 18, (uint32_t)comp_sizes[i]);
        put32(hdr + 22, (uint32_t)e->data_len); put16(hdr + 26, (uint16_t)name_len); put16(hdr + 28, 0);
        if (write_all(f, hdr, ZIP_LFH_SIZE) || write_all(f, e->name, name_len)) goto fail;

        if (comp_types[i] == 8 && comp_data[i]) { if (write_all(f, comp_data[i], comp_sizes[i])) goto fail; }
        else if (e->data_len > 0) { if (write_all(f, e->data, e->data_len)) goto fail; }
    }

    cd_start = (int)ftell(f);
    for (i = 0; i < w->entry_count; i++) {
        e = &w->entries[i];
        name_len = (int)strlen(e->name);
        put32(cd + 0, SIG_CENTRAL); put16(cd + 4, 20); put16(cd + 6, 20); put16(cd + 8, 0);
        put16(cd + 10, (uint16_t)comp_types[i]); put16(cd + 12, e->mod_time); put16(cd + 14, e->mod_date);
        put32(cd + 16, e->crc); put32(cd + 20, (uint32_t)comp_sizes[i]);
        put32(cd + 24, (uint32_t)e->data_len); put16(cd + 28, (uint16_t)name_len);
        put16(cd + 30, 0); put16(cd + 32, 0); put16(cd + 34, 0); put16(cd + 36, 0);
        put32(cd + 38, 0); put32(cd + 42, (uint32_t)offsets[i]);
        if (write_all(f, cd, ZIP_CD_SIZE) || write_all(f, e->name, name_len)) goto fail;
    }
    cd_size = (int)ftell(f) - cd_start;

    put32(eocd + 0, SIG_EOCD); put16(eocd + 4, 0); put16(eocd + 6, 0);
    put16(eocd + 8, (uint16_t)w->entry_count); put16(eocd + 10, (uint16_t)w->entry_count);
    put32(eocd + 12, (uint32_t)cd_size); put32(eocd + 16, (uint32_t)cd_start); put16(eocd + 20, 0);
    if (write_all(f, eocd, ZIP_EOCD_SIZE)) goto fail;

    fclose(f);
    for (i = 0; i < w->entry_count; i++) { free(comp_data[i]); free(w->entries[i].data); }
    free(comp_sizes); free(comp_types); free(comp_data); free(offsets); free(w);
    return 0;

fail:
    for (i = 0; i < w->entry_count; i++) free(comp_data[i]);
    free(comp_sizes); free(comp_types); free(comp_data); free(offsets);
    fclose(f); remove(path); return -1;
}

/* ================================================================== */
/*  Zlib (RFC 1950) wrapper                                            */
/* ================================================================== */

static uint32_t adler32(const uint8_t* data, int len) {
    uint32_t a, b;
    int i;
    a = 1; b = 0;
    for (i = 0; i < len; i++) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

int comp_zlib_decompress(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    uint8_t cmf, flg;
    int def_len, out;
    uint32_t expected;

    if (src_len < 6) return -1;
    cmf = src[0]; flg = src[1];
    if ((cmf & 0x0F) != 8) return -1;
    if (((cmf * 256 + flg) % 31) != 0) return -1;

    def_len = src_len - 6;
    if (def_len <= 0) return -1;

    out = comp_inflate(src + 2, def_len, dst, dst_cap);
    if (out < 0) return -1;

    expected = ((uint32_t)src[src_len - 4] << 24) | ((uint32_t)src[src_len - 3] << 16) |
                ((uint32_t)src[src_len - 2] << 8) | (uint32_t)src[src_len - 1];
    if (adler32(dst, out) != expected) return -1;
    return out;
}

int comp_zlib_compress(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    int def_cap, def_len;
    uint32_t checksum;

    if (dst_cap < src_len + 6) return -1;

    dst[0] = 0x78; dst[1] = 0x9C;
    def_cap = dst_cap - 6;
    if (def_cap < 0) return -1;

    def_len = comp_deflate(src, src_len, dst + 2, def_cap);
    if (def_len < 0) return -1;

    checksum = adler32(src, src_len);
    dst[2 + def_len]     = (uint8_t)(checksum >> 24);
    dst[2 + def_len + 1] = (uint8_t)(checksum >> 16);
    dst[2 + def_len + 2] = (uint8_t)(checksum >> 8);
    dst[2 + def_len + 3] = (uint8_t)(checksum);
    return 2 + def_len + 4;
}

/* Compress into gzip format (RFC 1952). Output is readable by comp_gunzip:
   10-byte header (magic 1F 8B, method 8=deflate, flags 0 = no FEXTRA/FNAME/
   FCOMMENT/FHCRC, mtime 0, xfl 0, os 0xFF) + DEFLATE body + 8-byte trailer
   (CRC32 of uncompressed data, then ISIZE = length mod 2^32, both little-endian).
   comp_deflate here uses fixed Huffman only (no stored-block fallback) and
   silently truncates past dst_cap, so the cap must cover the 9-bits/literal
   worst case: require at least src_len + src_len/8 + 64. */
int comp_gzip(const uint8_t* src, int src_len, uint8_t* dst, int dst_cap) {
    int def_cap, def_len;
    uint32_t crc, isize;

    if (dst_cap < src_len + (src_len >> 3) + 64) return -1;

    dst[0] = 0x1F; dst[1] = 0x8B; dst[2] = 0x08;   /* magic, method=deflate */
    dst[3] = 0x00;                                 /* flags: no extras */
    dst[4] = 0x00; dst[5] = 0x00; dst[6] = 0x00; dst[7] = 0x00;  /* mtime=0 */
    dst[8] = 0x00;                                 /* xfl */
    dst[9] = 0xFF;                                 /* os=unknown */

    def_cap = dst_cap - 18;                        /* 10 hdr + 8 trailer */
    def_len = comp_deflate(src, src_len, dst + 10, def_cap);
    if (def_len < 0) return -1;

    crc   = comp_crc32(src, src_len);
    isize = (uint32_t)src_len;
    dst[10 + def_len]     = (uint8_t)(crc);
    dst[10 + def_len + 1] = (uint8_t)(crc >> 8);
    dst[10 + def_len + 2] = (uint8_t)(crc >> 16);
    dst[10 + def_len + 3] = (uint8_t)(crc >> 24);
    dst[10 + def_len + 4] = (uint8_t)(isize);
    dst[10 + def_len + 5] = (uint8_t)(isize >> 8);
    dst[10 + def_len + 6] = (uint8_t)(isize >> 16);
    dst[10 + def_len + 7] = (uint8_t)(isize >> 24);
    return 10 + def_len + 8;
}

/* ================================================================== */
/*  LZ77 compression / decompression                                   */
/* ================================================================== */

static int lz_read_varsize(const unsigned char *buf, int *value) {
    unsigned int v, b;
    int n;
    v = 0; n = 0;
    do { b = buf[n]; v = (v << 7) | (b & 0x7F); n++; } while (b & 0x80);
    *value = (int)v;
    return n;
}

int comp_lz_uncompress(const unsigned char *in, int insize,
                       unsigned char *out, int out_hint) {
    unsigned char marker, symbol;
    int inpos, outpos, length, offset, i;
    (void)out_hint;

    if (insize < 1) return 0;
    marker = in[0];
    inpos = 1; outpos = 0;

    while (inpos < insize) {
        symbol = in[inpos++];
        if (symbol == marker) {
            if (inpos >= insize) break;
            if (in[inpos] == 0) { out[outpos++] = marker; inpos++; }
            else {
                if (inpos >= insize) break;
                inpos += lz_read_varsize(in + inpos, &length);
                if (inpos >= insize) break;
                inpos += lz_read_varsize(in + inpos, &offset);
                for (i = 0; i < length; i++) {
                    if (outpos - offset < 0) return 0;
                    out[outpos] = out[outpos - offset]; outpos++;
                }
            }
        } else {
            out[outpos++] = symbol;
        }
    }
    return outpos;
}

static int lz_write_varsize(unsigned char *buf, int value) {
    int n;
    unsigned int v;
    n = 0; v = (unsigned int)value;
    if (v < 0x80) { buf[n++] = (unsigned char)v; }
    else if (v < 0x3FFF) { buf[n++] = (unsigned char)((v >> 7) | 0x80); buf[n++] = (unsigned char)(v & 0x7F); }
    else if (v < 0x1FFFFF) { buf[n++] = (unsigned char)((v >> 14) | 0x80); buf[n++] = (unsigned char)((v >> 7) | 0x80); buf[n++] = (unsigned char)(v & 0x7F); }
    else { buf[n++] = (unsigned char)((v >> 21) | 0x80); buf[n++] = (unsigned char)((v >> 14) | 0x80); buf[n++] = (unsigned char)((v >> 7) | 0x80); buf[n++] = (unsigned char)(v & 0x7F); }
    return n;
}

static unsigned char lz_find_marker(const unsigned char *in, int insize) {
    int counts[256];
    int i, min_count;
    unsigned char marker;

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < insize; i++) counts[in[i]]++;
    marker = 0; min_count = insize + 1;
    for (i = 0; i < 256; i++) { if (counts[i] < min_count) { min_count = counts[i]; marker = (unsigned char)i; if (min_count == 0) break; } }
    return marker;
}

int comp_lz_compress(const unsigned char *in, int insize,
                     unsigned char *out, int outsize) {
    unsigned char marker;
    int inpos, outpos, best_len, best_off, len, off, window_start, search_off, max_len_c;
    int varbuf[8], vlen, i;

    if (insize == 0 || outsize < insize + 256) return 0;

    marker = lz_find_marker(in, insize);
    out[0] = marker;
    inpos = 0; outpos = 1;

    while (inpos < insize) {
        best_len = 0; best_off = 0;
        window_start = inpos - 65535; if (window_start < 0) window_start = 0;

        for (off = 1; off <= inpos - window_start; off++) {
            search_off = inpos - off;
            len = 0;
            max_len_c = insize - inpos;
            if (max_len_c > 1024) max_len_c = 1024;
            while (len < max_len_c && in[search_off + len] == in[inpos + len]) len++;
            if (len >= 3 && len > best_len) { best_len = len; best_off = off; if (best_len >= 256) break; }
        }

        if (best_len >= 3) {
            if (outpos + 1 + 8 + 8 >= outsize) return 0;
            out[outpos++] = marker;
            vlen = lz_write_varsize((unsigned char *)varbuf, best_len);
            for (i = 0; i < vlen; i++) out[outpos++] = (unsigned char)varbuf[i];
            vlen = lz_write_varsize((unsigned char *)varbuf, best_off);
            for (i = 0; i < vlen; i++) out[outpos++] = (unsigned char)varbuf[i];
            inpos += best_len;
        } else {
            if (outpos + 2 >= outsize) return 0;
            if (in[inpos] == marker) { out[outpos++] = marker; out[outpos++] = 0x00; }
            else out[outpos++] = in[inpos];
            inpos++;
        }
    }
    return outpos;
}