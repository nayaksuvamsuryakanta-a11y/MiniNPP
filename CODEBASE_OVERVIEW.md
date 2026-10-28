# MiniNPP: Codebase Architecture Overview

## 1. Introduction

MiniNPP is a native Notepad++-style text editor for 64-bit Windows. It is implemented in C99 around three cooperating components:

- The raw Win32 API provides the application window, menus, dialogs, tab control, status bar, timers, file dialogs, drag-and-drop handling, and message loop.
- Scintilla provides the text editing surface, document storage, selections, search, folding, undo, notifications, and editor commands.
- Lexilla supplies the language lexers used by Scintilla for syntax highlighting and folding.

The portable `textutil.c` library is kept separate from the Windows UI. It implements encoding detection, UTF-8/UTF-16 conversion, line-ending helpers, and line operations. The application is built with MinGW-w64 GCC and uses the Unicode Win32 API.

The source currently uses `WinMain` with Unicode APIs and obtains the Unicode command line through `GetCommandLineW`/`CommandLineToArgvW`; it does not define a `wWinMain` entry point.

## 2. High-Level Architecture

The application is a Win32 shell around a Scintilla editing control:

1. `WinMain` initializes common controls, resolves the executable directory, loads Scintilla and Lexilla, registers MiniNPP's window classes, builds accelerators and menus, creates the main window, restores the previous session, and enters the standard `GetMessageW`/`TranslateAcceleratorW`/`DispatchMessageW` loop.
2. `MainProc` owns the top-level window lifecycle. During `WM_CREATE` it creates the tab control, the primary Scintilla control, a hidden Scintilla scratch control for find-in-files, the status bar, and the search-results list box.
3. Win32 notifications and commands are translated into editor operations by `OnCommand`, `OnScintilla`, and the window procedures. Scintilla notifications mark documents dirty, update the status bar and tab labels, trigger completion and brace matching, and record macros.
4. Each open tab is represented by a `Doc` structure while the actual text remains in a Scintilla document referenced by `Doc.ptr`. Switching tabs changes the document attached to the visible Scintilla control with `SCI_SETDOCPOINTER`.

`LoadScintilla` calls `LoadLibraryW` for `Scintilla.dll` and then `Lexilla.dll`, first beside the executable and then through the normal DLL search path. Scintilla's DLL initialization registers the `"Scintilla"` window class. Lexilla's `CreateLexer` export is resolved with `GetProcAddress`; if Lexilla is unavailable, the editor can continue without syntax highlighting and reports that state in the status bar.

## 3. Core Data Structures

### The `Doc` structure

`Doc` is the per-tab application record:

- `ptr` is the Scintilla document handle returned by `SCI_CREATEDOCUMENT`. It identifies the document buffer held by Scintilla and is attached to the visible editor when the tab is active.
- `path` stores the canonical file path. An empty path denotes an untitled document.
- `enc` stores the `TuEncoding` used when the document is saved.
- `lang` stores the selected language index into `LANGS`.
- `dirty` records whether the current Scintilla document differs from its save point.
- `touched` records whether an untitled document has ever been edited, allowing truly empty untouched tabs to be omitted from session storage.
- `needsBackup` records whether the text has changed since the last crash-recovery backup.
- `uid` identifies the per-document backup file under `%APPDATA%\MiniNPP\backup`.
- `untitledNo` supplies display names such as `new 1`.
- `monitor` and `monSize` implement file monitoring for saved, unmodified non-UTF-16 files.
- `caret` and `topLine` preserve the caret and first visible line while a document is in the background.

The global `g_docs` array stores pointers to all open `Doc` records. `g_nDocs` is the active count, `g_capDocs` is the allocated capacity, and `g_cur` identifies the selected tab. `DocNew` grows the array with `xrealloc`, creates a Scintilla document, and inserts a tab. `SwitchTo` saves the outgoing view state, flushes its backup if needed, attaches the selected Scintilla document, restores its view state, and refreshes the UI. `CloseDoc` releases the Scintilla document with `SCI_RELEASEDOCUMENT` and frees the `Doc`.

### The `LANGS` array

`LANGS` is a table of `LangDef` records. Each row supplies a display name, a semicolon-delimited extension list, a Lexilla lexer name, keyword sets, and a style specification table. The C/C++ row contains `.c`, `.h`, `.cpp`, and related extensions and maps them to the Lexilla `"cpp"` lexer. Python maps to `"python"`, JSON to `"json"`, XML/HTML to `"xml"`, and configuration files to `"props"`.

`DetectLang` lowercases the path extension, wraps it in semicolons, and searches the corresponding extension string. `ApplyLanguage` selects the lexer through `g_createLexer`, sends it to Scintilla with `SCI_SETILEXER`, configures folding properties and keywords, applies the language-specific styles, and requests recolouring with `SCI_COLOURISE`.

## 4. The 10 Core Features (Implementation Breakdown)

### 1. Syntax highlighting and code folding

