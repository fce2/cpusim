/**
 * json.h - Minimal JSON library
 *
 * Streaming parser for token-by-token extraction,
 * plus string-based helpers for search/escape.
 *
 * Build:  #include "json.h", compile+link json.c
 */

#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ================================================================== */
/*  Streaming parser                                                    */
/* ================================================================== */

typedef struct {
    const uint8_t* data;
    int pos;
    int len;
} JsonParser;

void     jp_init(JsonParser* p, const uint8_t* data, int len);
uint8_t  jp_peek(JsonParser* p);
uint8_t  jp_next(JsonParser* p);
int64_t  jp_int(JsonParser* p);
unsigned jp_uint(JsonParser* p);
int      jp_string(JsonParser* p, char* buf, int buf_cap);
int      jp_key_eq(JsonParser* p, const char* expected);
int      jp_expect(JsonParser* p, char c);

/* ================================================================== */
/*  Inline helpers                                                       */
/* ================================================================== */

/** Decode a 4-hex-digit Unicode escape (\uXXXX) to UTF-8.
 *  Writes 1-3 bytes to dst. Returns number of bytes written (1, 2, or 3).
 *  val is the 16-bit Unicode code point. */
static inline int json_decode_u16(unsigned int val, char *dst) {
    if (val < 0x80) {
        dst[0] = (char)val;
        return 1;
    } else if (val < 0x800) {
        dst[0] = (char)(0xC0 | (val >> 6));
        dst[1] = (char)(0x80 | (val & 0x3F));
        return 2;
    } else {
        dst[0] = (char)(0xE0 | (val >> 12));
        dst[1] = (char)(0x80 | ((val >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (val & 0x3F));
        return 3;
    }
}

/** Parse 4 hex digits from s+1..s+4 into a 16-bit value.
 *  Returns 1 on success, 0 if any character is not hex. */
static inline int json_parse_hex4(const char *s, unsigned int *val) {
    unsigned int v = 0;
    for (int i = 1; i <= 4; i++) {
        v <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') v += c - '0';
        else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
        else return 0;
    }
    *val = v;
    return 1;
}

/* ================================================================== */
/*  String-based helpers                                                */
/* ================================================================== */

/**
 * json_find_str - Find "key":"value" in a JSON C string.
 *
 * Sets *out to start of value (inside quotes), *len to byte length.
 * Returns 1 if found, 0 if not. Pointer is into input, not NUL-terminated.
 */
int  json_find_str(const char *json, const char *key,
                   const char **out, int *len);

/** Find "key":<number> or "key":"<number>" in JSON. Returns the int value, 0 if not found. */
int  json_find_int(const char *json, const char *key);

/** Duplicate a json_find_str result. Caller must free(). */
char *json_str_dup(const char *out, int len);

/** Escape a C string for JSON. dst must hold 2*strlen(src)+1. Returns bytes written. */
int  json_escape(const char *src, char *dst);

/** Decode JSON escape sequences in-place: \u003c -> <, \\n -> newline, etc. */
void json_decode_inplace(char *s);

/** Write JSON-escaped string to a FILE stream. Returns bytes written. */
int  json_fescape(FILE *fp, const char *s);

/**
 * json_find_nested - Find a value in nested JSON using dotted key notation.
 *
 * Finds "section.field" by locating the "section" object, then finding
 * "field" within it. For single-level keys (no dot), delegates to json_find_str.
 * Returns 1 if found, 0 if not.
 */
int  json_find_nested(const char *json, const char *dotted_key,
                      const char **out, int *len);

/* ================================================================== */
/*  C++ JSON DOM class                                                  */
/* ================================================================== */

#ifdef __cplusplus

#include <string>
#include <vector>
#include <map>
#include <initializer_list>
#include <cstdio>
#include <cmath>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::map<std::string, Json> o_;

    Json() : type_(Null) {}
    Json(std::nullptr_t) : type_(Null) {}
    Json(bool v) : type_(Bool), b_(v) {}
    Json(int v) : type_(Number), n_(v) {}
    Json(double v) : type_(Number), n_(v) {}
    Json(const char* v) : type_(String), s_(v) {}
    Json(const std::string& v) : type_(String), s_(v) {}

    Json(std::initializer_list<std::pair<std::string, Json>> init) : type_(Object) {
        for (const auto& p : init) o_[p.first] = p.second;
    }

    static Json object() { Json j; j.type_ = Object; return j; }
    static Json array() { Json j; j.type_ = Array; return j; }
    static Json array(std::initializer_list<Json> init) {
        Json j; j.type_ = Array;
        for (const auto& v : init) j.a_.push_back(v);
        return j;
    }

    static Json parse(const std::string& s) {
        size_t pos = 0;
        return parse_value_(s, pos);
    }

    Json& operator[](const std::string& key) {
        if (type_ != Object) *this = object();
        return o_[key];
    }
    const Json& operator[](const std::string& key) const {
        static Json nil;
        auto it = o_.find(key);
        return it != o_.end() ? it->second : nil;
    }
    Json& operator[](int idx) { return a_[idx]; }
    const Json& operator[](int idx) const { return a_[idx]; }

    Json value(const std::string& key, const Json& def) const {
        auto it = o_.find(key);
        return it != o_.end() ? it->second : def;
    }
    std::string value(const std::string& key, const char* def) const {
        auto it = o_.find(key);
        return it != o_.end() && it->second.type_ == String ? it->second.s_ : std::string(def);
    }

    explicit operator std::string() const { return type_ == String ? s_ : dump(); }
    explicit operator double() const { return type_ == Number ? n_ : 0.0; }
    explicit operator int() const { return type_ == Number ? (int)n_ : 0; }
    explicit operator bool() const { return type_ != Null; }

    std::string as_string() const { return type_ == String ? s_ : dump(); }
    double as_double() const { return type_ == Number ? n_ : 0.0; }
    int as_int() const { return type_ == Number ? (int)n_ : 0; }

    std::string dump() const {
        char buf[64];
        switch (type_) {
            case Null: return "null";
            case Bool: return b_ ? "true" : "false";
            case Number: {
                if (!std::isinf(n_) && n_ == (long long)n_ && fabs(n_) < 1e15)
                    snprintf(buf, sizeof(buf), "%lld", (long long)n_);
                else
                    snprintf(buf, sizeof(buf), "%.17g", n_);
                return buf;
            }
            case String: {
                std::string out = "\"";
                int elen = (int)s_.size() * 6 + 1;
                char *ebuf = (char *)malloc(elen);
                if (ebuf) {
                    int n = json_escape(s_.c_str(), ebuf);
                    out.append(ebuf, n);
                    free(ebuf);
                }
                out += '"';
                return out;
            }
            case Array: {
                std::string out = "[";
                for (size_t i = 0; i < a_.size(); i++) {
                    if (i) out += ",";
                    out += a_[i].dump();
                }
                out += "]";
                return out;
            }
            case Object: {
                std::string out = "{";
                bool first = true;
                for (const auto& p : o_) {
                    if (!first) out += ",";
                    first = false;
                    out += "\"";
                    {
                        int klen = (int)p.first.size() * 6 + 1;
                        char *kbuf = (char *)malloc(klen);
                        if (kbuf) {
                            int n = json_escape(p.first.c_str(), kbuf);
                            out.append(kbuf, n);
                            free(kbuf);
                        }
                    }
                    out += "\":";
                    out += p.second.dump();
                }
                out += "}";
                return out;
            }
        }
        return "null";
    }

    bool is_null() const { return type_ == Null; }
    Type type() const { return type_; }
    size_t size() const { return type_ == Array ? a_.size() : o_.size(); }

private:
    static void skip_ws_(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\r'||s[pos]=='\n')) pos++;
    }

    static Json parse_value_(const std::string& s, size_t& pos) {
        skip_ws_(s, pos);
        if (pos >= s.size()) return Json();

        char c = s[pos];
        if (c == '"') return Json(parse_string_(s, pos));

        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = pos;
            if (s[pos] == '-') pos++;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
            if (pos < s.size() && s[pos] == '.') {
                pos++;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                pos++;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
            }
            return Json(std::stod(s.substr(start, pos - start)));
        }

        if (s.compare(pos, 4, "true") == 0)  { pos += 4; return Json(true); }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; return Json(false); }
        if (s.compare(pos, 4, "null") == 0)  { pos += 4; return Json(); }

        if (c == '[') {
            Json arr = Json::array();
            pos++;
            skip_ws_(s, pos);
            if (pos < s.size() && s[pos] != ']') {
                arr.a_.push_back(parse_value_(s, pos));
                while (pos < s.size()) {
                    skip_ws_(s, pos);
                    if (pos >= s.size() || s[pos] != ',') break;
                    pos++;
                    arr.a_.push_back(parse_value_(s, pos));
                }
            }
            skip_ws_(s, pos);
            if (pos < s.size() && s[pos] == ']') pos++;
            return arr;
        }

        if (c == '{') {
            Json obj = Json::object();
            pos++;
            skip_ws_(s, pos);
            if (pos < s.size() && s[pos] != '}') {
                std::string key = parse_string_(s, pos);
                skip_ws_(s, pos);
                if (pos < s.size() && s[pos] == ':') pos++;
                obj.o_[key] = parse_value_(s, pos);
                while (pos < s.size()) {
                    skip_ws_(s, pos);
                    if (pos >= s.size() || s[pos] != ',') break;
                    pos++;
                    skip_ws_(s, pos);
                    std::string k = parse_string_(s, pos);
                    skip_ws_(s, pos);
                    if (pos < s.size() && s[pos] == ':') pos++;
                    obj.o_[k] = parse_value_(s, pos);
                }
            }
            skip_ws_(s, pos);
            if (pos < s.size() && s[pos] == '}') pos++;
            return obj;
        }

        return Json();
    }

    static std::string parse_string_(const std::string& s, size_t& pos) {
        std::string result;
        pos++; // skip opening "
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                pos++;
                switch (s[pos]) {
                    case '"':  result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'u': {
                        if (pos + 4 < s.size()) {
                            unsigned int val;
                            if (json_parse_hex4(s.c_str() + pos, &val)) {
                                char u8[3];
                                int u8len = json_decode_u16(val, u8);
                                result.append(u8, u8len);
                            }
                            pos += 4;
                        }
                        break;
                    }
                    default: result += s[pos]; break;
                }
            } else {
                result += s[pos];
            }
            pos++;
        }
        if (pos < s.size()) pos++; // skip closing "
        return result;
    }
};

#endif /* __cplusplus */

#endif /* JSON_H */