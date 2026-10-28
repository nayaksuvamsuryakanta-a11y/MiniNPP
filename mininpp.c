/*
 * MiniNPP - a small Notepad++-style text editor written in C.
 *
 * Same architecture as Notepad++ itself:
 *   - Win32 API ........ window, menus, tabs, dialogs, status bar
 *   - Scintilla ........ the editing engine (Scintilla.dll)
 *   - Lexilla .......... syntax-highlighting / folding lexers (Lexilla.dll)
 *   - textutil.c ....... portable helpers (encodings, line tools), unit-tested
 *
 * Features: syntax highlighting + folding, tabs with session restore (incl.
 * unsaved tabs), find/replace with regex + find/replace in files, multi-cursor
 * and column editing, encoding / line-ending conversion, macros, a DLL plugin
 * system, line tools, auto-completion + brace matching, large-file loading and
 * "tail -f" monitoring.  See README.md for build instructions.
 */
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "Scintilla.h"
#include "SciLexer.h"
#include "textutil.h"
#include "mininpp_plugin.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* The helper library uses the same numbers as Scintilla for line endings. */
typedef char assert_eol_crlf[(TU_EOL_CRLF == SC_EOL_CRLF) ? 1 : -1];
typedef char assert_eol_cr[(TU_EOL_CR == SC_EOL_CR) ? 1 : -1];
typedef char assert_eol_lf[(TU_EOL_LF == SC_EOL_LF) ? 1 : -1];

/* ====================================================================== */
/* Constants, globals                                                      */
/* ====================================================================== */

#define APP_NAME      L"MiniNPP"
#define PATH_CAP      1024
#define CHUNK         (1u << 20)
#define LEXER_LIMIT   (24u << 20)     /* above this size: no highlighting/folding   */
#define CONVERT_LIMIT (512u << 20)    /* max size for UTF-16 / ANSI files           */
#define SEARCH_LIMIT  (64u << 20)     /* find-in-files skips bigger files           */
#define MAX_HITS      5000

#define SW(h, m, w, l) SendMessageW((h), (m), (WPARAM)(w), (LPARAM)(l))
#define SS(m, w, l)    SW(g_sci, m, w, l)
#define SNW(b, ...)    (_snwprintf((b), sizeof(b) / sizeof((b)[0]), __VA_ARGS__), \
                        (b)[sizeof(b) / sizeof((b)[0]) - 1] = 0)

enum {
    IDM_FILE_NEW = 1001, IDM_FILE_OPEN, IDM_FILE_SAVE, IDM_FILE_SAVEAS, IDM_FILE_CLOSE, IDM_FILE_EXIT,
    IDM_EDIT_UNDO, IDM_EDIT_REDO, IDM_EDIT_CUT, IDM_EDIT_COPY, IDM_EDIT_PASTE, IDM_EDIT_SELALL, IDM_EDIT_GOTO,
    IDM_LINE_DUP, IDM_LINE_DEL, IDM_LINE_UP, IDM_LINE_DOWN,
    IDM_LINE_SORT_ASC, IDM_LINE_SORT_DESC, IDM_LINE_UNIQUE, IDM_LINE_NOEMPTY, IDM_LINE_TRIM,
    IDM_CASE_UPPER, IDM_CASE_LOWER,
    IDM_SEL_NEXT, IDM_SEL_ALL, IDM_COL_NUMBERS,
    IDM_FIND, IDM_REPLACE, IDM_FINDNEXT, IDM_FINDPREV, IDM_FINDFILES,
    IDM_VIEW_WRAP, IDM_VIEW_WS, IDM_VIEW_EOL, IDM_VIEW_FOLDALL, IDM_VIEW_UNFOLDALL,
    IDM_VIEW_ZOOMIN, IDM_VIEW_ZOOMOUT, IDM_VIEW_ZOOM0, IDM_VIEW_NEXTTAB, IDM_VIEW_PREVTAB,
    IDM_VIEW_MONITOR, IDM_VIEW_RESULTS,
    IDM_MACRO_REC, IDM_MACRO_PLAY, IDM_MACRO_MULTI, IDM_MACRO_TOEND,
    IDM_PLUGINS_ADMIN,
    IDM_ENC_BASE = 1200,      /* + TuEncoding        (5 items) */
    IDM_EOL_BASE = 1220,      /* + SC_EOL_*          (3 items) */
    IDM_LANG_BASE = 1240,     /* + LANG_*                       */
    IDM_PLUGIN_BASE = 1400    /* + command number               */
};

enum { IDC_TAB = 101, IDC_SCI, IDC_STATUS, IDC_RESULTS };

enum { TIMER_SESSION = 1, TIMER_MONITOR = 2 };

typedef struct Doc {
    sptr_t    ptr;                    /* Scintilla document pointer           */
    wchar_t   path[PATH_CAP];         /* empty = untitled                     */
    int       enc;                    /* TuEncoding used when saving          */
    int       lang;                   /* LANG_*                               */
    int       dirty;                  /* unsaved changes                      */
    int       touched;                /* edited at least once since opening   */
    int       needsBackup;            /* text changed since last backup file  */
    int       uid;                    /* names the backup file                */
    int       untitledNo;
    int       monitor;                /* tail -f mode                         */
    ULONGLONG monSize;                /* file size at last read               */
    sptr_t    caret, topLine;         /* saved view state while in background */
} Doc;

typedef void *(WINAPI *CreateLexerFn)(const char *);   /* Lexilla's CreateLexer */

static HINSTANCE g_hInst;
static HWND g_main, g_tab, g_sci, g_status, g_results, g_scratch, g_findWnd;
static HACCEL g_accel;
static wchar_t g_exeDir[PATH_CAP], g_dataDir[PATH_CAP];
static CreateLexerFn g_createLexer;
static int g_wrap, g_showWs, g_showEol, g_resultsVisible, g_sessionDirty;
static int g_nextUntitled = 1, g_nextUid = 1;

static Doc **g_docs;
static int g_nDocs, g_capDocs, g_cur = -1;

static HMENU g_mEnc, g_mEol, g_mLang, g_mView, g_mMacro, g_mPlugins;

/* forward declarations */
static void UpdateStatus(void);
static void UpdateTab(int i);
static void UpdateTitle(void);
static void UpdateLineMargin(void);
static void SwitchTo(int idx);
static void ApplyLanguage(Doc *d, int lang);
static void FlushBackup(Doc *d);
static void DeleteBackup(const Doc *d);
static int  SaveDoc(Doc *d, int saveAs);
static Doc *NewUntitled(void);
static Doc *OpenDocument(const wchar_t *p);
static int  CloseDoc(int idx);
static void ShowFind(int focusReplace);
static void Layout(void);

/* ====================================================================== */
/* Small helpers                                                           */
/* ====================================================================== */

static void *xalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { MessageBoxW(NULL, L"Out of memory.", APP_NAME, MB_ICONERROR); ExitProcess(1); }
    return p;
}

static void *xrealloc(void *old, size_t n)
{
    void *p = realloc(old, n ? n : 1);
    if (!p) { MessageBoxW(NULL, L"Out of memory.", APP_NAME, MB_ICONERROR); ExitProcess(1); }
    return p;
}

static void CopyW(wchar_t *d, size_t cap, const wchar_t *s)
{
    if (!cap) return;
    wcsncpy(d, s, cap - 1);
    d[cap - 1] = 0;
}

static char *WideToUtf8(const wchar_t *w, int *outLen)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)xalloc((size_t)(n > 0 ? n : 1));
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    if (outLen) *outLen = n > 0 ? n - 1 : 0;
    return s;
}

static wchar_t *Utf8ToWide(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t *w = (wchar_t *)xalloc(((size_t)(n > 0 ? n : 0) + 2) * sizeof(wchar_t));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n > 0 ? n : 0] = 0;
    return w;
}

static char *StrDupUtf8(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xalloc(n);
    memcpy(p, s, n);
    return p;
}

static void StatusSet(int part, const wchar_t *text)
{
    SendMessageW(g_status, SB_SETTEXTW, (WPARAM)part, (LPARAM)text);
}

static void InitPaths(void)
{
    wchar_t appdata[MAX_PATH] = L"", b[PATH_CAP], *sl;
    GetModuleFileNameW(NULL, g_exeDir, PATH_CAP);
    sl = wcsrchr(g_exeDir, L'\\');
    if (sl) *sl = 0;
    SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata);
    SNW(g_dataDir, L"%ls\\MiniNPP", appdata);
    CreateDirectoryW(g_dataDir, NULL);
    SNW(b, L"%ls\\backup", g_dataDir);
    CreateDirectoryW(b, NULL);
}

/* ====================================================================== */
/* File I/O helpers                                                        */
/* ====================================================================== */

static int ReadWholeFile(const wchar_t *path, size_t limit, unsigned char **out, size_t *outLen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    size_t n, got = 0;
    unsigned char *buf;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &sz) || (unsigned long long)sz.QuadPart > limit) { CloseHandle(h); return 0; }
    n = (size_t)sz.QuadPart;
    buf = (unsigned char *)malloc(n + 1);
    if (!buf) { CloseHandle(h); return 0; }
    while (got < n) {
        DWORD want = (DWORD)((n - got) > CHUNK ? CHUNK : (n - got)), r = 0;
        if (!ReadFile(h, buf + got, want, &r, NULL) || !r) break;
        got += r;
    }
    CloseHandle(h);
    buf[got] = 0;
    *out = buf;
    *outLen = got;
    return 1;
}

static int WriteAll(HANDLE h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    while (n) {
        DWORD w = 0, chunk = (DWORD)(n > (16u << 20) ? (16u << 20) : n);
        if (!WriteFile(h, p, chunk, &w, NULL) || !w) return 0;
        p += w;
        n -= w;
    }
    return 1;
}

/* Writes a+b to a temp file, then renames it over <path>, so a crash or full
 * disk in the middle of a save can never leave a half-written file. */
static int WriteFileAtomic(const wchar_t *path, const void *a, size_t an, const void *b, size_t bn)
{
    wchar_t tmp[PATH_CAP + 16];
    HANDLE h;
    int ok;
    _snwprintf(tmp, PATH_CAP + 16, L"%ls.mininpp-tmp", path);
    tmp[PATH_CAP + 15] = 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteAll(h, a, an) && WriteAll(h, b, bn);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}

/* Any supported encoding -> UTF-8 (malloc'd, NUL-terminated). */
static char *ToUtf8(const unsigned char *src, size_t n, TuEncoding enc, size_t *outLen)
{
    char *out;
    if (enc == TU_ENC_UTF16LE || enc == TU_ENC_UTF16BE)
        return tu_utf16_to_utf8(src, n, enc == TU_ENC_UTF16BE, outLen);
    if (enc == TU_ENC_ANSI && n) {
        int wl = MultiByteToWideChar(CP_ACP, 0, (const char *)src, (int)n, NULL, 0), ul;
        if (wl <= 0) return NULL;
        wchar_t *w = (wchar_t *)malloc(((size_t)wl + 1) * sizeof(wchar_t));
        if (!w) return NULL;
        MultiByteToWideChar(CP_ACP, 0, (const char *)src, (int)n, w, wl);
        ul = WideCharToMultiByte(CP_UTF8, 0, w, wl, NULL, 0, NULL, NULL);
        if (ul <= 0) { free(w); return NULL; }
        out = (char *)malloc((size_t)ul + 1);
        if (out) {
            WideCharToMultiByte(CP_UTF8, 0, w, wl, out, ul, NULL, NULL);
            out[ul] = 0;
            *outLen = (size_t)ul;
        }
        free(w);
        return out;
    }
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = 0;
    *outLen = n;
    return out;
}

/* UTF-8 text -> file in the requested encoding.
 * Returns 1 = ok, 0 = failed, -1 = cancelled by the user. */
static int WriteEncoded(const wchar_t *path, const char *t, size_t n, TuEncoding enc, int interactive)
{
    int ok = 0;
    switch (enc) {
    case TU_ENC_UTF8:
        return WriteFileAtomic(path, NULL, 0, t, n);
    case TU_ENC_UTF8_BOM: {
        static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        return WriteFileAtomic(path, bom, 3, t, n);
    }
    case TU_ENC_UTF16LE:
    case TU_ENC_UTF16BE: {
        size_t m = 0;
        int be = (enc == TU_ENC_UTF16BE);
        unsigned char *w = tu_utf8_to_utf16(t, n, be, &m);
        unsigned char bom[2];
        bom[0] = be ? 0xFE : 0xFF;
        bom[1] = be ? 0xFF : 0xFE;
        ok = w && WriteFileAtomic(path, bom, 2, w, m);
        free(w);
        return ok;
    }
    default: {
        int wl = MultiByteToWideChar(CP_UTF8, 0, t, (int)n, NULL, 0), al;
        BOOL lossy = FALSE;
        wchar_t *w = (wchar_t *)malloc(((size_t)wl + 1) * sizeof(wchar_t));
        char *a;
        if (!w) return 0;
        MultiByteToWideChar(CP_UTF8, 0, t, (int)n, w, wl);
        al = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, wl, NULL, 0, NULL, &lossy);
        if (lossy) {
            if (!interactive) { free(w); return 0; }
            if (MessageBoxW(g_main, L"Some characters cannot be stored in the ANSI code page and "
                                    L"would become '?'.\n\nSave anyway?", APP_NAME,
                            MB_YESNO | MB_ICONWARNING) != IDYES) { free(w); return -1; }
        }
        a = (char *)malloc((size_t)al + 1);
        if (!a) { free(w); return 0; }
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, wl, a, al, NULL, NULL);
        ok = WriteFileAtomic(path, NULL, 0, a, (size_t)al);
        free(a);
        free(w);
        return ok;
    }
    }
}

/* ====================================================================== */
/* Languages and styles (syntax highlighting is data-driven: add a row)    */
/* ====================================================================== */

