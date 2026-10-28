#ifndef MININPP_TEXTUTIL_H
#define MININPP_TEXTUTIL_H

#include <stddef.h>

typedef enum {
    TU_ENC_UTF8 = 0,
    TU_ENC_UTF8_BOM,
    TU_ENC_UTF16LE,
    TU_ENC_UTF16BE,
    TU_ENC_ANSI
} TuEncoding;

enum {
    TU_EOL_CRLF = 0,
    TU_EOL_CR = 1,
    TU_EOL_LF = 2
};

TuEncoding tu_detect_encoding(const unsigned char *data, size_t len,
                              int more_follows, size_t *bom_len);
int tu_detect_eol(const char *s, size_t n);
const char *tu_eol_string(int eol);
char *tu_utf16_to_utf8(const unsigned char *src, size_t n, int big_endian,
                       size_t *out_len);
unsigned char *tu_utf8_to_utf16(const char *src, size_t n, int big_endian,
                                size_t *out_len);
char *tu_sort_lines(const char *src, size_t n, const char *eol,
                    int descending, size_t *out_len);
char *tu_unique_lines(const char *src, size_t n, const char *eol,
                      size_t *out_len);
char *tu_remove_empty_lines(const char *src, size_t n, const char *eol,
                            size_t *out_len);
char *tu_trim_trailing(const char *src, size_t n, const char *eol,
                       size_t *out_len);

#endif

