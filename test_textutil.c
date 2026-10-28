#include "textutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;

static void check(int ok, const char *name)
{
    checks++;
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failed++;
}

static int text_is(const char *got, size_t n, const char *want)
{
    return got && n == strlen(want) && memcmp(got, want, n) == 0;
}

int main(void)
{
    size_t bom, n, m;
    TuEncoding enc;
    char *s;
    unsigned char *u;
    static const unsigned char utf16le[] = { 0x48, 0, 0x69, 0, 0x20, 0, 0x3D, 0xD8, 0x00, 0xDE };
    static const unsigned char utf16be[] = { 0, 0x48, 0, 0x69, 0, 0x20, 0xD8, 0x3D, 0xDE, 0 };
    enc = tu_detect_encoding((const unsigned char *)"\xEF\xBB\xBFx", 4, 0, &bom);
    check(enc == TU_ENC_UTF8_BOM && bom == 3, "UTF-8 BOM detection");
    enc = tu_detect_encoding((const unsigned char *)"\xFF\xFEx", 3, 0, &bom);
    check(enc == TU_ENC_UTF16LE && bom == 2, "UTF-16 LE BOM detection");
    enc = tu_detect_encoding((const unsigned char *)"\xFE\xFFx", 3, 0, &bom);
    check(enc == TU_ENC_UTF16BE && bom == 2, "UTF-16 BE BOM detection");
    enc = tu_detect_encoding((const unsigned char *)"caf\xC3\xA9", 5, 0, &bom);
    check(enc == TU_ENC_UTF8 && bom == 0, "valid UTF-8 detection");
    enc = tu_detect_encoding((const unsigned char *)"caf\xE9", 4, 0, &bom);
    check(enc == TU_ENC_ANSI, "ANSI detection");
    enc = tu_detect_encoding((const unsigned char *)"\xC3", 1, 1, &bom);
    check(enc == TU_ENC_UTF8, "truncated UTF-8 sample");
    check(tu_detect_eol("a\r\nb\r", 5) == TU_EOL_CRLF, "CRLF detection");
    check(tu_detect_eol("a\rb\n", 4) == TU_EOL_CR, "CR detection");
    check(tu_detect_eol("a\nb", 3) == TU_EOL_LF, "LF detection");
    check(tu_detect_eol("abc", 3) == TU_EOL_CRLF, "default EOL");
    check(strcmp(tu_eol_string(TU_EOL_CRLF), "\r\n") == 0, "CRLF string");
    check(strcmp(tu_eol_string(TU_EOL_CR), "\r") == 0, "CR string");
    check(strcmp(tu_eol_string(TU_EOL_LF), "\n") == 0, "LF string");
    s = tu_utf16_to_utf8(utf16le, sizeof utf16le, 0, &n);
    check(text_is(s, n, "Hi \xF0\x9F\x98\x80"), "UTF-16 LE decode surrogate");
    free(s);
    s = tu_utf16_to_utf8(utf16be, sizeof utf16be, 1, &n);
    check(text_is(s, n, "Hi \xF0\x9F\x98\x80"), "UTF-16 BE decode surrogate");
    free(s);
    u = tu_utf8_to_utf16("Hi \xF0\x9F\x98\x80", 7, 0, &n);
    check(u && n == sizeof utf16le && memcmp(u, utf16le, n) == 0, "UTF-16 LE encode");
    free(u);
    u = tu_utf8_to_utf16("Hi \xF0\x9F\x98\x80", 7, 1, &n);
    check(u && n == sizeof utf16be && memcmp(u, utf16be, n) == 0, "UTF-16 BE encode");
    free(u);
    s = tu_utf16_to_utf8(NULL, 0, 0, &n);
    check(text_is(s, n, ""), "UTF-16 empty decode");
    free(s);
    u = tu_utf8_to_utf16("", 0, 0, &n);
    check(u && n == 0 && u[0] == 0 && u[1] == 0, "UTF-16 empty encode");
    free(u);
    s = tu_sort_lines("b\na\nc\n", 6, "\n", 0, &n);
    check(text_is(s, n, "a\nb\nc\n"), "sort ascending trailing EOL");
    free(s);
    s = tu_sort_lines("b\na\nc", 5, "\n", 1, &n);
    check(text_is(s, n, "c\nb\na"), "sort descending no trailing EOL");
    free(s);
    s = tu_unique_lines("a\nb\na\nc\nb", 9, "\n", &n);
    check(text_is(s, n, "a\nb\nc"), "unique non-adjacent duplicates");
    free(s);
    s = tu_remove_empty_lines("a\n \n\t\nb\n", 8, "\n", &n);
    check(text_is(s, n, "a\nb\n"), "remove blank lines");
    free(s);
    s = tu_trim_trailing("a  \nb\t\nc", 8, "\n", &n);
    check(text_is(s, n, "a\nb\nc"), "trim trailing spaces");
    free(s);
    s = tu_sort_lines("b\na", 3, "\n", 0, &n);
    check(text_is(s, n, "a\nb"), "sort without trailing EOL");
    free(s);
    s = tu_unique_lines("a\nb\n", 4, "\n", &n);
    check(text_is(s, n, "a\nb\n"), "unique with trailing EOL");
    free(s);
    s = tu_remove_empty_lines("a\n\nb", 4, "\n", &n);
    check(text_is(s, n, "a\nb"), "remove empty without trailing EOL");
    free(s);
    s = tu_trim_trailing("a \nb \n", 6, "\n", &n);
    check(text_is(s, n, "a\nb\n"), "trim with trailing EOL");
    free(s);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

