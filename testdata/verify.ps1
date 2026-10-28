function Show-HeadHex([string] $Path, [int] $Count = 32) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    (($bytes | Select-Object -First $Count | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}
function Get-Bom([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { return 'UTF-8 BOM' }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { return 'UTF-16 LE BOM' }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) { return 'UTF-16 BE BOM' }
    return 'No BOM'
}
function Count-Eol([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $cr = 0; $lf = 0; $crlf = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13) { $cr++; if ($i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10) { $crlf++ } }
        if ($bytes[$i] -eq 10) { $lf++ }
    }
    [pscustomobject]@{ CR = $cr; LF = $lf; CRLF = $crlf }
}
function Count-EolDecoded([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bom = Get-Bom $Path
    if ($bom -eq 'UTF-16 LE BOM') {
        $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
    } elseif ($bom -eq 'UTF-16 BE BOM') {
        $text = [Text.Encoding]::BigEndianUnicode.GetString($bytes, 2, $bytes.Length - 2)
    } elseif ($bom -eq 'UTF-8 BOM') {
        $text = [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
    } else {
        $text = [Text.Encoding]::UTF8.GetString($bytes)
    }
    $cr = 0; $lf = 0; $crlf = 0
    for ($i = 0; $i -lt $text.Length; $i++) {
        if ($text[$i] -eq [char]13) {
            $cr++
            if ($i + 1 -lt $text.Length -and $text[$i + 1] -eq [char]10) { $crlf++ }
        }
        if ($text[$i] -eq [char]10) { $lf++ }
    }
    [pscustomobject]@{ CR = $cr; LF = $lf; CRLF = $crlf }
}
function Compare-Text([string] $Path, [string] $Expected, [Text.Encoding] $Encoding = [Text.Encoding]::UTF8) {
    $actual = [IO.File]::ReadAllText($Path, $Encoding)
    return $actual -ceq $Expected
}
