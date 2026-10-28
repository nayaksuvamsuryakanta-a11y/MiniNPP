@echo off
set SCI=%~dp0deps\scintilla\include
set SCI_LIB=%~dp0deps\scintilla\bin
set LEX=%~dp0deps\lexilla\include
set LEX_LIB=%~dp0deps\lexilla\bin

gcc -O2 -Wall -mwindows -o MiniNPP.exe mininpp.c textutil.c -I"%SCI%" -I"%LEX%" -I. ^
    -L"%SCI_LIB%" -L"%LEX_LIB%" -lscintilla -llexilla -lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -lole32 -luser32 -lgdi32
if errorlevel 1 goto :fail

if not exist plugins mkdir plugins
gcc -O2 -Wall -shared -o plugins\jsonfmt.dll plugins\jsonfmt.c -I"%SCI%" -I"%LEX%" -I.
if errorlevel 1 goto :fail

echo.
echo Built MiniNPP.exe and plugins\jsonfmt.dll
goto :eof

:fail
echo.
echo Build failed.
exit /b 1
