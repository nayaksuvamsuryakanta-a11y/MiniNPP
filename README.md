# MiniNPP

A small Notepad++-style editor written in C, keeping only the ten features most people use every day.
It follows Notepad++'s own architecture: **Win32** for the window, **Scintilla** as the editing engine,
**Lexilla** for syntax lexers, and a DLL plugin interface.

## Status (please read)

| Part | Verified how |
|---|---|
| `textutil.c` (encodings, line endings, sort/dedupe/trim) | 36 checks in `test_textutil.c`, run under AddressSanitizer + UBSan: **all pass** |
| `mininpp.c`, `plugins/jsonfmt.c` | **Compiled successfully** with MinGW-w64 GCC 16.2.0 (0 errors, 0 warnings) |
| Building, linking, running on Windows | **Verified.** Scintilla 5.5.8 and Lexilla 5.4.6 built from source and linked successfully. |

So treat the first build as a shakedown. If the compiler complains, the most likely causes are small
differences between Scintilla versions (a renamed constant) or a missing library on the link line.
Send me the exact error and it is usually a one-line fix.

## What you need

1. A C compiler: **MinGW-w64 (64-bit gcc)** or **Visual Studio**. Build a **64-bit** exe.
2. Scintilla and Lexilla (both from their official sites, scintilla.org and lexilla.org):
   - the headers `Scintilla.h` and `SciLexer.h` (in the Scintilla `include` folder)
   - `Scintilla.dll` and `Lexilla.dll`. Build them from source with each project's own instructions
     (each has a makefile for MinGW and an `nmake` file for MSVC in its `win32` / `src` folder; exact
     paths vary a little by version). Use the same architecture (64-bit) as MiniNPP.
3. Put `Scintilla.dll` and `Lexilla.dll` in the same folder as `MiniNPP.exe`.
   Without `Lexilla.dll` the editor still works, just without colours and folding.

## Build

MinGW-w64: edit `SCI=` in `build.bat` to your Scintilla `include` folder, then run it.
Visual Studio: use `build_msvc.bat` from an "x64 Native Tools" prompt.
Or by hand:

```
gcc -O2 -mwindows -o MiniNPP.exe mininpp.c textutil.c -I<scintilla>/include -I. ^
    -lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -lole32 -luser32 -lgdi32
```

Run the portable unit tests on any OS:

```
gcc -Wall -Wextra -o test_textutil test_textutil.c textutil.c && ./test_textutil
```

## The ten features, and how to use them

| # | Feature | How |
|---|---|---|
| 1 | Syntax highlighting + code folding | Automatic by file extension (C/C++/Java/JS/C#, Python, JSON, XML/HTML, INI) or the **Language** menu. Click the `+/-` margin, or **View > Fold all** (Alt+0) |
| 2 | Tabs + session restore | Tabs across the top; middle-click or Ctrl+W closes. Closing the app does **not** ask to save: every tab, including unsaved ones, comes back next start (also after a crash; backups are written every few seconds) |
| 3 | Find / Replace with regex, Find in Files | Ctrl+F, Ctrl+H, F3 / Shift+F3. **Ctrl+Shift+F** for folder-wide search (filter like `*.c;*.h`) and Replace in Files. Double-click a result to open it. Regex replace uses `\1`, `\2` |
| 4 | Multi-cursor and column mode | Ctrl+click adds a caret; **Alt+D** adds the next occurrence of the selection, **Alt+A** selects all of them; Alt+drag or Alt+Shift+arrows for a column selection; **Alt+C** fills the selection with 1, 2, 3, ... |
| 5 | Encoding and line endings | **Encoding** menu: UTF-8, UTF-8 BOM, UTF-16 LE/BE, ANSI, and Windows / Unix / Mac line endings |
| 6 | Macros | Ctrl+Shift+R start/stop recording, Ctrl+Shift+P play, plus "run N times" and "run until end of file" |
| 7 | Plugins | Put a plugin `.dll` in `plugins\` next to the exe; it appears under **Plugins**. `plugins/jsonfmt.c` is a working example. API in `mininpp_plugin.h` |
| 8 | Line tools | **Edit > Line operations**: duplicate (Ctrl+D), delete (Ctrl+L), move (Ctrl+Shift+Up/Down), sort, remove duplicates, remove blank lines, trim trailing spaces, upper/lower case. They act on the selected lines, or the whole file if nothing is selected |
| 9 | Auto-completion + bracket matching | Word completion pops up after 3 typed letters; matching brackets highlight; new lines keep the previous indent |
| 10 | Large files + tail -f | UTF-8 files are streamed in 1 MB chunks. Highlighting is switched off above 24 MB. **View > Monitor file changes** follows a growing log |

## Honest limitations

- **Plugins Admin** only lists and loads DLLs from the plugins folder. There is no online plugin catalogue like Notepad++ has.
- **Macros** live in memory only (lost on exit), and searches made from the Find window are not recorded.
- **Regex flavour** depends on how Scintilla was built. If it has C++11 regex support (the default), you get ECMAScript regex; otherwise Scintilla's simpler built-in engine.
- **Encoding** changes how the file is *saved*; the text stays Unicode in the editor. A BOM-less UTF-16 file gains a BOM on save. UTF-16 and ANSI files are limited to 512 MB.
- **Find in Files** runs on the UI thread, so the window freezes during a scan of a very large tree. Replace in Files rewrites files on disk and does not refresh tabs that are already open (you get a confirmation first).
- Highlighting covers 5 language groups; adding more is one row in the `LANGS` table plus a style list.
- The UI uses fixed pixel sizes, so on very high DPI screens Windows scales it up (slightly blurry).

## Files

```
mininpp.c            the application (Win32 + Scintilla)
textutil.c / .h      portable helpers, unit-tested
test_textutil.c      unit tests
mininpp_plugin.h     plugin interface
plugins/jsonfmt.c    example plugin (pretty-print / minify JSON)
build.bat            MinGW-w64 build        build_msvc.bat   Visual Studio build
MiniNPP.exe.manifest optional: modern-looking controls (MinGW builds)
```

Session data and backups are kept in `%APPDATA%\MiniNPP`.