typedef struct { int style; COLORREF fore; int bold; int italic; } StyleSpec;

typedef struct {
    const wchar_t *name;
    const wchar_t *exts;      /* ";.c;.h;" - must start and end with ';' */
    const char    *lexer;     /* Lexilla lexer name, NULL = plain text   */
    const char    *kw0, *kw1;
    const StyleSpec *styles;  /* terminated by style == -1               */
} LangDef;

enum { LANG_TEXT, LANG_CPP, LANG_PYTHON, LANG_JSON, LANG_XML, LANG_INI, LANG_COUNT };

#define CL_COMMENT RGB(0, 128, 0)
#define CL_KEYWORD RGB(0, 0, 224)
#define CL_TYPE    RGB(128, 0, 160)
#define CL_STRING  RGB(163, 21, 21)
#define CL_NUMBER  RGB(255, 128, 0)
#define CL_PREPROC RGB(128, 64, 0)
#define CL_OPER    RGB(64, 64, 64)
#define CL_ATTR    RGB(220, 0, 0)

static const StyleSpec ST_CPP[] = {
    { SCE_C_COMMENT, CL_COMMENT, 0, 0 }, { SCE_C_COMMENTLINE, CL_COMMENT, 0, 0 },
    { SCE_C_COMMENTDOC, CL_COMMENT, 0, 0 }, { SCE_C_COMMENTLINEDOC, CL_COMMENT, 0, 0 },
    { SCE_C_NUMBER, CL_NUMBER, 0, 0 }, { SCE_C_WORD, CL_KEYWORD, 1, 0 }, { SCE_C_WORD2, CL_TYPE, 1, 0 },
    { SCE_C_STRING, CL_STRING, 0, 0 }, { SCE_C_CHARACTER, CL_STRING, 0, 0 },
    { SCE_C_PREPROCESSOR, CL_PREPROC, 0, 0 }, { SCE_C_OPERATOR, CL_OPER, 1, 0 }, { -1, 0, 0, 0 }
};
static const StyleSpec ST_PY[] = {
    { SCE_P_COMMENTLINE, CL_COMMENT, 0, 0 }, { SCE_P_COMMENTBLOCK, CL_COMMENT, 0, 0 },
    { SCE_P_NUMBER, CL_NUMBER, 0, 0 }, { SCE_P_STRING, CL_STRING, 0, 0 }, { SCE_P_CHARACTER, CL_STRING, 0, 0 },
    { SCE_P_TRIPLE, CL_STRING, 0, 0 }, { SCE_P_TRIPLEDOUBLE, CL_STRING, 0, 0 },
    { SCE_P_WORD, CL_KEYWORD, 1, 0 }, { SCE_P_WORD2, CL_TYPE, 1, 0 },
    { SCE_P_CLASSNAME, CL_TYPE, 1, 0 }, { SCE_P_DEFNAME, CL_TYPE, 1, 0 },
    { SCE_P_DECORATOR, CL_PREPROC, 0, 0 }, { SCE_P_OPERATOR, CL_OPER, 1, 0 }, { -1, 0, 0, 0 }
};
static const StyleSpec ST_JSON[] = {
    { SCE_JSON_NUMBER, CL_NUMBER, 0, 0 }, { SCE_JSON_STRING, CL_STRING, 0, 0 },
    { SCE_JSON_PROPERTYNAME, CL_KEYWORD, 0, 0 }, { SCE_JSON_KEYWORD, CL_TYPE, 1, 0 },
    { SCE_JSON_LINECOMMENT, CL_COMMENT, 0, 0 }, { SCE_JSON_BLOCKCOMMENT, CL_COMMENT, 0, 0 },
    { SCE_JSON_OPERATOR, CL_OPER, 1, 0 }, { -1, 0, 0, 0 }
};
static const StyleSpec ST_XML[] = {
    { SCE_H_TAG, CL_KEYWORD, 0, 0 }, { SCE_H_TAGUNKNOWN, CL_KEYWORD, 0, 0 }, { SCE_H_TAGEND, CL_KEYWORD, 0, 0 },
    { SCE_H_ATTRIBUTE, CL_ATTR, 0, 0 }, { SCE_H_ATTRIBUTEUNKNOWN, CL_ATTR, 0, 0 },
    { SCE_H_NUMBER, CL_NUMBER, 0, 0 }, { SCE_H_DOUBLESTRING, CL_STRING, 0, 0 },
    { SCE_H_SINGLESTRING, CL_STRING, 0, 0 }, { SCE_H_COMMENT, CL_COMMENT, 0, 0 },
    { SCE_H_ENTITY, CL_TYPE, 0, 0 }, { SCE_H_CDATA, CL_PREPROC, 0, 0 },
    { SCE_H_XMLSTART, CL_PREPROC, 0, 0 }, { SCE_H_XMLEND, CL_PREPROC, 0, 0 }, { -1, 0, 0, 0 }
};
static const StyleSpec ST_INI[] = {
    { SCE_PROPS_COMMENT, CL_COMMENT, 0, 0 }, { SCE_PROPS_SECTION, CL_TYPE, 1, 0 },
    { SCE_PROPS_ASSIGNMENT, CL_OPER, 1, 0 }, { SCE_PROPS_KEY, CL_KEYWORD, 0, 0 }, { -1, 0, 0, 0 }
};

static const LangDef LANGS[LANG_COUNT] = {
    { L"Plain text", L";;", NULL, NULL, NULL, NULL },
    { L"C / C++ / Java / JS / C#", L";.c;.h;.cpp;.hpp;.cc;.cxx;.hh;.java;.js;.jsx;.ts;.tsx;.cs;.go;.rs;.swift;.kt;.php;",
      "cpp",
      "alignas auto break case catch class const constexpr continue default delete do else enum explicit "
      "export extern false final finally for friend function goto if import in inline interface let "
      "namespace new nullptr null operator override private protected public register return sizeof "
      "static struct super switch template this throw true try typedef typename union using var "
      "virtual volatile while yield async await fn func package impl mod pub match mut use as",
      "bool byte char double float int long short signed unsigned void wchar_t size_t ssize_t "
      "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t string String object decimal boolean",
      ST_CPP },
    { L"Python", L";.py;.pyw;", "python",
      "False None True and as assert async await break class continue def del elif else except finally "
      "for from global if import in is lambda nonlocal not or pass raise return try while with yield",
      "print len range int str float list dict set tuple bool open self cls super isinstance", ST_PY },
    { L"JSON", L";.json;.jsonc;", "json", "true false null", NULL, ST_JSON },
    { L"XML / HTML", L";.xml;.html;.htm;.xhtml;.svg;.xaml;.csproj;.vcxproj;.config;.plist;", "xml", NULL, NULL, ST_XML },
    { L"INI / config", L";.ini;.cfg;.conf;.properties;.inf;.toml;", "props", NULL, NULL, ST_INI }
};

static int DetectLang(const wchar_t *path)
{
    const wchar_t *e = PathFindExtensionW(path);
    wchar_t pat[40];
    size_t k;
    int i;
    if (!e || !*e || wcslen(e) > 16) return LANG_TEXT;
    pat[0] = L';';
    for (k = 0; e[k]; k++) pat[k + 1] = (wchar_t)towlower(e[k]);
    pat[k + 1] = L';';
    pat[k + 2] = 0;
    for (i = 1; i < LANG_COUNT; i++)
        if (wcsstr(LANGS[i].exts, pat)) return i;
    return LANG_TEXT;
}

