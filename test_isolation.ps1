<#
.SYNOPSIS
    Verifies that MININPP_CONFIG_DIR isolates MiniNPP's config data from the
    user's real %APPDATA%.

    Without this hook MiniNPP resolves its config directory with
    SHGetFolderPathW(CSIDL_APPDATA), which reads the *registered* AppData path
    and ignores any override of the APPDATA environment variable — so every
    manual test launch rewrote the real %APPDATA%\MiniNPP\session.txt.

    This script:
      1. Clears stray MiniNPP processes (a running instance holds the
         single-instance mutex and would make the new instance forward and exit).
      2. Records the real session.txt state so we can prove it is untouched.
      3. Launches MiniNPP.exe with MININPP_CONFIG_DIR set to a temp folder.
      4. Closes it gracefully; WM_CLOSE calls SaveSession() immediately, so the
         test is deterministic instead of waiting for the 5s session timer.
      5. Asserts session.txt (and backup\) appeared in the temp folder only.
      6. Cleans up the temp folder and restores the environment variable.

.EXAMPLE
    .\test_isolation.ps1
    # Exit code 0 = isolation holds, 1 = failure.
#>

[CmdletBinding()]
param (
    # How many seconds to wait for MiniNPP to start and to write the session.
    [int]$Timeout = 15
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$exePath = Join-Path $scriptDir "MiniNPP.exe"

$pass = $true
function Assert-True {
    param([string]$Message, [string]$Detail)
    if (-not $pass) { return }   # report first failure only, then unwind
    $script:pass = $false
    Write-Host "  FAIL: $Message" -ForegroundColor Red
    if ($Detail) { Write-Host "        $Detail" -ForegroundColor Red }
}

Write-Host "[1/7] Pre-flight: cleaning stray MiniNPP processes..."
# A running instance holds the Global\ named mutex; without this the new
# instance would forward to it and exit immediately, and the test would fail.
Get-Process -Name MiniNPP -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (-not (Test-Path $exePath)) {
    Assert-True "MiniNPP.exe not found at $exePath" "Build it first with build.bat"
    exit 1
}
Write-Host "  using $exePath"

# --- Snapshot the real session file so we can prove it is untouched ---------
$appData = [Environment]::GetFolderPath("ApplicationData")
$realDir = Join-Path $appData "MiniNPP"
$realSession = Join-Path $realDir "session.txt"

$realExistedBefore = Test-Path $realSession
$realMtimeBefore = if ($realExistedBefore) {
    (Get-Item $realSession).LastWriteTimeUtc
} else { $null }
Write-Host "[2/7] Real session path: $realSession"
Write-Host "       existed before: $realExistedBefore"

# --- Prepare an isolated temp config dir ------------------------------------
$configDir = Join-Path ([IO.Path]::GetTempPath()) "MiniNPPTest_$([Guid]::NewGuid().ToString('N').Substring(0,12))"
Write-Host "[3/7] Isolated config dir: $configDir"
try {
    [System.IO.Directory]::CreateDirectory($configDir) | Out-Null
} catch {
    Assert-True "could not create temp config dir" "$_"
}

$originalEnv = $env:MININPP_CONFIG_DIR
$env:MININPP_CONFIG_DIR = $configDir

try {
    Write-Host "[4/7] Launching MiniNPP.exe..."
    $proc = Start-Process -FilePath $exePath -PassThru -WindowStyle Normal
    if (-not $proc) {
        Assert-True "MiniNPP.exe did not start"
        return
    }

    # Wait for the app to finish starting up (InitPaths + window creation).
    # Polling MainWindowHandle does not work: it reads 0 until the window is
    # created, so a naive poll breaks out immediately and never issues the close.
    Start-Sleep -Milliseconds 2000
    $proc.Refresh()
    if ($proc.HasExited) {
        # Vanished before its window appeared: almost certainly the
        # single-instance guard forwarded this instance to an already running
        # MiniNPP (see the pre-flight cleanup in step 1).
        Assert-True "MiniNPP exited immediately (code $($proc.ExitCode)); another instance may still be running" "PID $($proc.Id)"
        return
    }
    Write-Host "  running (PID $($proc.Id))"

    Write-Host "[5/7] Closing gracefully (WM_CLOSE -> SaveSession)..."
    # CloseMainWindow posts WM_CLOSE; MainProc's WM_CLOSE handler calls
    # SaveSession() before DestroyWindow, so the session is written now.
    $proc.CloseMainWindow() | Out-Null
    $exitDeadline = [DateTime]::Now.AddSeconds($Timeout)
    while ([DateTime]::Now -lt $exitDeadline) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $proc.HasExited) {
        Write-Host "  still running; forcing cleanup..." -ForegroundColor Yellow
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        $proc.WaitForExit($Timeout * 1000) | Out-Null
    }
    Write-Host "  exited (code $($proc.ExitCode))"

    # --- Verify the session landed in the isolated dir -----------------------
    Write-Host "[6/7] Verifying isolation..."
    # The env var value is the config directory itself (no MiniNPP subfolder is
    # appended); only the %APPDATA% fallback needs that extra level.
    $isolatedSession = Join-Path $configDir "session.txt"
    $isolatedBackup  = Join-Path $configDir "backup"

    if (-not (Test-Path $isolatedSession)) {
        Assert-True "session.txt was not written to the isolated config dir" "expected: $isolatedSession"
    } else {
        $len = (Get-Item $isolatedSession).Length
        Write-Host "  PASS: $isolatedSession ($len bytes)"
        if ($len -eq 0) {
            Assert-True "session.txt is empty" "SaveSession() should write JSON"
        }
    }

    if (-not (Test-Path $isolatedBackup)) {
        Assert-True "backup\ folder was not created under the isolated config dir" "expected: $isolatedBackup"
    } else {
        Write-Host "  PASS: $isolatedBackup exists"
    }

    # --- Prove the real %APPDATA% was never touched -------------------------
    if ($realExistedBefore) {
        $realMtimeAfter = (Get-Item $realSession).LastWriteTimeUtc
        if ($realMtimeAfter -gt $realMtimeBefore) {
            Assert-True "real session.txt was modified by the test run" "before: $realMtimeBefore  after: $realMtimeAfter"
        } else {
            Write-Host "  PASS: real session.txt pre-existed and was not modified"
        }
    } else {
        if (Test-Path $realSession) {
            Assert-True "real session.txt was created by the test run" "expected it to remain absent"
        } else {
            Write-Host "  PASS: real session.txt was not created (was absent before)"
        }
    }
} finally {
    Write-Host "[7/7] Cleanup..."
    $env:MININPP_CONFIG_DIR = $originalEnv
    if ($originalEnv -eq $null) { Remove-Item Env:\MININPP_CONFIG_DIR -ErrorAction SilentlyContinue }

    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }

    # Give the OS a moment to release file handles before deleting.
    for ($i = 0; $i -lt 5; $i++) {
        try {
            if (Test-Path $configDir) { Remove-Item -Recurse -Force -Path $configDir }
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (Test-Path $configDir) {
        Write-Host "  warning: could not remove $configDir (handle still held)" -ForegroundColor Yellow
    } else {
        Write-Host "  removed $configDir"
    }
}

Write-Host ""
if ($pass) {
    Write-Host "RESULT: PASS — MININPP_CONFIG_DIR isolation holds." -ForegroundColor Green
    exit 0
} else {
    Write-Host "RESULT: FAIL — see details above." -ForegroundColor Red
    exit 1
}