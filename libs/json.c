/**
 * json.c - Minimal JSON library
 *
 * Streaming parser + string-based helpers.
 * Build:  compile and link with any C/C++ project
 */

#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/*  Streaming parser                                                    */
/* ================================================================== */

void jp_init(JsonParser* p, const uint8_t* data, int len) {
    p->data = data; p->pos = 0; p->len = len;
}

static inline void jp_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        uint8_t c = p->data[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
        else break;
    }
}

uint8_t jp_peek(JsonParser* p) {
    jp_skip_ws(p);
    return (p->pos < p->len) ? p->data[p->pos] : 0;
}

uint8_t jp_next(JsonParser* p) {
    jp_skip_ws(p);
    return (p->pos < p->len) ? p->data[p->pos++] : 0;
}

int64_t jp_int(JsonParser* p) {
    jp_skip_ws(p);
    int64_t v = 0;
    int neg = 0;
    if (p->pos < p->len && p->data[p->pos] == '-') { neg = 1; p->pos++; }
    while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9')
        v = v * 10 + (p->data[p->pos++] - '0');
    return neg ? -v : v;
}

unsigned int jp_uint(JsonParser* p) {
    jp_skip_ws(p);
    unsigned int v = 0;
    while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9')
        v = v * 10 + (p->data[p->pos++] - '0');
    return v;
}

int jp_string(JsonParser* p, char* buf, int buf_cap) {
    jp_skip_ws(p);
    if (p->pos >= p->len || p->data[p->pos] != '"') return -1;
    p->pos++;
    int i = 0;
    while (p->pos < p->len && p->data[p->pos] != '"') {
        if (p->data[p->pos] == '\\' && p->pos + 1 < p->len) p->pos++;
        if (i < buf_cap - 1) buf[i++] = (char)p->data[p->pos];
        p->pos++;
    }
    if (p->pos < p->len) p->pos++;
    buf[i] = '\0';
    return i;
}

int jp_key_eq(JsonParser* p, const char* expected) {
    int saved = p->pos;
    jp_skip_ws(p);
    if (p->pos >= p->len || p->data[p->pos] != '"') { p->pos = saved; return 0; }
    p->pos++;
    const char* e = expected;
    while (*e && p->pos < p->len) {
        uint8_t c = p->data[p->pos];
        if (c == '\\' && p->pos + 1 < p->len) { p->pos++; c = p->data[p->pos]; }
        if (c != (uint8_t)*e) { p->pos = saved; return 0; }
        p->pos++; e++;
    }
    if (*e) { p->pos = saved; return 0; }
    if (p->pos >= p->len || p->data[p->pos] != '"') { p->pos = saved; return 0; }
    p->pos++;
    jp_skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == ':') p->pos++;
    jp_skip_ws(p);
    return 1;
}

int jp_expect(JsonParser* p, char c) {
    jp_skip_ws(p);
    if (p->pos < p->len && p->data[p->pos] == c) { p->pos++; return 0; }
    return -1;
}

/* ================================================================== */
/*  String-based helpers                                                */
/* ================================================================== */

