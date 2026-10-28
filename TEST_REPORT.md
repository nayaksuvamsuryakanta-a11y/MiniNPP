# MiniNPP QA Test Report

Setup: PASS — build succeeded; portable suite passed 28 checks; five-second launch smoke test passed. The requested 36-check count does not match this checkout; `git log -p -- test_textutil.c` shows the current test file was introduced with 28 checks.

| ID | test | result | evidence | notes |
|---|---|---|---|---|
| T1 | Highlighting + folding | FAIL | User-confirmed: after T2 relaunch, status bar showed "Lexilla.dll not found - running without syntax highlighting"; sample.c had plain black text and no fold markers | Runtime dependency fix applied; T1 re-test pending |
| T2 | Tabs + session restore | BLOCKED | Restore state invalidated by a second MiniNPP instance: current session still has 4 tabs, sample.c and untitled dirty, and both payloads, but backup identities changed from earlier 2.txt/5.txt to 1.txt/4.txt | Restart T2 from the beginning with exactly one process; only read evidence before confirming the PID |
| T3 | Save integrity | NOT RUN | Awaiting user operation | |
| T4 | Find / Replace (regex) | NOT RUN | Awaiting user operation | |
| T5 | Find in Files + Replace in Files | NOT RUN | Awaiting user operation | Use filter `*.*`; expected 9 matching lines in 6 text files; binary.bin is skipped |
| T6 | Multi-cursor + column | NOT RUN | Awaiting user operation | |
| T7 | Encoding + EOL | NOT RUN | Awaiting user operation | Use ansi_cafe.txt for encoding; use a pristine numbers.txt copy for EOL counts: CRLF 10/10/10, LF 0/10/0, CR 10/0/0 |
| T8 | Macros | NOT RUN | Awaiting user operation | |
| T9 | Plugins | NOT RUN | Awaiting user operation | |
| T10 | Line tools | NOT RUN | Awaiting user operation | Restore lines.txt from testdata\\_pristine before each operation; expected output must use CRLF |
| T11 | Auto-complete + brackets | NOT RUN | Awaiting user operation | |
| T12 | Large file + tail -f | NOT RUN | Awaiting user operation | |
| T13 | Robustness | NOT RUN | Awaiting user operation | |

test_textutil.c in this workspace has 28 checks and a compiler warning; the original had 36 checks and no warning, so textutil.c, textutil.h, test_textutil.c and possibly mininpp_plugin.h may have been rewritten when files were missing. Compare against the original files before calling v1.0.

T1 correction: minified.json is one line and sample.xml has little to fold, so Fold all doing nothing on those files is expected. Only sample.c and sample.py must demonstrate folding; JSON becomes foldable after pretty-print in T9.

Fixture corrections: the nine mixed-EOL fixtures were regenerated with consistent UTF-8 BOM + CRLF endings. `Count-EolDecoded` must be used for UTF-16 files. `testdata\\_pristine` contains immutable copies of every fixture except `big.txt`; restore the needed files from it before T3, T4, T6, T7, T8, and T10.

T1/T2 Lexilla diagnosis: Lexilla.dll was present and all PE files were x64 (`Machine=0x8664`), and Lexilla exported plain `CreateLexer`. Explorer-style loading failed with error 126 because Lexilla.dll required `libgcc_s_seh-1.dll` and `libstdc++-6.dll`, which were not beside MiniNPP.exe or in System32. The approved fix copied `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, and `libwinpthread-1.dll` beside the executable. The reduced-PATH probe then reported `LoadLibraryW=OK` and `GetProcAddress(CreateLexer)=OK`; MiniNPP stayed alive for 5 seconds and exited cleanly via CloseMainWindow. API-set UCRT imports are supplied by Windows and required no additional copied DLLs.

Observation O1: multiple instances share one `%APPDATA%\\MiniNPP` session folder. RestoreSession deletes each restored dirty document's backup file, SaveSession rewrites `session.txt`, and PruneBackups deletes backup files not used by that instance's dirty documents. This creates a data-loss risk when a second instance starts while another holds unsaved tabs. Propose a single-instance guard after the test pass; no code change now.

T13 additional case: start a second MiniNPP instance while the first instance holds unsaved tabs, then verify whether session and backup state is preserved.