`ApplyLanguage` maps a document's `lang` index to a Lexilla lexer object created by the dynamically resolved `CreateLexer` function. It installs the lexer with `SCI_SETILEXER`, sets keyword lists with `SCI_SETKEYWORDS`, and enables folding properties such as `fold`, `fold.comment`, `fold.preprocessor`, and `fold.html`. `SetupStyles` applies the configured colours and emphasis for comments, keywords, strings, numbers, operators, and language-specific tokens. `SetupView` defines fold markers, assigns the fold mask to the fold margin, enables click/change automatic folding, and configures the line-number margin.

For files larger than `LEXER_LIMIT` (24 MiB), `ApplyLanguage` falls back to plain text to keep lexing and folding responsive.

### 2. Tabs with session restore

Tabs are backed by `Doc` records and Scintilla document pointers rather than by copying text into application-owned buffers. `SaveSession` writes `%APPDATA%\MiniNPP\session.txt` with a version header, one `tab|...` record per retained tab, and an `active=` record. It stores encoding, language, caret, top-line, dirty state, backup UID, and path.

Dirty documents are persisted separately in `%APPDATA%\MiniNPP\backup\<uid>.txt`. `FlushBackup` writes the active document's Scintilla text when `needsBackup` is set. A five-second timer flushes backups and saves session metadata, and `WM_CLOSE` saves the session before destroying the window. `RestoreSession` reads the session file, reopens saved paths, loads dirty tabs from their backup files, restores language and view state, and selects the recorded active tab. `PruneBackups` removes backup files that no longer correspond to dirty open documents.

File writes use `WriteFileAtomic`: data is written to a temporary sibling file, flushed, and then moved over the destination with `MoveFileExW`. On failure, the temporary file is deleted so a partial write does not replace the original.

### 3. Find/Replace with regex and Find/Replace in Files

The find dialog stores the pattern, replacement, directory, filter, case, whole-word, regex, and recursive-search settings. `ReadParams` converts the wide dialog text to UTF-8 and builds Scintilla search flags, including `SCFIND_REGEXP` and `SCFIND_CXX11REGEX` when regex mode is enabled. `FindOnce`, `DoReplace`, and `DoReplaceAll` use `SCI_SETTARGETRANGE`, `SCI_SEARCHINTARGET`, and either `SCI_REPLACETARGET` or `SCI_REPLACETARGETRE`.

`DoFindInFiles` creates a `FifCtx`, clears previous hits, and calls `ScanDir`. `ScanDir` recursively enumerates matching files with `FindFirstFileW`/`FindNextFileW`, avoids reparse points, and limits recursion to depth 32. `ScanFile` reads a bounded file, detects encoding, converts it to UTF-8, loads it into the hidden Scintilla scratch document, searches with `SCI_SEARCHINTARGET`, and optionally rewrites matching files. Results are stored as paths and line numbers and displayed in the results list; double-clicking a result opens the file and positions the editor.

### 4. Multi-cursor and column editing

`SetupView` enables Scintilla multiple selections with `SCI_SETMULTIPLESELECTION`, allows typing into additional selections with `SCI_SETADDITIONALSELECTIONTYPING`, and selects per-caret paste behaviour with `SCI_SETMULTIPASTE`. Rectangular selection and virtual space are enabled, with Alt configured as the rectangular-selection modifier. Menu commands call `SCI_MULTIPLESELECTADDNEXT` and `SCI_MULTIPLESELECTADDEACH` for occurrence selection. `ColumnNumbers` reads all selection ranges, sorts them top-to-bottom, and replaces them back-to-front so inserting numbers does not invalidate earlier positions.

### 5. Encoding and line-ending control

`textutil.c` detects UTF-8, UTF-8 with BOM, UTF-16 little-endian, UTF-16 big-endian, and ANSI input. It validates UTF-8 sequences, recognizes BOMs, detects the first line-ending convention, and converts UTF-16 surrogate pairs to UTF-8 code points. The reverse conversion encodes non-BMP code points as UTF-16 surrogate pairs and replaces invalid surrogate-range code points with U+FFFD.

`LoadIntoCurrent` detects the input encoding at the file boundary and converts non-UTF-8 data to the editor's internal UTF-8 representation. `WriteEncoded` converts the Scintilla text back to the document's selected encoding and warns interactively when ANSI conversion would be lossy. `ChangeEncoding` changes the save encoding and marks the document dirty. `ChangeEol` updates Scintilla's EOL mode and calls `SCI_CONVERTEOLS` to rewrite line endings.

### 6. Macro recording and playback

Scintilla emits `SCN_MACRORECORD` notifications for recorded editor messages. `OnScintilla` forwards these notifications to `MacroRecord`, which stores the message, parameters, and a copy of text payloads for text-bearing messages such as `SCI_ADDTEXT`, `SCI_APPENDTEXT`, `SCI_REPLACESEL`, and search commands. `MacroPlayOnce` replays each stored message through the active Scintilla control. `MacroPlay` groups playback into one undo action and supports one-time playback, a requested repeat count, or repetition until the caret stops moving, reaches the document end, or reaches its guard limit.

### 7. DLL-based plugin system

