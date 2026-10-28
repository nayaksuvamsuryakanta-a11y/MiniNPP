#include "textutil.h"

#include <stdlib.h>
#include <string.h>

static int utf8_valid(const unsigned char *s, size_t n, int more_follows)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t need;
        unsigned int cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 2;
            cp = (unsigned int)(c & 0x1F);
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 3;
            cp = (unsigned int)(c & 0x0F);
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 4;
            cp = (unsigned int)(c & 0x07);
        } else {
            return 0;
        }
        if (i + need > n) return more_follows && i + need == n + (need - (n - i));
        if ((s[i + 1] & 0xC0) != 0x80) return 0;
        if (need >= 3 && (s[i + 2] & 0xC0) != 0x80) return 0;
        if (need == 4 && (s[i + 3] & 0xC0) != 0x80) return 0;
        if (need == 3 && c == 0xE0 && s[i + 1] < 0xA0) return 0;
        if (need == 3 && c == 0xED && s[i + 1] >= 0xA0) return 0;
        if (need == 4 && c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (need == 4 && c == 0xF4 && s[i + 1] >= 0x90) return 0;
        if (need == 2) cp = (cp << 6) | (s[i + 1] & 0x3F);
        else if (need == 3)
            cp = (cp << 12) | ((unsigned int)(s[i + 1] & 0x3F) << 6) |
                 (s[i + 2] & 0x3F);
        else
            cp = (cp << 18) | ((unsigned int)(s[i + 1] & 0x3F) << 12) |
                 ((unsigned int)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += need;
    }
    return 1;
}

TuEncoding tu_detect_encoding(const unsigned char *data, size_t len,
                              int more_follows, size_t *bom_len)
{
    if (bom_len) *bom_len = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        if (bom_len) *bom_len = 3;
        return TU_ENC_UTF8_BOM;
    }
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        if (bom_len) *bom_len = 2;
        return TU_ENC_UTF16LE;
    }
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        if (bom_len) *bom_len = 2;
        return TU_ENC_UTF16BE;
    }
    return utf8_valid(data, len, more_follows) ? TU_ENC_UTF8 : TU_ENC_ANSI;
}

int tu_detect_eol(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') return i + 1 < n && s[i + 1] == '\n'
            ? TU_EOL_CRLF : TU_EOL_CR;
        if (s[i] == '\n') return TU_EOL_LF;
    }
    return TU_EOL_CRLF;
}

const char *tu_eol_string(int eol)
{
    if (eol == TU_EOL_CR) return "\r";
    if (eol == TU_EOL_LF) return "\n";
    return "\r\n";
}

static unsigned int get16(const unsigned char *p, int be)
{
    return be ? ((unsigned int)p[0] << 8) | p[1] : p[0] | ((unsigned int)p[1] << 8);
}

static size_t put_utf8(char *out, size_t pos, unsigned int cp)
{
    if (cp <= 0x7F) out[pos++] = (char)cp;
    else if (cp <= 0x7FF) {
        out[pos++] = (char)(0xC0 | (cp >> 6));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out[pos++] = (char)(0xE0 | (cp >> 12));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[pos++] = (char)(0xF0 | (cp >> 18));
        out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
    }
    return pos;
}

char *tu_utf16_to_utf8(const unsigned char *src, size_t n, int big_endian,
                       size_t *out_len)
{
    size_t i, cap = n * 2 + 1, out = 0;
    char *result = (char *)malloc(cap);
    if (!result) return NULL;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned int u = get16(src + i, big_endian);
        unsigned int cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            unsigned int v = get16(src + i + 2, big_endian);
            if (v >= 0xDC00 && v <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + v - 0xDC00;
                i += 2;
            } else cp = 0xFFFD;
        } else if (u >= 0xD800 && u <= 0xDFFF) cp = 0xFFFD;
        out = put_utf8(result, out, cp);
    }
    result[out] = 0;
    if (out_len) *out_len = out;
    return result;
}

static size_t decode_utf8(const unsigned char *s, size_t n, size_t *used,
                          unsigned int *cp)
{
    unsigned char c = s[0];
    size_t need = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    size_t i;
    unsigned int v = c & (need == 1 ? 0x7F : need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    if (need > n) { *used = 1; *cp = 0xFFFD; return 1; }
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *used = 1; *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *used = need;
    *cp = v;
    return need;
}

unsigned char *tu_utf8_to_utf16(const char *src, size_t n, int big_endian,
                                size_t *out_len)
{
    size_t i = 0, units = 0, used;
    unsigned int cp;
    unsigned char *out;
    while (i < n) {
        decode_utf8((const unsigned char *)src + i, n - i, &used, &cp);
        units += cp > 0xFFFF ? 2 : 1;
        i += used;
    }
    out = (unsigned char *)malloc(units * 2 + 2);
    if (!out) return NULL;
    i = 0; units = 0;
    while (i < n) {
        unsigned int u;
        decode_utf8((const unsigned char *)src + i, n - i, &used, &cp);
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            u = 0xD800 | (cp >> 10);
            out[units++] = (unsigned char)(big_endian ? u >> 8 : u);
            out[units++] = (unsigned char)(big_endian ? u : u >> 8);
            u = 0xDC00 | (cp & 0x3FF);
            out[units++] = (unsigned char)(big_endian ? u >> 8 : u);
            out[units++] = (unsigned char)(big_endian ? u : u >> 8);
        } else {
            u = cp <= 0xD7FF || cp >= 0xE000 ? cp : 0xFFFD;
            out[units++] = (unsigned char)(big_endian ? u >> 8 : u);
            out[units++] = (unsigned char)(big_endian ? u : u >> 8);
        }
        i += used;
    }
    out[units] = out[units + 1] = 0;
    if (out_len) *out_len = units;
    return out;
}