static void SetupStyles(int lang)
{
    const StyleSpec *s = LANGS[lang].styles;
    int i;
    SS(SCI_STYLESETFONT, STYLE_DEFAULT, "Consolas");
    SS(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    SS(SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0, 0, 0));
    SS(SCI_STYLESETBACK, STYLE_DEFAULT, RGB(255, 255, 255));
    SS(SCI_STYLECLEARALL, 0, 0);
    SS(SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(120, 120, 120));
    SS(SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(238, 238, 238));
    SS(SCI_STYLESETFORE, STYLE_BRACELIGHT, RGB(0, 0, 0));
    SS(SCI_STYLESETBACK, STYLE_BRACELIGHT, RGB(150, 220, 150));
    SS(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
    SS(SCI_STYLESETFORE, STYLE_BRACEBAD, RGB(255, 255, 255));
    SS(SCI_STYLESETBACK, STYLE_BRACEBAD, RGB(220, 60, 60));
    SS(SCI_STYLESETFORE, STYLE_INDENTGUIDE, RGB(200, 200, 200));
    for (i = 0; s && s[i].style >= 0; i++) {
        SS(SCI_STYLESETFORE, s[i].style, s[i].fore);
        if (s[i].bold) SS(SCI_STYLESETBOLD, s[i].style, 1);
        if (s[i].italic) SS(SCI_STYLESETITALIC, s[i].style, 1);
    }
}

static void SciProp(const char *k, const char *v) { SS(SCI_SETPROPERTY, k, v); }

/* Attach a lexer to the current document and restyle it. */
static void ApplyLanguage(Doc *d, int lang)
{
    const LangDef *L;
    void *lx;
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_TEXT;
    if ((unsigned long long)SS(SCI_GETLENGTH, 0, 0) > LEXER_LIMIT) lang = LANG_TEXT;   /* keep huge files fast */
    d->lang = lang;
    L = &LANGS[lang];
    lx = (L->lexer && g_createLexer) ? g_createLexer(L->lexer) : NULL;
    SS(SCI_SETILEXER, 0, lx);
    if (lx) {
        SciProp("fold", "1");
        SciProp("fold.compact", "0");
        SciProp("fold.comment", "1");
        SciProp("fold.preprocessor", "1");
        SciProp("fold.html", "1");
        SciProp("fold.comment.python", "1");
        SciProp("lexer.json.allow.comments", "1");
        if (L->kw0) SS(SCI_SETKEYWORDS, 0, L->kw0);
        if (L->kw1) SS(SCI_SETKEYWORDS, 1, L->kw1);
    }
    SetupStyles(lang);
    if (lx) SS(SCI_COLOURISE, 0, -1);
    UpdateStatus();
}

/* ====================================================================== */
/* Scintilla view setup                                                    */
/* ====================================================================== */

static void DefFold(int marker, int shape)
{
    SS(SCI_MARKERDEFINE, marker, shape);
    SS(SCI_MARKERSETFORE, marker, RGB(255, 255, 255));
    SS(SCI_MARKERSETBACK, marker, RGB(110, 110, 110));
}

static void SetupView(void)
{
    SS(SCI_SETCODEPAGE, SC_CP_UTF8, 0);

    /* multi-cursor and column mode: Ctrl+click adds a caret, Alt+drag (or Alt+Shift+arrows) = column */
    SS(SCI_SETMULTIPLESELECTION, 1, 0);
    SS(SCI_SETADDITIONALSELECTIONTYPING, 1, 0);
    SS(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH, 0);
    SS(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION, 0);
    SS(SCI_SETRECTANGULARSELECTIONMODIFIER, SCMOD_ALT, 0);

    SS(SCI_SETTABWIDTH, 4, 0);
    SS(SCI_SETUSETABS, 1, 0);
    SS(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH, 0);
    SS(SCI_SETCARETLINEVISIBLE, 1, 0);
    SS(SCI_SETCARETLINEBACK, RGB(232, 242, 254), 0);
    SS(SCI_SETSELBACK, 1, RGB(173, 214, 255));

    /* margins: 0 = line numbers, 2 = fold markers */
    SS(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    SS(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    SS(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    SS(SCI_SETMARGINWIDTHN, 2, 16);
    SS(SCI_SETMARGINSENSITIVEN, 2, 1);
    DefFold(SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    DefFold(SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    DefFold(SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    DefFold(SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    DefFold(SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    DefFold(SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    DefFold(SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);
    SS(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE, 0);
    SS(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED, 0);

    SS(SCI_AUTOCSETIGNORECASE, 1, 0);
    SS(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT, 0);
    SS(SCI_AUTOCSETMAXHEIGHT, 10, 0);

    SS(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);
}

static void UpdateLineMargin(void)
{
    sptr_t lines = SS(SCI_GETLINECOUNT, 0, 0), t;
    int digits = 4, w;
    char probe[24];
    for (t = lines; t >= 10000; t /= 10) digits++;
    if (digits > 20) digits = 20;
    memset(probe, '9', (size_t)digits);
    probe[digits] = 0;
    w = (int)SS(SCI_TEXTWIDTH, STYLE_LINENUMBER, probe) + 10;
    if (SS(SCI_GETMARGINWIDTHN, 0, 0) != w) SS(SCI_SETMARGINWIDTHN, 0, w);
}

/* ====================================================================== */
/* Document list and tabs                                                  */
/* ====================================================================== */

static Doc *Cur(void) { return (g_cur >= 0 && g_cur < g_nDocs) ? g_docs[g_cur] : NULL; }

static int DocIndex(const Doc *d)
{
    int i;
    for (i = 0; i < g_nDocs; i++) if (g_docs[i] == d) return i;
    return -1;
}

static void DocName(const Doc *d, wchar_t *out, size_t cap)
{
    if (d->path[0]) CopyW(out, cap, PathFindFileNameW(d->path));
    else { _snwprintf(out, cap, L"new %d", d->untitledNo); out[cap - 1] = 0; }
}

static void UpdateTab(int i)
{
    wchar_t t[PATH_CAP + 4];
    TCITEMW ti;
    if (i < 0 || i >= g_nDocs) return;
    DocName(g_docs[i], t, PATH_CAP);
    if (g_docs[i]->dirty) wcsncat(t, L" *", (sizeof(t) / sizeof(t[0])) - wcslen(t) - 1);
    memset(&ti, 0, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = t;
    TabCtrl_SetItem(g_tab, i, &ti);
}

static void UpdateTitle(void)
{
    Doc *d = Cur();
    wchar_t t[PATH_CAP + 64], nm[PATH_CAP];
    if (!d) { SetWindowTextW(g_main, APP_NAME); return; }
    DocName(d, nm, PATH_CAP);
    SNW(t, L"%ls%ls - %ls", d->path[0] ? d->path : nm, d->dirty ? L" *" : L"", APP_NAME);
    SetWindowTextW(g_main, t);
}

static void UpdateStatus(void)
{
    static const wchar_t *EOLN[3] = { L"Windows (CRLF)", L"Mac (CR)", L"Unix (LF)" };
    static const wchar_t *ENCN[5] = { L"UTF-8", L"UTF-8 BOM", L"UTF-16 LE", L"UTF-16 BE", L"ANSI" };
    Doc *d = Cur();
    sptr_t pos, s, e;
    int nsel, eol;
    wchar_t b[128];
    if (!d || !g_status) return;
    pos = SS(SCI_GETCURRENTPOS, 0, 0);
    s = SS(SCI_GETSELECTIONSTART, 0, 0);
    e = SS(SCI_GETSELECTIONEND, 0, 0);
    nsel = (int)SS(SCI_GETSELECTIONS, 0, 0);
    if (nsel > 1)
        SNW(b, L"Ln %ld  Col %ld  (%d carets)", (long)(SS(SCI_LINEFROMPOSITION, pos, 0) + 1),
            (long)(SS(SCI_GETCOLUMN, pos, 0) + 1), nsel);
    else
        SNW(b, L"Ln %ld  Col %ld  Sel %ld", (long)(SS(SCI_LINEFROMPOSITION, pos, 0) + 1),
            (long)(SS(SCI_GETCOLUMN, pos, 0) + 1), (long)(e - s));
    StatusSet(1, b);
    StatusSet(2, LANGS[d->lang].name);
    eol = (int)SS(SCI_GETEOLMODE, 0, 0);
    StatusSet(3, EOLN[(eol >= 0 && eol <= 2) ? eol : 0]);
    StatusSet(4, ENCN[(d->enc >= 0 && d->enc <= 4) ? d->enc : 0]);
}

static Doc *DocNew(ULONGLONG sizeHint)
{
    Doc *d = (Doc *)xalloc(sizeof *d);
    TCITEMW ti;
    int idx, opt = sizeHint > (1ULL << 30) ? SC_DOCUMENTOPTION_TEXT_LARGE : SC_DOCUMENTOPTION_DEFAULT;
    memset(d, 0, sizeof *d);
    d->ptr = SS(SCI_CREATEDOCUMENT, sizeHint < (512ULL << 20) ? (size_t)sizeHint : 0, opt);
    d->uid = g_nextUid++;
    if (g_nDocs == g_capDocs) {
        g_capDocs = g_capDocs ? g_capDocs * 2 : 8;
        g_docs = (Doc **)xrealloc(g_docs, (size_t)g_capDocs * sizeof *g_docs);
    }
    idx = g_nDocs;
    g_docs[g_nDocs++] = d;
    memset(&ti, 0, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"...";
    TabCtrl_InsertItem(g_tab, idx, &ti);
    return d;
}

static void SwitchTo(int idx)
{
    Doc *old, *d;
    if (idx < 0 || idx >= g_nDocs) return;
    old = Cur();
    if (old && g_cur != idx) {
        old->caret = SS(SCI_GETCURRENTPOS, 0, 0);
        old->topLine = SS(SCI_GETFIRSTVISIBLELINE, 0, 0);
        FlushBackup(old);
    }
    g_cur = idx;
    d = g_docs[idx];
    SS(SCI_SETDOCPOINTER, 0, d->ptr);
    SetupStyles(d->lang);
    SS(SCI_SETSEL, d->caret, d->caret);
    SS(SCI_SETFIRSTVISIBLELINE, d->topLine, 0);
    TabCtrl_SetCurSel(g_tab, idx);
    UpdateLineMargin();
    UpdateTitle();
    UpdateStatus();
    SetFocus(g_sci);
    g_sessionDirty = 1;
}

static Doc *NewUntitled(void)
{
    Doc *d = DocNew(0);
    d->untitledNo = g_nextUntitled++;
    SwitchTo(DocIndex(d));
    ApplyLanguage(d, LANG_TEXT);
    UpdateTab(DocIndex(d));
    UpdateTitle();
    return d;
}

/* ====================================================================== */
/* Backups and session (unsaved tabs survive restarts and crashes)         */
/* ====================================================================== */

static void BackupPath(const Doc *d, wchar_t *out, size_t cap)
{
    _snwprintf(out, cap, L"%ls\\backup\\%d.txt", g_dataDir, d->uid);
    out[cap - 1] = 0;
}

static void DeleteBackup(const Doc *d)
{
    wchar_t p[PATH_CAP];
    BackupPath(d, p, PATH_CAP);
    DeleteFileW(p);
}

/* d must be the document currently shown in the view. */
static void FlushBackup(Doc *d)
{
    wchar_t p[PATH_CAP];
    const char *t;
    size_t n;
    if (!d || !d->dirty || !d->needsBackup) return;
    BackupPath(d, p, PATH_CAP);
    t = (const char *)SS(SCI_GETCHARACTERPOINTER, 0, 0);
    n = (size_t)SS(SCI_GETLENGTH, 0, 0);
    if (WriteFileAtomic(p, NULL, 0, t, n)) d->needsBackup = 0;
}

static void PruneBackups(void)
{
    wchar_t pat[PATH_CAP], full[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SNW(pat, L"%ls\\backup\\*.txt", g_dataDir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        int uid = _wtoi(fd.cFileName), i, used = 0;
        for (i = 0; i < g_nDocs; i++) if (g_docs[i]->uid == uid && g_docs[i]->dirty) used = 1;
        if (!used) {
            SNW(full, L"%ls\\backup\\%ls", g_dataDir, fd.cFileName);
            DeleteFileW(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void SaveSession(void)
{
    wchar_t sp[PATH_CAP];
    size_t cap, o = 0;
    char *b;
    int i, written = 0, active = 0;
    Doc *cur = Cur();
    if (!g_nDocs || !cur) return;
    FlushBackup(cur);
    cur->caret = SS(SCI_GETCURRENTPOS, 0, 0);
    cur->topLine = SS(SCI_GETFIRSTVISIBLELINE, 0, 0);
    cap = 4096 + (size_t)g_nDocs * (PATH_CAP * 3 + 128);
    b = (char *)xalloc(cap);
    o += (size_t)snprintf(b + o, cap - o, "MININPP-SESSION 1\n");
    for (i = 0; i < g_nDocs; i++) {
        Doc *d = g_docs[i];
        char *p;
        if (!d->path[0] && !d->dirty) continue;        /* empty untitled tab: nothing to keep */
        if (i == g_cur) active = written;
        p = WideToUtf8(d->path, NULL);
        o += (size_t)snprintf(b + o, cap - o, "tab|%d|%d|%ld|%ld|%d|%d|%s\n", d->enc, d->lang,
                              (long)d->caret, (long)d->topLine, d->dirty, d->uid, p);
        free(p);
        written++;
    }
    o += (size_t)snprintf(b + o, cap - o, "active=%d\n", active);
    SNW(sp, L"%ls\\session.txt", g_dataDir);
    WriteFileAtomic(sp, NULL, 0, b, o);
    free(b);
    PruneBackups();
}

static int LoadIntoCurrent(Doc *d, const wchar_t *path);

static int RestoreSession(void)
{
    wchar_t sp[PATH_CAP];
    unsigned char *raw;
    size_t n;
    char *text, *line;
    Doc *list[256];
    int nlist = 0, active = 0, i, restored = 0;

    SNW(sp, L"%ls\\session.txt", g_dataDir);
    if (!ReadWholeFile(sp, 8u << 20, &raw, &n)) return 0;
    text = (char *)raw;
    if (strncmp(text, "MININPP-SESSION 1", 17) != 0) { free(raw); return 0; }

    for (line = text; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (nl && nl > line && nl[-1] == '\r') nl[-1] = 0;

        if (!strncmp(line, "active=", 7)) {
            active = atoi(line + 7);
        } else if (!strncmp(line, "tab|", 4) && nlist < 256) {
            int enc = 0, lang = 0, dirty = 0, uid = 0, k;
            long caret = 0, top = 0;
            const char *p = line;
            wchar_t *wpath;
            Doc *d = NULL;
            sscanf(line, "tab|%d|%d|%ld|%ld|%d|%d|", &enc, &lang, &caret, &top, &dirty, &uid);
            for (k = 0; k < 7 && p; k++) { p = strchr(p, '|'); if (p) p++; }
            wpath = Utf8ToWide(p ? p : "", -1);

            if (dirty) {
                wchar_t bp[PATH_CAP];
                SNW(bp, L"%ls\\backup\\%d.txt", g_dataDir, uid);
                if (GetFileAttributesW(bp) != INVALID_FILE_ATTRIBUTES) {
                    d = DocNew(0);
                    SwitchTo(DocIndex(d));
                    if (LoadIntoCurrent(d, bp)) {
                        d->enc = (enc >= 0 && enc <= 4) ? enc : TU_ENC_UTF8;
                        if (wpath[0]) CopyW(d->path, PATH_CAP, wpath);
                        else d->untitledNo = g_nextUntitled++;
                        ApplyLanguage(d, lang);
                        d->dirty = 1;
                        d->touched = 1;
                        d->needsBackup = 1;
                        UpdateTab(DocIndex(d));
                        DeleteFileW(bp);
                    } else {
                        CloseDoc(DocIndex(d));
                        d = NULL;
                    }
                }
            } else if (wpath[0] && GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
                d = OpenDocument(wpath);
                if (d && lang != d->lang && lang >= 0 && lang < LANG_COUNT) ApplyLanguage(d, lang);
            }
            if (d && (caret || top)) {
                SS(SCI_SETSEL, caret, caret);
                SS(SCI_SETFIRSTVISIBLELINE, top, 0);
            }
            if (d) restored++;
            list[nlist++] = d;
            free(wpath);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(raw);

    if (restored) {
        Doc *target = (active >= 0 && active < nlist) ? list[active] : NULL;
        for (i = nlist - 1; i >= 0 && !target; i--) target = list[i];
        if (target) SwitchTo(DocIndex(target));
    }
    return restored;
}

/* ====================================================================== */
/* Loading, saving, opening, closing                                       */
/* ====================================================================== */

/* Reads <path> into the CURRENT Scintilla document. Large UTF-8 files are
 * streamed in 1 MB chunks, so even multi-gigabyte logs open without a huge
 * second copy in memory. */
static int LoadIntoCurrent(Doc *d, const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz, pos;
    unsigned char *sample;
    DWORD got = 0;
    size_t bom = 0;
    TuEncoding enc;
    int ok = 1;

    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    sample = (unsigned char *)malloc(65536);
    if (!sample) { CloseHandle(h); return 0; }
    ReadFile(h, sample, 65536, &got, NULL);
    enc = tu_detect_encoding(sample, got, (unsigned long long)sz.QuadPart > got, &bom);
    free(sample);

    SS(SCI_SETUNDOCOLLECTION, 0, 0);
    SS(SCI_CLEARALL, 0, 0);
    pos.QuadPart = (LONGLONG)bom;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);

    if (enc == TU_ENC_UTF8 || enc == TU_ENC_UTF8_BOM) {
        char *buf = (char *)malloc(CHUNK);
        DWORD n = 0;
        int first = 1;
        if (!buf) ok = 0;
        while (ok && ReadFile(h, buf, CHUNK, &n, NULL) && n) {
            if (first) { SS(SCI_SETEOLMODE, tu_detect_eol(buf, n), 0); first = 0; }
            SS(SCI_APPENDTEXT, n, buf);
        }
        free(buf);
    } else if ((unsigned long long)sz.QuadPart > CONVERT_LIMIT) {
        ok = 0;
    } else {
        size_t total = (size_t)sz.QuadPart - bom, have = 0, u8len = 0, off;
        unsigned char *raw = (unsigned char *)malloc(total + 1);
        char *u8 = NULL;
        if (!raw) ok = 0;
        while (ok && have < total) {
            DWORD want = (DWORD)((total - have) > CHUNK ? CHUNK : (total - have)), r = 0;
            if (!ReadFile(h, raw + have, want, &r, NULL) || !r) break;
            have += r;
        }
        if (ok) u8 = ToUtf8(raw, have, enc, &u8len);
        if (ok && !u8) ok = 0;
        if (ok) {
            SS(SCI_SETEOLMODE, tu_detect_eol(u8, u8len), 0);
            for (off = 0; off < u8len; off += CHUNK)
                SS(SCI_APPENDTEXT, (u8len - off) > CHUNK ? CHUNK : (u8len - off), u8 + off);
        }
        free(u8);
        free(raw);
    }
    CloseHandle(h);

    SS(SCI_SETUNDOCOLLECTION, 1, 0);
    SS(SCI_EMPTYUNDOBUFFER, 0, 0);
    SS(SCI_SETSAVEPOINT, 0, 0);
    SS(SCI_GOTOPOS, 0, 0);
    d->enc = (int)enc;
    d->monSize = (ULONGLONG)sz.QuadPart;
    d->dirty = 0;
    return ok;
}

/* Writes the current document to <path> in its own encoding. 1 ok, 0 failed, -1 cancelled. */
static int SaveCurrentTo(Doc *d, const wchar_t *path)
{
    size_t n = (size_t)SS(SCI_GETLENGTH, 0, 0);
    const char *t = (const char *)SS(SCI_GETCHARACTERPOINTER, 0, 0);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int rc = WriteEncoded(path, t, n, (TuEncoding)d->enc, 1);
    if (rc > 0) {
        SS(SCI_SETSAVEPOINT, 0, 0);
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa))
            d->monSize = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    return rc;
}

static int SaveDoc(Doc *d, int saveAs)
{
    wchar_t path[PATH_CAP];
    int rc;
    if (d != Cur()) SwitchTo(DocIndex(d));
    if (!saveAs && d->path[0]) {
        CopyW(path, PATH_CAP, d->path);
    } else {
        OPENFILENAMEW o;
        wchar_t nm[PATH_CAP];
        DocName(d, nm, PATH_CAP);
        CopyW(path, PATH_CAP, d->path[0] ? d->path : nm);
        memset(&o, 0, sizeof o);
        o.lStructSize = sizeof o;
        o.hwndOwner = g_main;
        o.lpstrFilter = L"All files (*.*)\0*.*\0";
        o.lpstrFile = path;
        o.nMaxFile = PATH_CAP;
        o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&o)) return 0;
    }
    rc = SaveCurrentTo(d, path);
    if (rc < 0) return 0;
    if (rc == 0) { MessageBoxW(g_main, L"Could not save the file.", APP_NAME, MB_ICONERROR); return 0; }
    CopyW(d->path, PATH_CAP, path);
    d->dirty = 0;
    d->needsBackup = 0;
    DeleteBackup(d);
    if (saveAs) ApplyLanguage(d, DetectLang(path));
    UpdateTab(DocIndex(d));
    UpdateTitle();
    UpdateStatus();
    g_sessionDirty = 1;
    return 1;
}

static int IsPristine(const Doc *d) { return !d->path[0] && !d->dirty && !d->touched; }

static Doc *OpenDocument(const wchar_t *p)
{
    wchar_t full[PATH_CAP];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    ULONGLONG size;
    Doc *pristine, *d;
    int i;

    if (!GetFullPathNameW(p, PATH_CAP, full, NULL)) CopyW(full, PATH_CAP, p);
    for (i = 0; i < g_nDocs; i++)
        if (!_wcsicmp(g_docs[i]->path, full)) { SwitchTo(i); return g_docs[i]; }
    if (!GetFileAttributesExW(full, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t m[PATH_CAP + 64];
        SNW(m, L"Cannot open \"%ls\".", full);
        MessageBoxW(g_main, m, APP_NAME, MB_ICONERROR);
        return NULL;
    }
    size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    pristine = (g_nDocs == 1 && IsPristine(g_docs[0])) ? g_docs[0] : NULL;

    d = DocNew(size);
    SwitchTo(DocIndex(d));
    if (!LoadIntoCurrent(d, full)) {
        wchar_t m[PATH_CAP + 64];
        SNW(m, L"Could not read \"%ls\" (too large to convert, or in use).", full);
        MessageBoxW(g_main, m, APP_NAME, MB_ICONERROR);
        CloseDoc(DocIndex(d));
        return NULL;
    }
    CopyW(d->path, PATH_CAP, full);
    ApplyLanguage(d, DetectLang(full));
    d->touched = 0;
    d->needsBackup = 0;
    UpdateTab(DocIndex(d));
    UpdateTitle();
    UpdateLineMargin();
    UpdateStatus();
    if (pristine) CloseDoc(DocIndex(pristine));        /* replace the empty "new 1" tab */
    return d;
}

static void OpenDialog(void)
{
    wchar_t *buf = (wchar_t *)xalloc(32768 * sizeof(wchar_t));
    OPENFILENAMEW o;
    buf[0] = 0;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_main;
    o.lpstrFilter = L"All files (*.*)\0*.*\0";
    o.lpstrFile = buf;
    o.nMaxFile = 32768;
    o.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) {
        const wchar_t *dir = buf, *f = dir + wcslen(dir) + 1;
        if (!*f) OpenDocument(dir);                         /* single file: buf holds the full path */
        else for (; *f; f += wcslen(f) + 1) {
            wchar_t full[PATH_CAP];
            SNW(full, L"%ls\\%ls", dir, f);
            OpenDocument(full);
        }
    }
    free(buf);
}

static int CloseDoc(int idx)
{
    Doc *d;
    if (idx < 0 || idx >= g_nDocs) return 0;
    if (idx != g_cur) SwitchTo(idx);
    d = g_docs[idx];
    if (d->dirty) {
        wchar_t nm[PATH_CAP], msg[PATH_CAP + 64];
        int r;
        DocName(d, nm, PATH_CAP);
        SNW(msg, L"Save changes to \"%ls\"?", nm);
        r = MessageBoxW(g_main, msg, APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !SaveDoc(d, 0)) return 0;
    }
    d->needsBackup = 0;
    if (g_nDocs == 1) NewUntitled();                   /* always keep one tab */
    else SwitchTo(idx > 0 ? idx - 1 : idx + 1);
    DeleteBackup(d);
    idx = DocIndex(d);
    g_nDocs--;
    memmove(&g_docs[idx], &g_docs[idx + 1], (size_t)(g_nDocs - idx) * sizeof *g_docs);
    TabCtrl_DeleteItem(g_tab, idx);
    if (g_cur > idx) g_cur--;
    TabCtrl_SetCurSel(g_tab, g_cur);
    SS(SCI_RELEASEDOCUMENT, 0, d->ptr);
    free(d);
    g_sessionDirty = 1;
    UpdateTitle();
    return 1;
}

/* "tail -f": append whatever was added to the file since we last looked. */
static void CheckMonitor(Doc *d)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    ULONGLONG size;
    if (!d || d != Cur() || !d->monitor || !d->path[0] || d->dirty) return;
    if (!GetFileAttributesExW(d->path, GetFileExInfoStandard, &fa)) return;
    size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size == d->monSize) return;
    if (size < d->monSize) {                           /* truncated or rotated: start over */
        LoadIntoCurrent(d, d->path);
    } else {
        HANDLE h = CreateFileW(d->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER pos;
        size_t want, have = 0;
        unsigned char *raw;
        if (h == INVALID_HANDLE_VALUE) return;
        pos.QuadPart = (LONGLONG)d->monSize;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
        want = (size_t)((size - d->monSize) > (64u << 20) ? (64u << 20) : (size - d->monSize));
        raw = (unsigned char *)malloc(want + 1);
        if (raw) {
            while (have < want) {
                DWORD r = 0, w = (DWORD)((want - have) > CHUNK ? CHUNK : (want - have));
                if (!ReadFile(h, raw + have, w, &r, NULL) || !r) break;
                have += r;
            }
            if (have) {
                size_t u8len = 0;
                char *u8 = ToUtf8(raw, have, d->enc == TU_ENC_ANSI ? TU_ENC_ANSI : TU_ENC_UTF8, &u8len);
                if (u8) {
                    SS(SCI_SETUNDOCOLLECTION, 0, 0);
                    SS(SCI_APPENDTEXT, u8len, u8);
                    SS(SCI_SETUNDOCOLLECTION, 1, 0);
                    free(u8);
                }
            }
            d->monSize += have;
            free(raw);
        }
        CloseHandle(h);
    }
    d->monSize = size;
    SS(SCI_GOTOPOS, SS(SCI_GETLENGTH, 0, 0), 0);
}

/* ====================================================================== */
/* Editing features: auto-complete, brace matching, line tools, columns    */
/* ====================================================================== */

static int IsWordCh(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

static int ListHas(const char *list, const char *w)
{
    size_t n = strlen(w);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !_strnicmp(p, w, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Word completion from the words already in the document. */
static void AutoComplete(void)
{
    sptr_t pos, ws, plen, len, i = 0;
    char prefix[64], *list;
    const char *t;
    size_t lo = 0;
    int count = 0;
    if (SS(SCI_AUTOCACTIVE, 0, 0) || SS(SCI_GETSELECTIONS, 0, 0) > 1) return;
    pos = SS(SCI_GETCURRENTPOS, 0, 0);
    ws = SS(SCI_WORDSTARTPOSITION, pos, 1);
    plen = pos - ws;
    len = SS(SCI_GETLENGTH, 0, 0);
    if (plen < 3 || plen > 48 || len > (4 << 20)) return;
    memcpy(prefix, (const char *)SS(SCI_GETRANGEPOINTER, ws, plen), (size_t)plen);
    prefix[plen] = 0;
    t = (const char *)SS(SCI_GETCHARACTERPOINTER, 0, 0);
    list = (char *)xalloc(16384);
    list[0] = 0;
    while (i < len && count < 60) {
        sptr_t s, wl;
        char word[64];
        if (!IsWordCh((unsigned char)t[i])) { i++; continue; }
        s = i;
        while (i < len && IsWordCh((unsigned char)t[i])) i++;
        wl = i - s;
        if (wl <= plen || wl > 60 || s == ws) continue;
        if (_strnicmp(t + s, prefix, (size_t)plen) != 0) continue;
        memcpy(word, t + s, (size_t)wl);
        word[wl] = 0;
        if (ListHas(list, word)) continue;
        if (lo + (size_t)wl + 2 > 16384) break;
        if (lo) list[lo++] = ' ';
        memcpy(list + lo, word, (size_t)wl);
        lo += (size_t)wl;
        list[lo] = 0;
        count++;
    }
    if (count) SS(SCI_AUTOCSHOW, plen, list);
    free(list);
}

static void OnCharAdded(int ch)
{
    int eol = (int)SS(SCI_GETEOLMODE, 0, 0);
    if ((ch == '\n' && eol != SC_EOL_CR) || (ch == '\r' && eol == SC_EOL_CR)) {   /* auto-indent */
        if (SS(SCI_GETSELECTIONS, 0, 0) == 1) {
            sptr_t line = SS(SCI_LINEFROMPOSITION, SS(SCI_GETCURRENTPOS, 0, 0), 0);
            if (line > 0) {
                sptr_t ind = SS(SCI_GETLINEINDENTATION, line - 1, 0);
                if (ind > 0) {
                    SS(SCI_SETLINEINDENTATION, line, ind);
                    SS(SCI_GOTOPOS, SS(SCI_GETLINEINDENTPOSITION, line, 0), 0);
                }
            }
        }
        return;
    }
    if (ch > 0 && ch < 128 && IsWordCh((unsigned char)ch)) AutoComplete();
}

static void MatchBraces(void)
{
    sptr_t pos = SS(SCI_GETCURRENTPOS, 0, 0), at = -1, m;
    int c = (int)SS(SCI_GETCHARAT, pos, 0);
    if (c > 0 && strchr("()[]{}", c)) at = pos;
    else if (pos > 0) {
        c = (int)SS(SCI_GETCHARAT, pos - 1, 0);
        if (c > 0 && strchr("()[]{}", c)) at = pos - 1;
    }
    if (at >= 0) {
        m = SS(SCI_BRACEMATCH, at, 0);
        if (m >= 0) SS(SCI_BRACEHIGHLIGHT, at, m);
        else SS(SCI_BRACEBADLIGHT, at, 0);
    } else {
        SS(SCI_BRACEHIGHLIGHT, INVALID_POSITION, INVALID_POSITION);
    }
}

/* Sort / de-duplicate / remove blank / trim: works on the selected lines, or the whole file. */
static void LineTool(int cmd)
{
    sptr_t selS = SS(SCI_GETSELECTIONSTART, 0, 0), selE = SS(SCI_GETSELECTIONEND, 0, 0), start, end;
    const char *src, *eol = tu_eol_string((int)SS(SCI_GETEOLMODE, 0, 0));
    size_t outLen = 0;
    char *res = NULL;
    if (selS == selE) {
        start = 0;
        end = SS(SCI_GETLENGTH, 0, 0);
    } else {
        sptr_t l1 = SS(SCI_LINEFROMPOSITION, selS, 0), l2 = SS(SCI_LINEFROMPOSITION, selE, 0);
        if (l2 > l1 && SS(SCI_POSITIONFROMLINE, l2, 0) == selE) l2--;
        start = SS(SCI_POSITIONFROMLINE, l1, 0);
        end = SS(SCI_GETLINEENDPOSITION, l2, 0);
    }
    if (end <= start) return;
    src = (const char *)SS(SCI_GETRANGEPOINTER, start, end - start);
    switch (cmd) {
    case IDM_LINE_SORT_ASC:  res = tu_sort_lines(src, (size_t)(end - start), eol, 0, &outLen); break;
    case IDM_LINE_SORT_DESC: res = tu_sort_lines(src, (size_t)(end - start), eol, 1, &outLen); break;
    case IDM_LINE_UNIQUE:    res = tu_unique_lines(src, (size_t)(end - start), eol, &outLen); break;
    case IDM_LINE_NOEMPTY:   res = tu_remove_empty_lines(src, (size_t)(end - start), eol, &outLen); break;
    case IDM_LINE_TRIM:      res = tu_trim_trailing(src, (size_t)(end - start), eol, &outLen); break;
    }
    if (!res) return;
    SS(SCI_BEGINUNDOACTION, 0, 0);
    SS(SCI_SETTARGETRANGE, start, end);
    SS(SCI_REPLACETARGET, outLen, res);
    SS(SCI_ENDUNDOACTION, 0, 0);
    free(res);
}

typedef struct { sptr_t s, e; } SelRange;
static int CmpSel(const void *a, const void *b)
{
    const SelRange *x = (const SelRange *)a, *y = (const SelRange *)b;
    return x->s < y->s ? -1 : x->s > y->s ? 1 : 0;
}

static BOOL PromptText(HWND parent, const wchar_t *title, const wchar_t *label, wchar_t *buf, int cap);

/* Column editor: replace every selection/caret with 1, 2, 3, ... (top to bottom). */
static void ColumnNumbers(void)
{
    wchar_t buf[64] = L"1,1", startStr[64];
    wchar_t *comma;
    int n, i, start, step = 1, width = 0, value;
    SelRange *r;
    if (!PromptText(g_main, L"Insert numbers", L"Start,step  (a leading 0 pads: 001,1)", buf, 64)) return;
    comma = wcschr(buf, L',');
    if (comma) { step = _wtoi(comma + 1); *comma = 0; }
    CopyW(startStr, 64, buf);
    start = _wtoi(startStr);
    if (startStr[0] == L'0' && wcslen(startStr) > 1) width = (int)wcslen(startStr);

    n = (int)SS(SCI_GETSELECTIONS, 0, 0);
    r = (SelRange *)xalloc((size_t)n * sizeof *r);
    for (i = 0; i < n; i++) {
        r[i].s = SS(SCI_GETSELECTIONNSTART, i, 0);
        r[i].e = SS(SCI_GETSELECTIONNEND, i, 0);
    }
    qsort(r, (size_t)n, sizeof *r, CmpSel);
    SS(SCI_BEGINUNDOACTION, 0, 0);
    for (i = n - 1; i >= 0; i--) {                     /* back to front so earlier positions stay valid */
        char tmp[32];
        value = start + step * i;
        snprintf(tmp, sizeof tmp, "%0*d", width, value);
        SS(SCI_SETTARGETRANGE, r[i].s, r[i].e);
        SS(SCI_REPLACETARGET, strlen(tmp), tmp);
    }
    SS(SCI_ENDUNDOACTION, 0, 0);
    free(r);
}

/* ====================================================================== */
/* Macros (record / play / run many times)                                 */
/* ====================================================================== */

typedef struct { UINT msg; WPARAM w; LPARAM l; char *text; } MacroStep;
static MacroStep *g_macro;
static int g_nMacro, g_capMacro, g_recording;

static void MacroClear(void)
{
    int i;
    for (i = 0; i < g_nMacro; i++) free(g_macro[i].text);
    g_nMacro = 0;
}

static void MacroRecord(UINT msg, WPARAM w, LPARAM l)
{
    MacroStep *s;
    if (!g_recording) return;
    if (g_nMacro == g_capMacro) {
        g_capMacro = g_capMacro ? g_capMacro * 2 : 256;
        g_macro = (MacroStep *)xrealloc(g_macro, (size_t)g_capMacro * sizeof *g_macro);
    }
    s = &g_macro[g_nMacro++];
    s->msg = msg; s->w = w; s->l = l; s->text = NULL;
    switch (msg) {
    case SCI_ADDTEXT:
    case SCI_APPENDTEXT:                               /* text + explicit length */
        s->text = (char *)xalloc((size_t)w + 1);
        memcpy(s->text, (const char *)l, (size_t)w);
        s->text[w] = 0;
        break;
    case SCI_REPLACESEL:
    case SCI_INSERTTEXT:
    case SCI_SEARCHNEXT:
    case SCI_SEARCHPREV:                               /* NUL-terminated text */
        s->text = StrDupUtf8((const char *)l);
        break;
    }
}

static void MacroPlayOnce(void)
{
    int i;
    for (i = 0; i < g_nMacro; i++)
        SS(g_macro[i].msg, g_macro[i].w, g_macro[i].text ? (LPARAM)g_macro[i].text : g_macro[i].l);
}

static void MacroPlay(int times, int toEnd)
{
    if (!g_nMacro || g_recording) return;
    SS(SCI_BEGINUNDOACTION, 0, 0);
    if (toEnd) {
        long guard = 0;
        for (;;) {                                     /* repeat until the caret stops moving or hits the end */
            sptr_t before = SS(SCI_GETCURRENTPOS, 0, 0), after;
            MacroPlayOnce();
            after = SS(SCI_GETCURRENTPOS, 0, 0);
            if (after <= before || after >= SS(SCI_GETLENGTH, 0, 0) || ++guard > 1000000L) break;
        }
    } else {
        int k;
        for (k = 0; k < times; k++) MacroPlayOnce();
    }
    SS(SCI_ENDUNDOACTION, 0, 0);
}

static void MacroToggle(void)
{
    if (!g_recording) {
        MacroClear();
        g_recording = 1;
        SS(SCI_STARTRECORD, 0, 0);
        StatusSet(0, L"Recording macro...  (Ctrl+Shift+R to stop)");
    } else {
        SS(SCI_STOPRECORD, 0, 0);
        g_recording = 0;
        StatusSet(0, L"Macro recorded.  Ctrl+Shift+P plays it.");
    }
}

/* ====================================================================== */
/* Prompt dialog (one text field), built by hand so no .rc file is needed  */
/* ====================================================================== */

typedef struct { wchar_t *buf; int cap; const wchar_t *label; int done, ok; HWND edit; } PromptData;

static HWND MkCtl(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                  int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, g_hInst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    PromptData *pd = (PromptData *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE:
        pd = (PromptData *)((CREATESTRUCTW *)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)pd);
        MkCtl(h, L"STATIC", pd->label, 0, 0, 10, 10, 310, 20, -1);
        pd->edit = MkCtl(h, L"EDIT", pd->buf, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 10, 34, 310, 22, 300);
        MkCtl(h, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 150, 68, 80, 24, IDOK);
        MkCtl(h, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 240, 68, 80, 24, IDCANCEL);
        SendMessageW(pd->edit, EM_SETSEL, 0, -1);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { GetWindowTextW(pd->edit, pd->buf, pd->cap); pd->ok = 1; pd->done = 1; }
        else if (LOWORD(w) == IDCANCEL) pd->done = 1;
        return 0;
    case WM_CLOSE:
        pd->done = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL PromptText(HWND parent, const wchar_t *title, const wchar_t *label, wchar_t *buf, int cap)
{
    PromptData pd;
    RECT r = { 0, 0, 330, 102 }, pr;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    int w, h;
    HWND dlg;
    MSG msg;
    pd.buf = buf; pd.cap = cap; pd.label = label; pd.done = 0; pd.ok = 0; pd.edit = NULL;
    AdjustWindowRectEx(&r, style, FALSE, ex);
    w = r.right - r.left;
    h = r.bottom - r.top;
    GetWindowRect(parent, &pr);
    dlg = CreateWindowExW(ex, L"MiniPrompt", title, style,
                          pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 3,
                          w, h, parent, NULL, g_hInst, &pd);
    if (!dlg) return FALSE;
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(pd.edit);
    while (!pd.done && GetMessageW(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    EnableWindow(parent, TRUE);
    DestroyWindow(dlg);
    SetFocus(g_sci);
    return pd.ok;
}

/* ====================================================================== */
/* Find / Replace (regex), Find in Files / Replace in Files                */
/* ====================================================================== */

enum { IDC_F_FIND = 2001, IDC_F_REPL, IDC_F_DIR, IDC_F_FILTER, IDC_F_CASE, IDC_F_WORD, IDC_F_REGEX,
       IDC_F_SUB, IDC_F_BROWSE, IDC_F_PREV, IDC_F_REPLACE, IDC_F_REPLALL, IDC_F_FIF, IDC_F_RIF, IDC_F_STATUS };

static HWND g_fFind, g_fRepl, g_fDir, g_fFilter, g_fCase, g_fWord, g_fRegex, g_fSub, g_fStatus;
static wchar_t g_lastFind[512], g_lastRepl[512], g_lastDir[PATH_CAP], g_lastFilter[128] = L"*.*";
static int g_lastCase, g_lastWord, g_lastRegex, g_lastSub = 1;

typedef struct { char *pat; int plen; char *rep; int rlen; int flags; int regex; } FindParams;

/* Scintilla's built-in regex engine is used by default; if Scintilla was built with C++11 regex
 * support, SCFIND_CXX11REGEX switches to the ECMAScript engine (lookahead, \d, \w, ...). */
static void ReadParams(FindParams *p)
{
    if (g_findWnd) {
        GetWindowTextW(g_fFind, g_lastFind, 512);
        GetWindowTextW(g_fRepl, g_lastRepl, 512);
        GetWindowTextW(g_fDir, g_lastDir, PATH_CAP);
        GetWindowTextW(g_fFilter, g_lastFilter, 128);
        g_lastCase = Button_GetCheck(g_fCase) == BST_CHECKED;
        g_lastWord = Button_GetCheck(g_fWord) == BST_CHECKED;
        g_lastRegex = Button_GetCheck(g_fRegex) == BST_CHECKED;
        g_lastSub = Button_GetCheck(g_fSub) == BST_CHECKED;
    }
    p->pat = WideToUtf8(g_lastFind, &p->plen);
    p->rep = WideToUtf8(g_lastRepl, &p->rlen);
    p->regex = g_lastRegex;
    p->flags = (g_lastCase ? SCFIND_MATCHCASE : 0) | (g_lastWord ? SCFIND_WHOLEWORD : 0) |
               (g_lastRegex ? (SCFIND_REGEXP | SCFIND_CXX11REGEX) : 0);
}

static void FreeParams(FindParams *p) { free(p->pat); free(p->rep); }

static void SetFindStatus(const wchar_t *t)
{
    StatusSet(0, t);
    if (g_findWnd) SetWindowTextW(g_fStatus, t);
}

/* Regex searches only run forward, so "find previous" scans forward and keeps the last hit. */
static sptr_t LastRegexMatch(const FindParams *p, sptr_t lo, sptr_t hi, sptr_t *endOut)
{
    sptr_t pos = lo, bs = -1, be = -1;
    SS(SCI_SETSEARCHFLAGS, p->flags, 0);
    while (pos <= hi) {
        sptr_t r, e;
        SS(SCI_SETTARGETRANGE, pos, hi);
        r = SS(SCI_SEARCHINTARGET, p->plen, p->pat);
        if (r < 0) break;
        e = SS(SCI_GETTARGETEND, 0, 0);
        bs = r;
        be = e;
        pos = (e > r) ? e : SS(SCI_POSITIONAFTER, e, 0);
        if (pos <= r) break;
    }
    *endOut = be;
    return bs;
}

/* 0 = not found, 1 = found, 2 = found after wrapping around. */
static int FindOnce(const FindParams *p, int forward)
{
    sptr_t len = SS(SCI_GETLENGTH, 0, 0), s = SS(SCI_GETSELECTIONSTART, 0, 0),
           e = SS(SCI_GETSELECTIONEND, 0, 0), a, b;
    int wrapped = 0;
    if (!p->plen) return 0;
    SS(SCI_SETSEARCHFLAGS, p->flags, 0);
    if (forward) {
        SS(SCI_SETTARGETRANGE, e, len);
        a = SS(SCI_SEARCHINTARGET, p->plen, p->pat);
        if (a < 0) {
            SS(SCI_SETTARGETRANGE, 0, len);
            a = SS(SCI_SEARCHINTARGET, p->plen, p->pat);
            wrapped = 1;
        }
        b = SS(SCI_GETTARGETEND, 0, 0);
    } else if (p->regex) {
        a = LastRegexMatch(p, 0, s, &b);
        if (a < 0) { a = LastRegexMatch(p, 0, len, &b); wrapped = 1; }
    } else {
        SS(SCI_SETTARGETRANGE, s, 0);
        a = SS(SCI_SEARCHINTARGET, p->plen, p->pat);
        if (a < 0) {
            SS(SCI_SETTARGETRANGE, len, 0);
            a = SS(SCI_SEARCHINTARGET, p->plen, p->pat);
            wrapped = 1;
        }
        b = SS(SCI_GETTARGETEND, 0, 0);
    }
    if (a < 0) return 0;
    SS(SCI_ENSUREVISIBLE, SS(SCI_LINEFROMPOSITION, a, 0), 0);
    SS(SCI_SETSEL, a, b);
    SS(SCI_SCROLLCARET, 0, 0);
    return wrapped ? 2 : 1;
}

static void DoFind(int forward)
{
    FindParams p;
    int r;
    ReadParams(&p);
    r = FindOnce(&p, forward);
    FreeParams(&p);
    SetFindStatus(r == 1 ? L"" : r == 2 ? L"Search wrapped around." : L"Not found (or invalid regular expression).");
}

static void DoReplace(void)
{
    FindParams p;
    int r;
    ReadParams(&p);
    if (p.plen) {
        sptr_t s = SS(SCI_GETSELECTIONSTART, 0, 0), e = SS(SCI_GETSELECTIONEND, 0, 0);
        SS(SCI_SETSEARCHFLAGS, p.flags, 0);
        SS(SCI_SETTARGETRANGE, s, e);
        if (SS(SCI_SEARCHINTARGET, p.plen, p.pat) == s && SS(SCI_GETTARGETEND, 0, 0) == e) {
            sptr_t te;
            SS(p.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, p.rlen, p.rep);
            te = SS(SCI_GETTARGETEND, 0, 0);
            SS(SCI_SETSEL, te, te);
        }
        r = FindOnce(&p, 1);
        SetFindStatus(r ? L"" : L"No more matches.");
    }
    FreeParams(&p);
}

static void DoReplaceAll(void)
{
    FindParams p;
    long count = 0;
    wchar_t m[64];
    ReadParams(&p);
    if (p.plen) {
        sptr_t pos = 0;
        SS(SCI_BEGINUNDOACTION, 0, 0);
        SS(SCI_SETSEARCHFLAGS, p.flags, 0);
        for (;;) {
            sptr_t len = SS(SCI_GETLENGTH, 0, 0), r, me, te;
            if (pos > len) break;
            SS(SCI_SETTARGETRANGE, pos, len);
            r = SS(SCI_SEARCHINTARGET, p.plen, p.pat);
            if (r < 0) break;
            me = SS(SCI_GETTARGETEND, 0, 0);
            SS(p.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, p.rlen, p.rep);
            te = SS(SCI_GETTARGETEND, 0, 0);
            count++;
            pos = te;
            if (me == r) {                             /* empty match: step over one character */
                if (te >= SS(SCI_GETLENGTH, 0, 0)) break;
                pos = SS(SCI_POSITIONAFTER, te, 0);
            }
            if (count > 50000000L) break;
        }
        SS(SCI_ENDUNDOACTION, 0, 0);
    }
    FreeParams(&p);
    SNW(m, L"Replaced %ld occurrence(s).", count);
    SetFindStatus(m);
}

/* ---- Find in Files ---- */

typedef struct { wchar_t *path; int line; } Hit;
static Hit *g_hits;
static int g_nHits, g_capHits;

typedef struct {
    const FindParams *p;
    const wchar_t *filter;
    int sub, replace;
    long hits, files, scanned, replaced, changedFiles;
} FifCtx;

static void AddHit(const wchar_t *path, int line, const char *text, size_t tlen)
{
    wchar_t disp[PATH_CAP + 300], *w;
    if (g_nHits == g_capHits) {
        g_capHits = g_capHits ? g_capHits * 2 : 256;
        g_hits = (Hit *)xrealloc(g_hits, (size_t)g_capHits * sizeof *g_hits);
    }
    g_hits[g_nHits].path = _wcsdup(path);
    g_hits[g_nHits].line = line;
    g_nHits++;
    while (tlen && (*text == ' ' || *text == '\t')) { text++; tlen--; }
    if (tlen > 200) tlen = 200;
    w = Utf8ToWide(text, (int)tlen);
    SNW(disp, L"%ls(%d):  %ls", path, line, w);
    free(w);
    ListBox_AddString(g_results, disp);
}

static void ScanFile(FifCtx *c, const wchar_t *path)
{
    unsigned char *raw;
    size_t rawLen, bom = 0, u8len = 0;
    TuEncoding enc;
    char *u8;
    sptr_t len, pos = 0, lastLine = -1;
    long fileHits = 0;

    if (!ReadWholeFile(path, SEARCH_LIMIT, &raw, &rawLen)) return;
    c->scanned++;
    enc = tu_detect_encoding(raw, rawLen > 65536 ? 65536 : rawLen, rawLen > 65536, &bom);
    if (enc != TU_ENC_UTF16LE && enc != TU_ENC_UTF16BE && memchr(raw, 0, rawLen < 8192 ? rawLen : 8192)) {
        free(raw);                                     /* looks binary */
        return;
    }
    u8 = ToUtf8(raw + bom, rawLen - bom, enc, &u8len);
    free(raw);
    if (!u8) return;

    SW(g_scratch, SCI_CLEARALL, 0, 0);
    SW(g_scratch, SCI_APPENDTEXT, u8len, u8);
    free(u8);
    SW(g_scratch, SCI_SETSEARCHFLAGS, c->p->flags, 0);

    for (;;) {
        sptr_t r, me, te;
        len = SW(g_scratch, SCI_GETLENGTH, 0, 0);
        if (pos > len || c->hits >= MAX_HITS) break;
        SW(g_scratch, SCI_SETTARGETRANGE, pos, len);
        r = SW(g_scratch, SCI_SEARCHINTARGET, c->p->plen, c->p->pat);
        if (r < 0) break;
        me = SW(g_scratch, SCI_GETTARGETEND, 0, 0);
        if (c->replace) {
            SW(g_scratch, c->p->regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, c->p->rlen, c->p->rep);
            te = SW(g_scratch, SCI_GETTARGETEND, 0, 0);
            c->replaced++;
        } else {
            sptr_t line = SW(g_scratch, SCI_LINEFROMPOSITION, r, 0);
            te = me;
            if (line != lastLine) {
                sptr_t ls = SW(g_scratch, SCI_POSITIONFROMLINE, line, 0),
                       le = SW(g_scratch, SCI_GETLINEENDPOSITION, line, 0);
                AddHit(path, (int)line + 1, (const char *)SW(g_scratch, SCI_GETRANGEPOINTER, ls, le - ls),
                       (size_t)(le - ls));
                lastLine = line;
            }
        }
        fileHits++;
        c->hits++;
        pos = te;
        if (me == r) {
            if (te >= SW(g_scratch, SCI_GETLENGTH, 0, 0)) break;
            pos = SW(g_scratch, SCI_POSITIONAFTER, te, 0);
        }
    }
    if (fileHits) {
        c->files++;
        if (c->replace) {
            const char *t = (const char *)SW(g_scratch, SCI_GETCHARACTERPOINTER, 0, 0);
            if (WriteEncoded(path, t, (size_t)SW(g_scratch, SCI_GETLENGTH, 0, 0), enc, 0) > 0) c->changedFiles++;
        }
    }
}

static void ScanDir(FifCtx *c, const wchar_t *dir, int depth)
{
    wchar_t pat[PATH_CAP], full[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SNW(pat, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        SNW(full, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (c->sub && depth < 32 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ScanDir(c, full, depth + 1);
        } else if (PathMatchSpecW(fd.cFileName, c->filter)) {
            ScanFile(c, full);
        }
    } while (c->hits < MAX_HITS && FindNextFileW(h, &fd));
    FindClose(h);
}

static void ShowResults(int show)
{
    g_resultsVisible = show;
    ShowWindow(g_results, show ? SW_SHOW : SW_HIDE);
    Layout();
}

static void DoFindInFiles(int replace)
{
    FindParams p;
    FifCtx c;
    wchar_t summary[PATH_CAP + 200];
    size_t dl;
    int i;

    ReadParams(&p);
    dl = wcslen(g_lastDir);
    while (dl > 3 && g_lastDir[dl - 1] == L'\\') g_lastDir[--dl] = 0;
    if (!p.plen || !g_lastDir[0]) {
        FreeParams(&p);
        SetFindStatus(L"Enter the text to find and a folder.");
        return;
    }
    if (replace) {
        wchar_t m[PATH_CAP + 300];
        SNW(m, L"Replace in ALL matching files under:\n%ls\n\nFiles are rewritten on disk and this cannot be undone "
               L"(tabs that are already open are not reloaded).\n\nContinue?", g_lastDir);
        if (MessageBoxW(g_findWnd ? g_findWnd : g_main, m, APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) {
            FreeParams(&p);
            return;
        }
    }
    for (i = 0; i < g_nHits; i++) free(g_hits[i].path);
    g_nHits = 0;
    ListBox_ResetContent(g_results);
    memset(&c, 0, sizeof c);
    c.p = &p;
    c.filter = g_lastFilter[0] ? g_lastFilter : L"*.*";
    c.sub = g_lastSub;
    c.replace = replace;
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    ScanDir(&c, g_lastDir, 0);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    if (replace)
        SNW(summary, L"Replaced %ld occurrence(s) in %ld of %ld file(s) scanned (%ld file(s) written).",
            c.replaced, c.files, c.scanned, c.changedFiles);
    else
        SNW(summary, L"%ld hit(s) in %ld file(s)  [%ld scanned]%ls   -   double-click a line to open it",
            c.hits, c.files, c.scanned, c.hits >= MAX_HITS ? L"  (stopped at the result limit)" : L"");
    ListBox_InsertString(g_results, 0, summary);
    ShowResults(1);
    SetFindStatus(summary);
    FreeParams(&p);
}

static void OpenHit(int item)
{
    Doc *d;
    if (item < 1 || item > g_nHits) return;
    d = OpenDocument(g_hits[item - 1].path);
    if (d) {
        SS(SCI_ENSUREVISIBLE, g_hits[item - 1].line - 1, 0);
        SS(SCI_GOTOLINE, g_hits[item - 1].line - 1, 0);
        SetFocus(g_sci);
    }
}

static void BrowseFolder(void)
{
    BROWSEINFOW bi;
    wchar_t name[MAX_PATH], path[MAX_PATH];
    LPITEMIDLIST pidl;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_findWnd;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"Select the folder to search";
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_fDir, path);
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK FindProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        MkCtl(h, L"STATIC", L"Find what:", 0, 0, 10, 12, 80, 18, -1);
        g_fFind = MkCtl(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 95, 8, 260, 22, IDC_F_FIND);
        MkCtl(h, L"STATIC", L"Replace with:", 0, 0, 10, 42, 80, 18, -1);
        g_fRepl = MkCtl(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 95, 38, 260, 22, IDC_F_REPL);
        MkCtl(h, L"STATIC", L"Folder:", 0, 0, 10, 72, 80, 18, -1);
        g_fDir = MkCtl(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 95, 68, 200, 22, IDC_F_DIR);
        MkCtl(h, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 0, 300, 68, 55, 22, IDC_F_BROWSE);
        MkCtl(h, L"STATIC", L"File filter:", 0, 0, 10, 102, 80, 18, -1);
        g_fFilter = MkCtl(h, L"EDIT", g_lastFilter, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 95, 98, 260, 22, IDC_F_FILTER);
        g_fCase = MkCtl(h, L"BUTTON", L"Match case", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 10, 130, 100, 20, IDC_F_CASE);
        g_fWord = MkCtl(h, L"BUTTON", L"Whole word", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 115, 130, 100, 20, IDC_F_WORD);
        g_fRegex = MkCtl(h, L"BUTTON", L"Regular expression", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 220, 130, 140, 20, IDC_F_REGEX);
        g_fSub = MkCtl(h, L"BUTTON", L"Include subfolders (find in files)", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 10, 152, 230, 20, IDC_F_SUB);
        Button_SetCheck(g_fSub, BST_CHECKED);
        MkCtl(h, L"BUTTON", L"Find Next", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 375, 8, 115, 24, IDOK);
        MkCtl(h, L"BUTTON", L"Find Previous", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 36, 115, 24, IDC_F_PREV);
        MkCtl(h, L"BUTTON", L"Replace", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 64, 115, 24, IDC_F_REPLACE);
        MkCtl(h, L"BUTTON", L"Replace All", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 92, 115, 24, IDC_F_REPLALL);
        MkCtl(h, L"BUTTON", L"Find in Files", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 120, 115, 24, IDC_F_FIF);
        MkCtl(h, L"BUTTON", L"Replace in Files", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 148, 115, 24, IDC_F_RIF);
        MkCtl(h, L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 0, 375, 176, 115, 24, IDCANCEL);
        g_fStatus = MkCtl(h, L"STATIC", L"", 0, 0, 10, 182, 350, 18, IDC_F_STATUS);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK:          DoFind(1); break;
        case IDC_F_PREV:    DoFind(0); break;
        case IDC_F_REPLACE: DoReplace(); break;
        case IDC_F_REPLALL: DoReplaceAll(); break;
        case IDC_F_FIF:     DoFindInFiles(0); break;
        case IDC_F_RIF:     DoFindInFiles(1); break;
        case IDC_F_BROWSE:  BrowseFolder(); break;
        case IDCANCEL:      ShowWindow(h, SW_HIDE); SetFocus(g_sci); break;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        SetFocus(g_sci);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowFind(int focusReplace)
{
    Doc *d = Cur();
    if (!g_findWnd) {
        RECT r = { 0, 0, 500, 210 }, mr;
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT;
        AdjustWindowRectEx(&r, style, FALSE, ex);
        GetWindowRect(g_main, &mr);
        g_findWnd = CreateWindowExW(ex, L"MiniFind", L"Find / Replace", style,
                                    mr.left + 80, mr.top + 90, r.right - r.left, r.bottom - r.top,
                                    g_main, NULL, g_hInst, NULL);
        if (!g_findWnd) return;
        Button_SetCheck(g_fCase, g_lastCase ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_fWord, g_lastWord ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_fRegex, g_lastRegex ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(g_fFind, g_lastFind);
    }
    {   /* pre-fill with the selected text (single line only) and the document's folder */
        sptr_t s = SS(SCI_GETSELECTIONSTART, 0, 0), e = SS(SCI_GETSELECTIONEND, 0, 0);
        if (e > s && e - s < 200) {
            const char *t = (const char *)SS(SCI_GETRANGEPOINTER, s, e - s);
            if (!memchr(t, '\n', (size_t)(e - s)) && !memchr(t, '\r', (size_t)(e - s))) {
                wchar_t *w = Utf8ToWide(t, (int)(e - s));
                SetWindowTextW(g_fFind, w);
                free(w);
            }
        }
        if (d && d->path[0] && !g_lastDir[0]) {
            wchar_t dir[PATH_CAP], *sl;
            CopyW(dir, PATH_CAP, d->path);
            sl = wcsrchr(dir, L'\\');
            if (sl) *sl = 0;
            SetWindowTextW(g_fDir, dir);
        } else {
            SetWindowTextW(g_fDir, g_lastDir);
        }
    }
    ShowWindow(g_findWnd, SW_SHOW);
    SetFocus(focusReplace ? g_fRepl : g_fFind);
    SendMessageW(focusReplace ? g_fRepl : g_fFind, EM_SETSEL, 0, -1);
}

/* F3 / Shift+F3 without opening the dialog once a search term exists. */
static void FindNextCmd(int forward)
{
    if (!g_lastFind[0] && !g_findWnd) { ShowFind(0); return; }
    DoFind(forward);
}

/* ====================================================================== */
/* Plugins                                                                 */
/* ====================================================================== */

typedef struct {
    HMODULE h;
    wchar_t file[MAX_PATH];
    const wchar_t *name;
    PFN_PluginCommandName cmdName;
    PFN_PluginRun run;
    int nCmd;
} Plugin;

static Plugin g_plugins[32];
static int g_nPlugins;
static struct { int plugin, cmd; } g_pluginCmds[256];
static int g_nPluginCmds;
static MiniHost g_host;

static LRESULT HostSci(UINT m, WPARAM w, LPARAM l) { return SendMessageW(g_sci, m, w, l); }

static void LoadPlugins(HMENU menu)
{
    wchar_t dir[PATH_CAP], pat[PATH_CAP], full[PATH_CAP];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SNW(dir, L"%ls\\plugins", g_exeDir);
    SNW(pat, L"%ls\\*.dll", dir);
    g_host.size = sizeof g_host;
    g_host.mainWnd = g_main;
    g_host.sciWnd = g_sci;
    g_host.sci = HostSci;
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            HMODULE mod;
            PFN_PluginInit init;
            PFN_PluginName name;
            PFN_PluginCommandCount count;
            PFN_PluginCommandName cname;
            PFN_PluginRun run;
            Plugin *p;
            HMENU sub;
            int k;
            if (g_nPlugins >= 32) break;
            SNW(full, L"%ls\\%ls", dir, fd.cFileName);
            mod = LoadLibraryW(full);
            if (!mod) continue;
            init = (PFN_PluginInit)(void *)GetProcAddress(mod, "mininpp_init");
            name = (PFN_PluginName)(void *)GetProcAddress(mod, "mininpp_name");
            count = (PFN_PluginCommandCount)(void *)GetProcAddress(mod, "mininpp_command_count");
            cname = (PFN_PluginCommandName)(void *)GetProcAddress(mod, "mininpp_command_name");
            run = (PFN_PluginRun)(void *)GetProcAddress(mod, "mininpp_run");
            if (!init || !name || !count || !cname || !run) { FreeLibrary(mod); continue; }
            init(&g_host);
            p = &g_plugins[g_nPlugins];
            p->h = mod;
            CopyW(p->file, MAX_PATH, fd.cFileName);
            p->name = name();
            p->cmdName = cname;
            p->run = run;
            p->nCmd = count();
            sub = CreatePopupMenu();
            for (k = 0; k < p->nCmd && g_nPluginCmds < 256; k++) {
                g_pluginCmds[g_nPluginCmds].plugin = g_nPlugins;
                g_pluginCmds[g_nPluginCmds].cmd = k;
                AppendMenuW(sub, MF_STRING, (UINT_PTR)(IDM_PLUGIN_BASE + g_nPluginCmds), p->cmdName(k));
                g_nPluginCmds++;
            }
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, p->name);
            g_nPlugins++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_nPlugins) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_PLUGINS_ADMIN, L"Plugins Admin...");
}

static void PluginsAdmin(void)
{
    wchar_t msg[4096], dir[PATH_CAP];
    size_t o = 0;
    int i;
    SNW(dir, L"%ls\\plugins", g_exeDir);
    o += (size_t)_snwprintf(msg + o, 4096 - o, L"Plugins folder:\n%ls\n\n", dir);
    if (!g_nPlugins) o += (size_t)_snwprintf(msg + o, 4096 - o, L"No plugins are loaded.\n");
    for (i = 0; i < g_nPlugins && o < 3500; i++)
        o += (size_t)_snwprintf(msg + o, 4096 - o, L"  %ls   (%ls, %d command%ls)\n", g_plugins[i].name,
                                g_plugins[i].file, g_plugins[i].nCmd, g_plugins[i].nCmd == 1 ? L"" : L"s");
    _snwprintf(msg + o, 4096 - o, L"\nTo install a plugin, copy its .dll into that folder and restart MiniNPP.\n"
                                   L"To remove one, delete the .dll.\n\nOpen the plugins folder now?");
    msg[4095] = 0;
    CreateDirectoryW(dir, NULL);
    if (MessageBoxW(g_main, msg, L"Plugins Admin", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(g_main, L"open", dir, NULL, NULL, SW_SHOW);
}

/* ====================================================================== */
/* Main window: menus, layout, commands                                    */
/* ====================================================================== */

static void Add(HMENU m, int id, const wchar_t *t) { AppendMenuW(m, MF_STRING, (UINT_PTR)id, t); }
static void Sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); }
static HMENU Sub(HMENU bar, const wchar_t *t)
{
    HMENU s = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)s, t);
    return s;
}

static void BuildMenu(HWND wnd)
{
    HMENU bar = CreateMenu(), m, lines;
    int i;

    m = Sub(bar, L"&File");
    Add(m, IDM_FILE_NEW, L"&New\tCtrl+N");
    Add(m, IDM_FILE_OPEN, L"&Open...\tCtrl+O");
    Add(m, IDM_FILE_SAVE, L"&Save\tCtrl+S");
    Add(m, IDM_FILE_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    Add(m, IDM_FILE_CLOSE, L"&Close tab\tCtrl+W");
    Sep(m);
    Add(m, IDM_FILE_EXIT, L"E&xit");

    m = Sub(bar, L"&Edit");
    Add(m, IDM_EDIT_UNDO, L"&Undo\tCtrl+Z");
    Add(m, IDM_EDIT_REDO, L"&Redo\tCtrl+Y");
    Sep(m);
    Add(m, IDM_EDIT_CUT, L"Cu&t\tCtrl+X");
    Add(m, IDM_EDIT_COPY, L"&Copy\tCtrl+C");
    Add(m, IDM_EDIT_PASTE, L"&Paste\tCtrl+V");
    Add(m, IDM_EDIT_SELALL, L"Select &all\tCtrl+A");
    Sep(m);
    lines = Sub(m, L"&Line operations");
    Add(lines, IDM_LINE_DUP, L"Duplicate line\tCtrl+D");
    Add(lines, IDM_LINE_DEL, L"Delete line\tCtrl+L");
    Add(lines, IDM_LINE_UP, L"Move line up\tCtrl+Shift+Up");
    Add(lines, IDM_LINE_DOWN, L"Move line down\tCtrl+Shift+Down");
    Sep(lines);
    Add(lines, IDM_LINE_SORT_ASC, L"Sort lines ascending");
    Add(lines, IDM_LINE_SORT_DESC, L"Sort lines descending");
    Add(lines, IDM_LINE_UNIQUE, L"Remove duplicate lines");
    Add(lines, IDM_LINE_NOEMPTY, L"Remove empty / blank lines");
    Add(lines, IDM_LINE_TRIM, L"Trim trailing spaces");
    Sep(lines);
    Add(lines, IDM_CASE_UPPER, L"UPPERCASE\tCtrl+Shift+U");
    Add(lines, IDM_CASE_LOWER, L"lowercase\tCtrl+U");
    Add(m, IDM_SEL_NEXT, L"Add next occurrence to selection\tAlt+D");
    Add(m, IDM_SEL_ALL, L"Select all occurrences\tAlt+A");
    Add(m, IDM_COL_NUMBERS, L"Insert numbers down the selection...\tAlt+C");
    Sep(m);
    Add(m, IDM_EDIT_GOTO, L"&Go to line...\tCtrl+G");

    m = Sub(bar, L"&Search");
    Add(m, IDM_FIND, L"&Find...\tCtrl+F");
    Add(m, IDM_REPLACE, L"&Replace...\tCtrl+H");
    Add(m, IDM_FINDNEXT, L"Find &next\tF3");
    Add(m, IDM_FINDPREV, L"Find p&revious\tShift+F3");
    Sep(m);
    Add(m, IDM_FINDFILES, L"Find / replace in &files...\tCtrl+Shift+F");
    Add(m, IDM_VIEW_RESULTS, L"Show / hide search results");

    g_mView = m = Sub(bar, L"&View");
    Add(m, IDM_VIEW_WRAP, L"Word wrap");
    Add(m, IDM_VIEW_WS, L"Show whitespace");
    Add(m, IDM_VIEW_EOL, L"Show line endings");
    Sep(m);
    Add(m, IDM_VIEW_FOLDALL, L"Fold all\tAlt+0");
    Add(m, IDM_VIEW_UNFOLDALL, L"Unfold all\tAlt+Shift+0");
    Sep(m);
    Add(m, IDM_VIEW_ZOOMIN, L"Zoom in\tCtrl++");
    Add(m, IDM_VIEW_ZOOMOUT, L"Zoom out\tCtrl+-");
    Add(m, IDM_VIEW_ZOOM0, L"Restore zoom\tCtrl+0");
    Sep(m);
    Add(m, IDM_VIEW_NEXTTAB, L"Next tab\tCtrl+Tab");
    Add(m, IDM_VIEW_PREVTAB, L"Previous tab\tCtrl+Shift+Tab");
    Sep(m);
    Add(m, IDM_VIEW_MONITOR, L"Monitor file changes (tail -f)");

    g_mEnc = m = Sub(bar, L"E&ncoding");
    Add(m, IDM_ENC_BASE + TU_ENC_UTF8, L"UTF-8");
    Add(m, IDM_ENC_BASE + TU_ENC_UTF8_BOM, L"UTF-8 with BOM");
    Add(m, IDM_ENC_BASE + TU_ENC_UTF16LE, L"UTF-16 LE");
    Add(m, IDM_ENC_BASE + TU_ENC_UTF16BE, L"UTF-16 BE");
    Add(m, IDM_ENC_BASE + TU_ENC_ANSI, L"ANSI (system code page)");
    Sep(m);
    g_mEol = Sub(m, L"Line ending");
    Add(g_mEol, IDM_EOL_BASE + SC_EOL_CRLF, L"Windows (CR LF)");
    Add(g_mEol, IDM_EOL_BASE + SC_EOL_CR, L"Old Mac (CR)");
    Add(g_mEol, IDM_EOL_BASE + SC_EOL_LF, L"Unix (LF)");

    g_mLang = m = Sub(bar, L"&Language");
    for (i = 0; i < LANG_COUNT; i++) Add(m, IDM_LANG_BASE + i, LANGS[i].name);

    g_mMacro = m = Sub(bar, L"&Macro");
    Add(m, IDM_MACRO_REC, L"Start / stop recording\tCtrl+Shift+R");
    Add(m, IDM_MACRO_PLAY, L"Playback\tCtrl+Shift+P");
    Add(m, IDM_MACRO_MULTI, L"Run a macro multiple times...");
    Add(m, IDM_MACRO_TOEND, L"Run until the end of the file");

    g_mPlugins = Sub(bar, L"&Plugins");
    LoadPlugins(g_mPlugins);

    SetMenu(wnd, bar);
}

static void BuildAccelerators(void)
{
    static ACCEL A[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW }, { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE }, { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVEAS },
        { FVIRTKEY | FCONTROL, 'W', IDM_FILE_CLOSE },
        { FVIRTKEY | FCONTROL, 'F', IDM_FIND }, { FVIRTKEY | FCONTROL, 'H', IDM_REPLACE },
        { FVIRTKEY, VK_F3, IDM_FINDNEXT }, { FVIRTKEY | FSHIFT, VK_F3, IDM_FINDPREV },
        { FVIRTKEY | FCONTROL | FSHIFT, 'F', IDM_FINDFILES }, { FVIRTKEY | FCONTROL, 'G', IDM_EDIT_GOTO },
        { FVIRTKEY | FCONTROL, 'D', IDM_LINE_DUP }, { FVIRTKEY | FCONTROL, 'L', IDM_LINE_DEL },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_UP, IDM_LINE_UP }, { FVIRTKEY | FCONTROL | FSHIFT, VK_DOWN, IDM_LINE_DOWN },
        { FVIRTKEY | FCONTROL, 'U', IDM_CASE_LOWER }, { FVIRTKEY | FCONTROL | FSHIFT, 'U', IDM_CASE_UPPER },
        { FVIRTKEY | FALT, 'D', IDM_SEL_NEXT }, { FVIRTKEY | FALT, 'A', IDM_SEL_ALL },
        { FVIRTKEY | FALT, 'C', IDM_COL_NUMBERS },
        { FVIRTKEY | FCONTROL | FSHIFT, 'R', IDM_MACRO_REC }, { FVIRTKEY | FCONTROL | FSHIFT, 'P', IDM_MACRO_PLAY },
        { FVIRTKEY | FALT, '0', IDM_VIEW_FOLDALL }, { FVIRTKEY | FALT | FSHIFT, '0', IDM_VIEW_UNFOLDALL },
        { FVIRTKEY | FCONTROL, VK_ADD, IDM_VIEW_ZOOMIN }, { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_VIEW_ZOOMIN },
        { FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_VIEW_ZOOMOUT }, { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_VIEW_ZOOMOUT },
        { FVIRTKEY | FCONTROL, VK_NUMPAD0, IDM_VIEW_ZOOM0 }, { FVIRTKEY | FCONTROL, '0', IDM_VIEW_ZOOM0 },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_VIEW_NEXTTAB }, { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_VIEW_PREVTAB }
    };
    g_accel = CreateAcceleratorTableW((LPACCEL)(void *)A, (int)(sizeof A / sizeof A[0]));
}

static void UpdateMenuChecks(void)
{
    Doc *d = Cur();
    if (!d) return;
    CheckMenuRadioItem(g_mEnc, IDM_ENC_BASE, IDM_ENC_BASE + 4, IDM_ENC_BASE + (UINT)d->enc, MF_BYCOMMAND);
    CheckMenuRadioItem(g_mEol, IDM_EOL_BASE, IDM_EOL_BASE + 2, IDM_EOL_BASE + (UINT)SS(SCI_GETEOLMODE, 0, 0), MF_BYCOMMAND);
    CheckMenuRadioItem(g_mLang, IDM_LANG_BASE, IDM_LANG_BASE + LANG_COUNT - 1, IDM_LANG_BASE + (UINT)d->lang, MF_BYCOMMAND);
    CheckMenuItem(g_mView, IDM_VIEW_WRAP, MF_BYCOMMAND | (g_wrap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_mView, IDM_VIEW_WS, MF_BYCOMMAND | (g_showWs ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_mView, IDM_VIEW_EOL, MF_BYCOMMAND | (g_showEol ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_mView, IDM_VIEW_MONITOR, MF_BYCOMMAND | (d->monitor ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_mMacro, IDM_MACRO_REC, MF_BYCOMMAND | (g_recording ? MF_CHECKED : MF_UNCHECKED));
}

static void Layout(void)
{
    RECT rc, sr;
    int w, h, sbh, tabH = 26, resH, parts[5];
    if (!g_tab || !g_sci || !g_status) return;
    GetClientRect(g_main, &rc);
    w = rc.right;
    h = rc.bottom;
    SendMessageW(g_status, WM_SIZE, 0, 0);
    GetWindowRect(g_status, &sr);
    sbh = sr.bottom - sr.top;
    resH = g_resultsVisible ? 160 : 0;
    MoveWindow(g_tab, 0, 0, w, tabH, TRUE);
    MoveWindow(g_sci, 0, tabH, w, h - tabH - sbh - resH > 0 ? h - tabH - sbh - resH : 0, TRUE);
    MoveWindow(g_results, 0, h - sbh - resH, w, resH, TRUE);
    parts[0] = w - 580 > 100 ? w - 580 : 100;
    parts[1] = parts[0] + 210;
    parts[2] = parts[1] + 150;
    parts[3] = parts[2] + 110;
    parts[4] = -1;
    SendMessageW(g_status, SB_SETPARTS, 5, (LPARAM)parts);
    UpdateStatus();
}

static void ApplyViewOptions(void)
{
    SS(SCI_SETWRAPMODE, g_wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
    SS(SCI_SETVIEWWS, g_showWs ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
    SS(SCI_SETVIEWEOL, g_showEol, 0);
}

static void ToggleMonitor(void)
{
    Doc *d = Cur();
    if (!d) return;
    if (!d->monitor) {
        if (!d->path[0] || d->dirty || d->enc == TU_ENC_UTF16LE || d->enc == TU_ENC_UTF16BE) {
            MessageBoxW(g_main, L"Monitoring needs a saved, unmodified file that is not UTF-16.", APP_NAME, MB_ICONINFORMATION);
            return;
        }
        d->monitor = 1;
        CheckMonitor(d);
    } else d->monitor = 0;
}

static void ChangeEncoding(int enc)
{
    Doc *d = Cur();
    if (!d || enc == d->enc) return;
    d->enc = enc;                                      /* the text is Unicode inside; this sets how it is saved */
    d->dirty = 1;
    d->needsBackup = 1;
    UpdateTab(g_cur);
    UpdateTitle();
    UpdateStatus();
    g_sessionDirty = 1;
}

static void ChangeEol(int mode)
{
    SS(SCI_SETEOLMODE, mode, 0);
    SS(SCI_CONVERTEOLS, mode, 0);
    UpdateStatus();
}

static void GotoLine(void)
{
    wchar_t buf[32] = L"";
    if (PromptText(g_main, L"Go to line", L"Line number:", buf, 32)) {
        int n = _wtoi(buf);
        if (n > 0) {
            SS(SCI_ENSUREVISIBLE, n - 1, 0);
            SS(SCI_GOTOLINE, n - 1, 0);
        }
    }
}

static void OnCommand(int id)
{
    Doc *d = Cur();
    if (id >= IDM_PLUGIN_BASE && id < IDM_PLUGIN_BASE + g_nPluginCmds) {
        int k = id - IDM_PLUGIN_BASE;
        g_plugins[g_pluginCmds[k].plugin].run(g_pluginCmds[k].cmd);
        return;
    }
    if (id >= IDM_ENC_BASE && id <= IDM_ENC_BASE + 4) { ChangeEncoding(id - IDM_ENC_BASE); return; }
    if (id >= IDM_EOL_BASE && id <= IDM_EOL_BASE + 2) { ChangeEol(id - IDM_EOL_BASE); return; }
    if (id >= IDM_LANG_BASE && id < IDM_LANG_BASE + LANG_COUNT) {
        if (d) ApplyLanguage(d, id - IDM_LANG_BASE);
        g_sessionDirty = 1;
        return;
    }
    switch (id) {
    case IDM_FILE_NEW:    NewUntitled(); break;
    case IDM_FILE_OPEN:   OpenDialog(); break;
    case IDM_FILE_SAVE:   if (d) SaveDoc(d, 0); break;
    case IDM_FILE_SAVEAS: if (d) SaveDoc(d, 1); break;
    case IDM_FILE_CLOSE:  CloseDoc(g_cur); break;
    case IDM_FILE_EXIT:   SendMessageW(g_main, WM_CLOSE, 0, 0); break;

    case IDM_EDIT_UNDO:   SS(SCI_UNDO, 0, 0); break;
    case IDM_EDIT_REDO:   SS(SCI_REDO, 0, 0); break;
    case IDM_EDIT_CUT:    SS(SCI_CUT, 0, 0); break;
    case IDM_EDIT_COPY:   SS(SCI_COPY, 0, 0); break;
    case IDM_EDIT_PASTE:  SS(SCI_PASTE, 0, 0); break;
    case IDM_EDIT_SELALL: SS(SCI_SELECTALL, 0, 0); break;
    case IDM_EDIT_GOTO:   GotoLine(); break;

    case IDM_LINE_DUP:    SS(SCI_LINEDUPLICATE, 0, 0); break;
    case IDM_LINE_DEL:    SS(SCI_LINEDELETE, 0, 0); break;
    case IDM_LINE_UP:     SS(SCI_MOVESELECTEDLINESUP, 0, 0); break;
    case IDM_LINE_DOWN:   SS(SCI_MOVESELECTEDLINESDOWN, 0, 0); break;
    case IDM_LINE_SORT_ASC: case IDM_LINE_SORT_DESC: case IDM_LINE_UNIQUE:
    case IDM_LINE_NOEMPTY: case IDM_LINE_TRIM: LineTool(id); break;
    case IDM_CASE_UPPER:  SS(SCI_UPPERCASE, 0, 0); break;
    case IDM_CASE_LOWER:  SS(SCI_LOWERCASE, 0, 0); break;

    case IDM_SEL_NEXT:    SS(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE, 0); SS(SCI_MULTIPLESELECTADDNEXT, 0, 0); break;
    case IDM_SEL_ALL:     SS(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE, 0); SS(SCI_MULTIPLESELECTADDEACH, 0, 0); break;
    case IDM_COL_NUMBERS: ColumnNumbers(); break;

    case IDM_FIND:        ShowFind(0); break;
    case IDM_REPLACE:     ShowFind(1); break;
    case IDM_FINDNEXT:    FindNextCmd(1); break;
    case IDM_FINDPREV:    FindNextCmd(0); break;
    case IDM_FINDFILES:   ShowFind(0); SetFocus(g_fDir); break;
    case IDM_VIEW_RESULTS: ShowResults(!g_resultsVisible); break;

    case IDM_VIEW_WRAP:   g_wrap = !g_wrap; ApplyViewOptions(); break;
    case IDM_VIEW_WS:     g_showWs = !g_showWs; ApplyViewOptions(); break;
    case IDM_VIEW_EOL:    g_showEol = !g_showEol; ApplyViewOptions(); break;
    case IDM_VIEW_FOLDALL:   SS(SCI_FOLDALL, SC_FOLDACTION_CONTRACT, 0); break;
    case IDM_VIEW_UNFOLDALL: SS(SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0); break;
    case IDM_VIEW_ZOOMIN:  SS(SCI_ZOOMIN, 0, 0); UpdateLineMargin(); break;
    case IDM_VIEW_ZOOMOUT: SS(SCI_ZOOMOUT, 0, 0); UpdateLineMargin(); break;
    case IDM_VIEW_ZOOM0:   SS(SCI_SETZOOM, 0, 0); UpdateLineMargin(); break;
    case IDM_VIEW_NEXTTAB: if (g_nDocs > 1) SwitchTo((g_cur + 1) % g_nDocs); break;
    case IDM_VIEW_PREVTAB: if (g_nDocs > 1) SwitchTo((g_cur + g_nDocs - 1) % g_nDocs); break;
    case IDM_VIEW_MONITOR: ToggleMonitor(); break;

    case IDM_MACRO_REC:   MacroToggle(); break;
    case IDM_MACRO_PLAY:  MacroPlay(1, 0); break;
    case IDM_MACRO_MULTI: {
        wchar_t buf[16] = L"10";
        if (g_nMacro && PromptText(g_main, L"Run macro", L"Number of times:", buf, 16)) {
            int n = _wtoi(buf);
            if (n > 0) MacroPlay(n, 0);
        }
        break;
    }
    case IDM_MACRO_TOEND: MacroPlay(0, 1); break;
    case IDM_PLUGINS_ADMIN: PluginsAdmin(); break;
    }
}

static void OnScintilla(const SCNotification *n)
{
    Doc *d = Cur();
    switch (n->nmhdr.code) {
    case SCN_SAVEPOINTLEFT:
        if (d) { d->dirty = 1; UpdateTab(g_cur); UpdateTitle(); g_sessionDirty = 1; }
        break;
    case SCN_SAVEPOINTREACHED:
        if (d) { d->dirty = 0; d->needsBackup = 0; DeleteBackup(d); UpdateTab(g_cur); UpdateTitle(); }
        break;
    case SCN_MODIFIED:
        if (d && (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
            d->touched = 1;
            d->needsBackup = 1;
            if (n->linesAdded) UpdateLineMargin();
        }
        break;
    case SCN_UPDATEUI:
        if (n->updated & SC_UPDATE_SELECTION) MatchBraces();
        UpdateStatus();
        break;
    case SCN_CHARADDED:
        OnCharAdded(n->ch);
        break;
    case SCN_MACRORECORD:
        MacroRecord((UINT)n->message, n->wParam, n->lParam);
        break;
    }
}

static LRESULT CALLBACK TabProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (m == WM_MBUTTONUP) {                           /* middle-click closes a tab */
        TCHITTESTINFO ht;
        int i;
        ht.pt.x = GET_X_LPARAM(l);
        ht.pt.y = GET_Y_LPARAM(l);
        i = TabCtrl_HitTest(h, &ht);
        if (i >= 0) CloseDoc(i);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_main = h;
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                                0, 0, 100, 26, h, (HMENU)(INT_PTR)IDC_TAB, g_hInst, NULL);
        SendMessageW(g_tab, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SetWindowSubclass(g_tab, TabProc, 1, 0);
        g_sci = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 26, 100, 100, h, (HMENU)(INT_PTR)IDC_SCI, g_hInst, NULL);
        g_scratch = CreateWindowExW(0, L"Scintilla", L"", WS_CHILD, 0, 0, 10, 10, h, NULL, g_hInst, NULL);
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, h, (HMENU)(INT_PTR)IDC_STATUS, g_hInst, NULL);
        g_results = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                                    WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                    0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_RESULTS, g_hInst, NULL);
        SendMessageW(g_results, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        if (!g_sci || !g_scratch) return -1;
        SetupView();
        SW(g_scratch, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
        SW(g_scratch, SCI_SETUNDOCOLLECTION, 0, 0);
        BuildMenu(h);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_sci);
        return 0;
    case WM_INITMENUPOPUP:
        UpdateMenuChecks();
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_RESULTS) {
            if (HIWORD(w) == LBN_DBLCLK) OpenHit(ListBox_GetCurSel(g_results));
        } else if (!l || HIWORD(w) <= 1) {              /* menu or accelerator */
            OnCommand(LOWORD(w));
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nh = (const NMHDR *)l;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) { SwitchTo(TabCtrl_GetCurSel(g_tab)); return 0; }
        if (nh->hwndFrom == g_sci) { OnScintilla((const SCNotification *)l); return 0; }
        break;
    }
    case WM_DROPFILES: {
        HDROP dr = (HDROP)w;
        UINT i, n = DragQueryFileW(dr, 0xFFFFFFFF, NULL, 0);
        for (i = 0; i < n; i++) {
            wchar_t p[PATH_CAP];
            DragQueryFileW(dr, i, p, PATH_CAP);
            OpenDocument(p);
        }
        DragFinish(dr);
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_SESSION) {
            FlushBackup(Cur());
            if (g_sessionDirty) { SaveSession(); g_sessionDirty = 0; }
        } else if (w == TIMER_MONITOR) {
            CheckMonitor(Cur());
        }
        return 0;
    case WM_CLOSE:
        SaveSession();             /* unsaved tabs are kept in the backup folder and come back next start */
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int LoadScintilla(void)
{
    wchar_t p[PATH_CAP];
    HMODULE h, lx;
    SNW(p, L"%ls\\Scintilla.dll", g_exeDir);
    h = LoadLibraryW(p);
    if (!h) h = LoadLibraryW(L"Scintilla.dll");
    if (!h) return 0;                                  /* DllMain registers the "Scintilla" window class */
    SNW(p, L"%ls\\Lexilla.dll", g_exeDir);
    lx = LoadLibraryW(p);
    if (!lx) lx = LoadLibraryW(L"Lexilla.dll");
    if (lx) g_createLexer = (CreateLexerFn)(void *)GetProcAddress(lx, "CreateLexer");
    return 1;
}

static void RegisterClasses(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"MiniNPPMain";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    wc.hIcon = NULL;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = FindProc;
    wc.lpszClassName = L"MiniFind";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PromptProc;
    wc.lpszClassName = L"MiniPrompt";
    RegisterClassExW(&wc);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    wchar_t **argv;
    int argc, i;
    MSG msg;
    (void)hPrev; (void)cmd;

    g_hInst = hInst;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    InitPaths();
    if (!LoadScintilla()) {
        MessageBoxW(NULL, L"Could not load Scintilla.dll.\n\nPut Scintilla.dll (and Lexilla.dll for syntax "
                          L"highlighting) next to MiniNPP.exe. See README.md.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    RegisterClasses();
    BuildAccelerators();

    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, L"MiniNPPMain", APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, NULL, NULL, hInst, NULL);
    if (!g_main) {
        MessageBoxW(NULL, L"Could not create the editor window (is Scintilla.dll the right architecture "
                          L"- 32 vs 64 bit?).", APP_NAME, MB_ICONERROR);
        return 1;
    }
    if (!g_createLexer)
        StatusSet(0, L"Lexilla.dll not found - running without syntax highlighting.");

    RestoreSession();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (i = 1; i < argc; i++) OpenDocument(argv[i]);
        LocalFree(argv);
    }
    if (!g_nDocs) NewUntitled();
    ApplyViewOptions();
    Layout();
    SwitchTo(g_cur);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetTimer(g_main, TIMER_SESSION, 5000, NULL);
    SetTimer(g_main, TIMER_MONITOR, 700, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_findWnd && IsWindowVisible(g_findWnd) && IsDialogMessageW(g_findWnd, &msg)) continue;
        if (TranslateAcceleratorW(g_main, g_accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