`LoadPlugins` scans the executable's `plugins` directory for DLLs. Each candidate is loaded with `LoadLibraryW`, and the required exports are resolved with `GetProcAddress`: `mininpp_init`, `mininpp_name`, `mininpp_command_count`, `mininpp_command_name`, and `mininpp_run`. Valid plugins receive a `MiniHost` structure containing the main window, the Scintilla window, and a callback that forwards Scintilla messages. Plugin commands are registered under the Plugins menu and dispatched by index.

The example `plugins/jsonfmt.c` plugin exports the required interface. It reformats or minifies the entire document by scanning characters, preserving quoted strings and escapes, and replacing the document in one undo action. It intentionally does not validate JSON syntax.

### 8. Line tools

`LineTool` determines either the selected line range or the entire document, obtains a direct Scintilla range pointer, and delegates transformation to the portable helpers. The menu routes sorting to `tu_sort_lines`, duplicate removal to `tu_unique_lines`, blank-line removal to `tu_remove_empty_lines`, and trailing-space cleanup to `tu_trim_trailing`. The transformed buffer is installed with `SCI_SETTARGETRANGE` and `SCI_REPLACETARGET` inside one undo action.

Separate Scintilla commands handle line duplication, deletion, moving lines up or down, and upper/lower casing.

### 9. Auto-completion and bracket matching

`AutoComplete` reads the current word prefix from Scintilla, requiring at least three characters and limiting the scan to documents no larger than 4 MiB. It walks the document's character pointer, extracts words beginning with the prefix, removes duplicates, and presents up to 60 candidates through `SCI_AUTOCSHOW`. `OnCharAdded` invokes it for word characters and also performs simple previous-line indentation for new lines.

`MatchBraces`, triggered on selection updates, checks the character at or immediately before the caret. It calls `SCI_BRACEMATCH` and highlights a matching pair with `SCI_BRACEHIGHLIGHT`, or marks an unmatched brace with `SCI_BRACEBADLIGHT`.

### 10. Large-file handling

`LoadIntoCurrent` opens files with sequential-scan semantics, reads a 64 KiB sample for encoding detection, and then positions the file after any BOM. UTF-8 files are streamed directly into Scintilla in 1 MiB chunks using `SCI_APPENDTEXT`, avoiding a full second in-memory copy. UTF-16 and ANSI files are converted through a bounded whole-file path controlled by `CONVERT_LIMIT` (512 MiB), then appended to Scintilla in 1 MiB pieces.

Scintilla documents larger than 1 GiB are created with `SC_DOCUMENTOPTION_TEXT_LARGE`, while syntax lexing is disabled above 24 MiB. Find-in-files separately skips files above its 64 MiB search limit.

## 5. The Portable Core (`textutil.c`)

`textutil.c` contains functionality that does not require Win32 or Scintilla:

- UTF-8 validation and encoding detection, including BOM recognition.
- First-EOL detection and conversion to canonical EOL strings.
- UTF-16 little-endian and big-endian conversion, including supplementary-plane characters and surrogate-pair handling.
- Stable line transformations for sorting, reverse sorting, de-duplication, blank-line removal, and trailing-space trimming.

Keeping these routines independent makes them testable with a normal C99 compiler and prevents Windows UI concerns from contaminating the encoding and line-processing logic. The repository's portable test program exercises the restored core with 28 checks, including Unicode conversion and line operations.

## 6. Memory Management Strategy

The application uses `xalloc` and `xrealloc` for long-lived or application-critical allocations. Both wrappers convert zero-size requests to a one-byte allocation, display an out-of-memory message, and terminate the process if allocation fails. This keeps callers from silently continuing with invalid pointers. Temporary file buffers and conversion buffers use direct `malloc` with explicit failure checks and cleanup.

Scintilla owns the text buffers for open documents. MiniNPP owns the `Doc` records, session strings, backup contents while writing, macro payload copies, search results, and plugin bookkeeping. `Doc.ptr` is released through `SCI_RELEASEDOCUMENT` when a tab closes.

`WriteFileAtomic` is the save-integrity boundary. It writes one or two data segments to a temporary file, flushes the file buffers, closes the handle, and atomically replaces the target. If writing or replacement fails, the temporary file is removed and the original target is not intentionally replaced by a partial buffer.

## 7. Build System

`build.bat` selects local dependency paths:

- `SCI` points to `deps\scintilla\include`.
- `SCI_LIB` points to `deps\scintilla\bin`.
- `LEX` points to `deps\lexilla\include`.
- `LEX_LIB` points to `deps\lexilla\bin`.

The first GCC command builds `MiniNPP.exe` from `mininpp.c` and `textutil.c`, includes both dependency header directories, and links the Scintilla and Lexilla import libraries through `-lscintilla` and `-llexilla`. It also links the Win32 libraries required for controls, dialogs, shell/path utilities, COM/OLE, user-interface, and graphics functionality: `comctl32`, `comdlg32`, `shlwapi`, `shell32`, `ole32`, `user32`, and `gdi32`.

The second GCC command builds `plugins\jsonfmt.dll` as a shared library against the same Scintilla headers and the local project headers. At runtime, `MiniNPP.exe` loads `Scintilla.dll` and `Lexilla.dll` beside the executable, while plugins are discovered from the executable's `plugins` directory.