typedef struct { const char *p; size_t n; } Line;

static int split_lines(const char *src, size_t n, const char *eol,
                       Line **lines, size_t *count, int *trailing)
{
    size_t el = strlen(eol), cap = 8, used = 0, pos = 0;
    Line *a = (Line *)malloc(cap * sizeof(*a));
    if (!a) return 0;
    *trailing = n >= el && memcmp(src + n - el, eol, el) == 0;
    while (pos < n) {
        size_t end = pos;
        while (end + el <= n && memcmp(src + end, eol, el) != 0) end++;
        if (used == cap) {
            Line *b;
            cap *= 2;
            b = (Line *)realloc(a, cap * sizeof(*a));
            if (!b) { free(a); return 0; }
            a = b;
        }
        a[used].p = src + pos;
        a[used].n = end - pos;
        used++;
        if (end == n) break;
        pos = end + el;
    }
    *lines = a;
    *count = used;
    return 1;
}

static int line_cmp(const void *va, const void *vb)
{
    const Line *a = (const Line *)va, *b = (const Line *)vb;
    size_t n = a->n < b->n ? a->n : b->n;
    int c = memcmp(a->p, b->p, n);
    return c ? c : (a->n > b->n) - (a->n < b->n);
}

static char *emit_lines(Line *lines, size_t count, const char *eol,
                        int trailing, size_t *out_len)
{
    size_t i, el = strlen(eol), total = trailing && count ? el : 0;
    char *out, *p;
    for (i = 0; i < count; i++) total += lines[i].n + (i + 1 < count ? el : 0);
    out = (char *)malloc(total + 1);
    if (!out) return NULL;
    p = out;
    for (i = 0; i < count; i++) {
        memcpy(p, lines[i].p, lines[i].n); p += lines[i].n;
        if (i + 1 < count || (trailing && count)) { memcpy(p, eol, el); p += el; }
    }
    *p = 0;
    if (out_len) *out_len = total;
    return out;
}

static char *transform_lines(const char *src, size_t n, const char *eol,
                             int mode, int descending, size_t *out_len)
{
    Line *lines, *kept;
    size_t count, i, used = 0;
    int trailing;
    char *out;
    if (!split_lines(src, n, eol, &lines, &count, &trailing)) return NULL;
    kept = (Line *)malloc((count ? count : 1) * sizeof(*kept));
    if (!kept) { free(lines); return NULL; }
    for (i = 0; i < count; i++) {
        size_t j;
        int keep = 1;
        if (mode == 1) {
            for (j = 0; j < used; j++)
                if (kept[j].n == lines[i].n && memcmp(kept[j].p, lines[i].p, lines[i].n) == 0) keep = 0;
        } else if (mode == 2) {
            for (j = 0; j < lines[i].n; j++)
                if (lines[i].p[j] != ' ' && lines[i].p[j] != '\t') break;
            if (j == lines[i].n) keep = 0;
        } else if (mode == 3) {
            while (lines[i].n && (lines[i].p[lines[i].n - 1] == ' ' || lines[i].p[lines[i].n - 1] == '\t')) lines[i].n--;
        }
        if (keep) kept[used++] = lines[i];
    }
    if (mode == 0) {
        qsort(kept, used, sizeof(*kept), line_cmp);
        if (descending) {
            for (i = 0; i < used / 2; i++) {
                Line t = kept[i]; kept[i] = kept[used - 1 - i]; kept[used - 1 - i] = t;
            }
        }
    }
    out = emit_lines(kept, used, eol, trailing, out_len);
    free(kept); free(lines);
    return out;
}

char *tu_sort_lines(const char *src, size_t n, const char *eol, int descending, size_t *out_len)
{ return transform_lines(src, n, eol, 0, descending, out_len); }
char *tu_unique_lines(const char *src, size_t n, const char *eol, size_t *out_len)
{ return transform_lines(src, n, eol, 1, 0, out_len); }
char *tu_remove_empty_lines(const char *src, size_t n, const char *eol, size_t *out_len)
{ return transform_lines(src, n, eol, 2, 0, out_len); }
char *tu_trim_trailing(const char *src, size_t n, const char *eol, size_t *out_len)
{ return transform_lines(src, n, eol, 3, 0, out_len); }

