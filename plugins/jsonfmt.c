/*
 * Sample MiniNPP plugin: "JSON Tools" (pretty-print / minify the whole document).
 *
 * Build (MinGW-w64):
 *   gcc -O2 -shared -o jsonfmt.dll jsonfmt.c -I<scintilla>/include -I..
 * Then copy jsonfmt.dll into the "plugins" folder next to MiniNPP.exe.
 *
 * It does not validate the JSON; it only re-flows the text, leaving strings untouched.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "Scintilla.h"
#include "mininpp_plugin.h"

static const MiniHost *H;

typedef struct { char *p; size_t n, cap; } Buf;

static void put(Buf *b, char c)
{
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->p = (char *)realloc(b->p, b->cap);
    }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void newline(Buf *b, int depth)
{
    int i;
    put(b, '\n');
    for (i = 0; i < depth * 2; i++) put(b, ' ');
}

static char *reformat(const char *s, size_t n, int pretty, size_t *outLen)
{
    Buf b = { NULL, 0, 0 };
    int inStr = 0, esc = 0, depth = 0;
    size_t i;
    put(&b, 0); b.n = 0;                                   /* make sure b.p is allocated */
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (inStr) {
            put(&b, c);
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') inStr = 0;
            continue;
        }
        switch (c) {
        case ' ': case '\t': case '\r': case '\n':
            break;                                         /* drop whitespace outside strings */
        case '"':
            inStr = 1;
            put(&b, c);
            break;
        case '{': case '[': {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
            put(&b, c);
            if (j < n && ((c == '{' && s[j] == '}') || (c == '[' && s[j] == ']'))) {
                put(&b, s[j]);                             /* empty container stays on one line */
                i = j;
            } else if (pretty) {
                depth++;
                newline(&b, depth);
            }
            break;
        }
        case '}': case ']':
            if (pretty) { if (depth > 0) depth--; newline(&b, depth); }
            put(&b, c);
            break;
        case ',':
            put(&b, c);
            if (pretty) newline(&b, depth);
            break;
        case ':':
            put(&b, c);
            if (pretty) put(&b, ' ');
            break;
        default:
            put(&b, c);
        }
    }
    *outLen = b.n;
    return b.p;
}

MININPP_EXPORT void __cdecl mininpp_init(const MiniHost *host) { H = host; }
MININPP_EXPORT const wchar_t *__cdecl mininpp_name(void) { return L"JSON Tools"; }
MININPP_EXPORT int __cdecl mininpp_command_count(void) { return 2; }

MININPP_EXPORT const wchar_t *__cdecl mininpp_command_name(int i)
{
    return i == 0 ? L"Pretty-print JSON" : L"Minify JSON";
}

MININPP_EXPORT void __cdecl mininpp_run(int i)
{
    size_t n = (size_t)H->sci(SCI_GETLENGTH, 0, 0), outLen = 0;
    const char *text = (const char *)H->sci(SCI_GETCHARACTERPOINTER, 0, 0);
    char *out = reformat(text, n, i == 0, &outLen);
    if (!out) return;
    H->sci(SCI_BEGINUNDOACTION, 0, 0);                     /* one Ctrl+Z undoes the whole reformat */
    H->sci(SCI_TARGETWHOLEDOCUMENT, 0, 0);
    H->sci(SCI_REPLACETARGET, (WPARAM)outLen, (LPARAM)out);
    H->sci(SCI_ENDUNDOACTION, 0, 0);
    free(out);
}