int json_find_str(const char *json, const char *key,
                  const char **out, int *len) {
    char pattern[128];
    int plen = sprintf(pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += plen;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    *out = p;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    *len = (int)(p - start);
    return 1;
}

int json_find_int(const char *json, const char *key) {
    char pattern[128];
    int plen = sprintf(pattern, "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += plen;
    while (*p == ' ' || *p == ':') p++;
    /* Skip optional quote for string-encoded numbers */
    if (*p == '"') p++;
    return atoi(p);
}

char *json_str_dup(const char *out, int len) {
    if (!out || len <= 0) return NULL;
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, out, len);
    s[len] = '\0';
    return s;
}

int json_escape(const char *src, char *dst) {
    char *d = dst;
    while (*src) {
        switch (*src) {
            case '"':  *d++ = '\\'; *d++ = '"';  break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\b': *d++ = '\\'; *d++ = 'b';  break;
            case '\f': *d++ = '\\'; *d++ = 'f';  break;
            case '\n': *d++ = '\\'; *d++ = 'n';  break;
            case '\r': *d++ = '\\'; *d++ = 'r';  break;
            case '\t': *d++ = '\\'; *d++ = 't';  break;
            default:
                if ((unsigned char)*src < 0x20) {
                    d += sprintf(d, "\\u%04x", (unsigned char)*src);
                } else {
                    *d++ = *src;
                }
        }
        src++;
    }
    *d = '\0';
    return (int)(d - dst);
}

/* Decode JSON escape sequences in-place: \u003c -> <, \\n -> newline, etc.
   Handles: \uXXXX (with UTF-8 encoding), \\, \", \n, \r, \t */
void json_decode_inplace(char *s) {
    char *dst = s;
    while (*s) {
        if (s[0] == '\\' && s[1] == 'u' && s[2] != '\0' && s[3] != '\0'
            && s[4] != '\0' && s[5] != '\0') {
            unsigned int val;
            if (json_parse_hex4(s + 1, &val)) {
                dst += json_decode_u16(val, dst);
                s += 6;
            } else {
                *dst++ = *s++;
            }
        } else if (s[0] == '\\' && s[1] == 'n') { *dst++ = '\n'; s += 2; }
        else if (s[0] == '\\' && s[1] == 'r') { *dst++ = '\r'; s += 2; }
        else if (s[0] == '\\' && s[1] == 't') { *dst++ = '\t'; s += 2; }
        else if (s[0] == '\\' && s[1] == '\\') { *dst++ = '\\'; s += 2; }
        else if (s[0] == '\\' && s[1] == '"') { *dst++ = '"'; s += 2; }
        else if (s[0] == '\\' && s[1] == 'b') { *dst++ = '\b'; s += 2; }
        else if (s[0] == '\\' && s[1] == 'f') { *dst++ = '\f'; s += 2; }
        else if (s[0] == '\\' && s[1] == '/') { *dst++ = '/'; s += 2; }
        else { *dst++ = *s++; }
    }
    *dst = '\0';
}

/* Write JSON-escaped string to a FILE stream. Returns bytes written. */
int json_fescape(FILE *fp, const char *s) {
    int len = (int)strlen(s);
    char *buf = (char *)malloc(len * 6 + 1);  /* worst case: every char -> \uXXXX */
    if (!buf) return 0;
    int n = json_escape(s, buf);
    fwrite(buf, 1, n, fp);
    free(buf);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Nested key finder: "section.field"                                */
/* ------------------------------------------------------------------ */

int json_find_nested(const char *json, const char *dotted_key,
                     const char **out, int *len) {
    if (!json || !dotted_key) return 0;

    /* No dot -> delegate to flat search */
    const char *dot = strchr(dotted_key, '.');
    if (!dot) return json_find_str(json, dotted_key, out, len);

    /* Extract section name */
    int sec_len = (int)(dot - dotted_key);
    const char *field = dot + 1;
    if (sec_len <= 0 || !field[0]) return 0;

    /* Find "section" key in JSON */
    char sec_key[256];
    if (sec_len + 4 > (int)sizeof(sec_key)) return 0;
    sec_key[0] = '"'; memcpy(sec_key + 1, dotted_key, sec_len);
    sec_key[sec_len + 1] = '"'; sec_key[sec_len + 2] = '\0';

    const char *sp = strstr(json, sec_key);
    if (!sp) return 0;
    sp += sec_len + 2; /* skip past closing quote */
    while (*sp == ' ' || *sp == '\t' || *sp == ':') sp++;
    if (*sp != '{') return 0;

    /* Find matching } by counting braces */
    const char *obj_start = sp;
    int depth = 0;
    const char *p = sp;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
        else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
        p++;
    }
    int obj_len = (int)(p - obj_start);
    char *obj = (char*)malloc(obj_len + 1);
    if (!obj) return 0;
    memcpy(obj, obj_start, obj_len);
    obj[obj_len] = '\0';

    int result = json_find_str(obj, field, out, len);
    if (result) {
        /* Adjust pointer from obj-relative to json-relative */
        int offset = (int)(*out - obj);
        *out = obj_start + offset;
    }
    free(obj);
    return result;
}