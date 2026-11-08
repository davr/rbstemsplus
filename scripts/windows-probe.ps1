<#
RB Stems Plus: Windows probe (read-only)

Finds out how rekordbox 7 for Windows ships ONNX Runtime and its STEMS model, so that the Windows
version of RB Stems Plus can be built to match. It only reads files and the registry: it changes
nothing, needs no administrator rights, and sends nothing anywhere.

Run it in PowerShell (Windows PowerShell 5.1 or PowerShell 7):

    powershell -NoProfile -ExecutionPolicy Bypass -File .\windows-probe.ps1

It prints its report and also saves it as rbstemsplus-probe.txt on your Desktop, with your user
name replaced by <user>. Paste that file's contents back (or attach it).

Optional: -RekordboxDir "C:\Program Files\Pioneer\rekordbox 7.2.19" scans only that folder (the
one holding rekordbox.exe), ignoring other installed versions; without it, every rekordbox folder
the registry or Program Files lists is scanned. -Deep also scans every .dll in the folder for ONNX
Runtime imports and strings (slower).
#>
[CmdletBinding()]
param(
    [string]$RekordboxDir = "",
    [switch]$Deep
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'
$script:lines = New-Object System.Collections.Generic.List[string]

function Out-Line([string]$s = "") { $script:lines.Add($s); Write-Host $s }
function Out-Section([string]$s) { Out-Line ""; Out-Line ("=" * 78); Out-Line "== $s"; Out-Line ("=" * 78) }

function Get-Sha256([string]$path) {
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite,Delete')
        try { $h = $sha.ComputeHash($fs) } finally { $fs.Dispose() }
        return (($h | ForEach-Object { $_.ToString("x2") }) -join "")
    } catch { return "(unreadable: $($_.Exception.Message))" }
}

function Get-Signer([string]$path) {
    try {
        $sig = Get-AuthenticodeSignature -LiteralPath $path -ErrorAction Stop
        $subj = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { "(none)" }
        $thumb = if ($sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint } else { "" }
        return "status=$($sig.Status) signer=`"$subj`" thumbprint=$thumb"
    } catch { return "(signature not checked: $($_.Exception.Message))" }
}

function Get-VersionLine([string]$path) {
    try {
        $v = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        return "FileVersion=`"$($v.FileVersion)`" ProductVersion=`"$($v.ProductVersion)`" Product=`"$($v.ProductName)`" Company=`"$($v.CompanyName)`" Description=`"$($v.FileDescription)`""
    } catch { return "(no version resource)" }
}

# ---------------------------------------------------------------- PE parsing (exports, imports)

function Read-At($fs, [long]$off, [int]$len) {
    $buf = New-Object byte[] $len
    $fs.Position = $off
    $got = 0
    while ($got -lt $len) {
        $n = $fs.Read($buf, $got, $len - $got)
        if ($n -le 0) { break }
        $got += $n
    }
    return ,$buf
}

function Read-CString($fs, [long]$off, [int]$max = 512) {
    if ($off -lt 0) { return "" }
    $b = Read-At $fs $off $max
    $end = [Array]::IndexOf($b, [byte]0)
    if ($end -lt 0) { $end = $b.Length }
    return [System.Text.Encoding]::ASCII.GetString($b, 0, $end)
}

function Open-PE([string]$path) {
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite,Delete')
    $dos = Read-At $fs 0 64
    if ($dos[0] -ne 0x4D -or $dos[1] -ne 0x5A) { $fs.Dispose(); throw "not a PE file" }
    $pe = [BitConverter]::ToUInt32($dos, 0x3C)
    $hdr = Read-At $fs $pe 24
    if ([BitConverter]::ToUInt32($hdr, 0) -ne 0x4550) { $fs.Dispose(); throw "no PE signature" }
    $machine = [BitConverter]::ToUInt16($hdr, 4)
    $nsec = [BitConverter]::ToUInt16($hdr, 6)
    $optSize = [BitConverter]::ToUInt16($hdr, 20)
    $opt = Read-At $fs ($pe + 24) $optSize
    $magic = [BitConverter]::ToUInt16($opt, 0)
    $pe64 = ($magic -eq 0x20B)
    $ddOff = if ($pe64) { 112 } else { 96 }
    $dirs = @()
    for ($i = 0; $i -lt 16; $i++) {
        $o = $ddOff + 8 * $i
        if ($o + 8 -le $opt.Length) { $dirs += ,@([BitConverter]::ToUInt32($opt, $o), [BitConverter]::ToUInt32($opt, $o + 4)) }
        else { $dirs += ,@(0, 0) }
    }
    $secs = @()
    $sb = Read-At $fs ($pe + 24 + $optSize) (40 * $nsec)
    for ($i = 0; $i -lt $nsec; $i++) {
        $o = 40 * $i
        $secs += ,@([BitConverter]::ToUInt32($sb, $o + 12), [BitConverter]::ToUInt32($sb, $o + 8), [BitConverter]::ToUInt32($sb, $o + 16), [BitConverter]::ToUInt32($sb, $o + 20))
    }
    return @{ fs = $fs; pe64 = $pe64; machine = $machine; dirs = $dirs; secs = $secs }
}

function RvaToOff($pe, [uint32]$rva) {
    foreach ($s in $pe.secs) {
        $va = $s[0]; $size = [Math]::Max($s[1], $s[2])
        if ($rva -ge $va -and $rva -lt $va + $size) { return [long]($rva - $va + $s[3]) }
    }
    return [long]-1
}

function Get-Exports([string]$path) {
    $pe = Open-PE $path
    try {
        $rva = $pe.dirs[0][0]; $size = $pe.dirs[0][1]
        if ($rva -eq 0) { return @() }
        $off = RvaToOff $pe $rva
        $d = Read-At $pe.fs $off 40
        $nFuncs = [BitConverter]::ToUInt32($d, 20)
        $nNames = [BitConverter]::ToUInt32($d, 24)
        $funcs = RvaToOff $pe ([BitConverter]::ToUInt32($d, 28))
        $names = RvaToOff $pe ([BitConverter]::ToUInt32($d, 32))
        $ords = RvaToOff $pe ([BitConverter]::ToUInt32($d, 36))
        $base = [BitConverter]::ToUInt32($d, 16)
        $out = @()
        if ($nNames -gt 5000) { $nNames = 5000 }
        $nb = Read-At $pe.fs $names (4 * $nNames)
        $ob = Read-At $pe.fs $ords (2 * $nNames)
        $fb = Read-At $pe.fs $funcs (4 * [Math]::Min($nFuncs, 65535))
        for ($i = 0; $i -lt $nNames; $i++) {
            $name = Read-CString $pe.fs (RvaToOff $pe ([BitConverter]::ToUInt32($nb, 4 * $i)))
            $ord = [BitConverter]::ToUInt16($ob, 2 * $i)
            $frva = [BitConverter]::ToUInt32($fb, 4 * $ord)
            $fwd = ""
            if ($frva -ge $rva -and $frva -lt $rva + $size) { $fwd = " -> forwarded to " + (Read-CString $pe.fs (RvaToOff $pe $frva)) }
            $out += ("{0} (ordinal {1}){2}" -f $name, ($ord + $base), $fwd)
        }
        return $out
    } finally { $pe.fs.Dispose() }
}

function Read-Thunks($pe, [uint32]$rva) {
    $out = @()
    if ($rva -eq 0) { return $out }
    $off = RvaToOff $pe $rva
    if ($off -lt 0) { return $out }
    $w = if ($pe.pe64) { 8 } else { 4 }
    for ($i = 0; $i -lt 4096; $i++) {
        $b = Read-At $pe.fs ($off + $w * $i) $w
        $zero = $true
        foreach ($x in $b) { if ($x -ne 0) { $zero = $false; break } }
        if ($zero) { break }
        # the top bit of the thunk: import by ordinal (read from the byte: no 64-bit bitwise maths,
        # which Windows PowerShell 5.1 can't do on values with the top bit set)
        if (($b[$w - 1] -band 0x80) -ne 0) { $out += ("ordinal " + [BitConverter]::ToUInt16($b, 0)) }
        else {
            $nameRva = [uint32]([BitConverter]::ToUInt32($b, 0) -band 0x7FFFFFFF)
            $out += (Read-CString $pe.fs ((RvaToOff $pe $nameRva) + 2))
        }
    }
    return $out
}

# @{ dll name = list of function names }, normal and delay-load imports
function Get-Imports([string]$path) {
    $pe = Open-PE $path
    $res = [ordered]@{}
    try {
        $rva = $pe.dirs[1][0]
        if ($rva -ne 0) {
            $off = RvaToOff $pe $rva
            for ($i = 0; $i -lt 1024; $i++) {
                $d = Read-At $pe.fs ($off + 20 * $i) 20
                $oft = [BitConverter]::ToUInt32($d, 0); $nameRva = [BitConverter]::ToUInt32($d, 12); $ft = [BitConverter]::ToUInt32($d, 16)
                if ($nameRva -eq 0 -and $ft -eq 0) { break }
                $dll = Read-CString $pe.fs (RvaToOff $pe $nameRva)
                $res[$dll] = @(Read-Thunks $pe ($(if ($oft) { $oft } else { $ft })))
            }
        }
        $rva = $pe.dirs[13][0]
        if ($rva -ne 0) {
            $off = RvaToOff $pe $rva
            for ($i = 0; $i -lt 1024; $i++) {
                $d = Read-At $pe.fs ($off + 32 * $i) 32
                $nameRva = [BitConverter]::ToUInt32($d, 4); $int = [BitConverter]::ToUInt32($d, 16)
                if ($nameRva -eq 0) { break }
                $dll = Read-CString $pe.fs (RvaToOff $pe $nameRva)
                $res["$dll (delay-load)"] = @(Read-Thunks $pe $int)
            }
        }
    } finally { $pe.fs.Dispose() }
    return $res
}

function Get-Machine([string]$path) {
    try { $pe = Open-PE $path; $m = $pe.machine; $pe.fs.Dispose() } catch { return "?" }
    switch ($m) { 0x8664 { "x64" } 0xAA64 { "arm64" } 0x14C { "x86" } default { "0x{0:x}" -f $m } }
}

# strings that tell how a binary finds ONNX Runtime and the model (ASCII and UTF-16)
function Find-Strings([string]$path, [string[]]$needles) {
    $found = @()
    try {
        $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite,Delete')
        try {
            $chunk = 16MB; $pos = 0L
            while ($pos -lt $fs.Length) {
                $len = [int][Math]::Min([long]$chunk + 4096, $fs.Length - $pos)
                $b = Read-At $fs $pos $len
                $a = [System.Text.Encoding]::ASCII.GetString($b)
                $u = [System.Text.Encoding]::Unicode.GetString($b)
                $u2 = if ($b.Length -gt 1) { [System.Text.Encoding]::Unicode.GetString($b, 1, $b.Length - 1) } else { "" }
                foreach ($n in $needles) {
                    foreach ($pair in @(@($a, "ascii"), @($u, "utf16"), @($u2, "utf16"))) {
                        $text = $pair[0]; $idx = 0
                        while (($idx = $text.IndexOf($n, $idx, [StringComparison]::OrdinalIgnoreCase)) -ge 0) {
                            $s = [Math]::Max(0, $idx - 60); $e = [Math]::Min($text.Length, $idx + $n.Length + 60)
                            $ctx = ($text.Substring($s, $e - $s) -replace '[^\x20-\x7E]', '.')
                            $found += "[$($pair[1])] $ctx"
                            $idx += $n.Length
                            if ($found.Count -gt 60) { break }
                        }
                    }
                }
                $pos += $chunk
            }
        } finally { $fs.Dispose() }
    } catch { $found += "(not read: $($_.Exception.Message))" }
    return ($found | Select-Object -Unique | Select-Object -First 40)
}

function Describe-Binary([string]$path, [switch]$WithExports, [switch]$WithImports, [switch]$WithStrings) {
    $fi = Get-Item -LiteralPath $path
    Out-Line ""
    Out-Line "--- $path"
    Out-Line "    size=$($fi.Length) modified=$($fi.LastWriteTimeUtc.ToString('u')) machine=$(Get-Machine $path)"
    Out-Line "    $(Get-VersionLine $path)"
    Out-Line "    sha256=$(Get-Sha256 $path)"
    Out-Line "    $(Get-Signer $path)"
    try {
        $acl = Get-Acl -LiteralPath $path
        Out-Line "    owner=$($acl.Owner)"
    } catch {}
    if ($WithExports) {
        try {
            $ex = @(Get-Exports $path)
            Out-Line "    exports ($($ex.Count)):"
            foreach ($e in $ex) { Out-Line "      $e" }
        } catch { Out-Line "    exports: (not read: $($_.Exception.Message))" }
    }
    if ($WithImports) {
        try {
            $im = Get-Imports $path
            Out-Line "    imports ($($im.Count) DLLs):"
            foreach ($k in $im.Keys) {
                $fns = @($im[$k])
                if ($k -match 'onnx|directml|dml|ort') {
                    Out-Line "      $k  <== $($fns.Count) functions:"
                    foreach ($f in $fns) { Out-Line "          $f" }
                } else {
                    Out-Line "      $k ($($fns.Count))"
                }
            }
        } catch { Out-Line "    imports: (not read: $($_.Exception.Message))" }
    }
    if ($WithStrings) {
        $st = @(Find-Strings $path @('onnxruntime', 'hdemucs', 'demucs3', 'DirectML', 'DmlExecutionProvider', 'OrtGetApiBase', 'AppendExecutionProvider', '.onnx'))
        Out-Line "    strings ($($st.Count) shown):"
        foreach ($s in $st) { Out-Line "      $s" }
    }
}

# ---------------------------------------------------------------- the report

Out-Section "RB Stems Plus Windows probe"
Out-Line "date (UTC)    : $((Get-Date).ToUniversalTime().ToString('u'))"
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Out-Line "Windows       : $($os.Caption) $($os.Version) build $($os.BuildNumber) ($($os.OSArchitecture))"
} catch { Out-Line "Windows       : $([Environment]::OSVersion.VersionString)" }
Out-Line "CPU arch      : $($env:PROCESSOR_ARCHITECTURE)"
Out-Line "PowerShell    : $($PSVersionTable.PSVersion)"
try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $admin = (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Out-Line "elevated now  : $admin"
} catch {}
Out-Line "APPDATA       : $env:APPDATA"
Out-Line "LOCALAPPDATA  : $env:LOCALAPPDATA"
Out-Line "ProgramData   : $env:ProgramData"

# rekordbox installs, from the uninstall entries
Out-Section "rekordbox installs (registry)"
$installs = New-Object System.Collections.Generic.List[string]
$keys = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')
foreach ($k in $keys) {
    Get-ItemProperty -Path $k -ErrorAction SilentlyContinue | Where-Object {
        ($_.PSObject.Properties['DisplayName'] -and $_.DisplayName -match 'rekordbox|Pioneer|AlphaTheta')
    } | ForEach-Object {
        $loc = if ($_.PSObject.Properties['InstallLocation']) { $_.InstallLocation } else { "" }
        $ver = if ($_.PSObject.Properties['DisplayVersion']) { $_.DisplayVersion } else { "" }
        $pub = if ($_.PSObject.Properties['Publisher']) { $_.Publisher } else { "" }
        $un = if ($_.PSObject.Properties['UninstallString']) { $_.UninstallString } else { "" }
        Out-Line "$($_.DisplayName) | version=$ver | publisher=$pub"
        Out-Line "    key=$($_.PSPath -replace '^.*::','')"
        Out-Line "    InstallLocation=$loc"
        Out-Line "    UninstallString=$un"
        if ($loc -and (Test-Path -LiteralPath $loc)) { $installs.Add($loc.TrimEnd('\')) }
    }
}
foreach ($k in @('HKLM:\SOFTWARE\Pioneer', 'HKLM:\SOFTWARE\WOW6432Node\Pioneer', 'HKCU:\SOFTWARE\Pioneer', 'HKLM:\SOFTWARE\AlphaTheta', 'HKCU:\SOFTWARE\AlphaTheta')) {
    if (Test-Path $k) {
        Out-Line "registry $k :"
        Get-ChildItem -Path $k -Recurse -ErrorAction SilentlyContinue | Select-Object -First 40 | ForEach-Object {
            $p = $_
            $vals = ($p.Property | ForEach-Object { "$_=$($p.GetValue($_))" }) -join '; '
            Out-Line "    $($p.Name -replace '^HKEY_[A-Z_]+\\','') : $vals"
        }
    }
}

# the folder given (and only that one), else the registry's and the usual folders
Out-Section "rekordbox folders"
if ($RekordboxDir) {
    $given = $RekordboxDir.Trim().Trim('"').TrimEnd('\')
    if (-not (Test-Path -LiteralPath $given -PathType Container)) {
        Out-Line "-RekordboxDir `"$given`" isn't a folder: check the path and run again."
        $installs = @()
    } else {
        $given = (Resolve-Path -LiteralPath $given).ProviderPath.TrimEnd('\')
        Out-Line "only the folder given with -RekordboxDir is scanned (the registry entries above are listed, not scanned)"
        if (-not (Get-ChildItem -LiteralPath $given -Filter 'rekordbox*.exe' -File -ErrorAction SilentlyContinue)) {
            Out-Line "note: no rekordbox*.exe directly in it; it's scanned anyway"
        }
        $installs = @($given)
    }
} else {
    foreach ($root in @("$env:ProgramFiles\Pioneer", "${env:ProgramFiles(x86)}\Pioneer", "$env:ProgramFiles\AlphaTheta", "$env:ProgramFiles\rekordbox")) {
        if ($root -and (Test-Path -LiteralPath $root)) {
            Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                if ($_.Name -match 'rekordbox') { $installs.Add($_.FullName) }
            }
        }
    }
    $installs = @($installs | Select-Object -Unique)
}
if ($installs.Count -eq 0) { Out-Line "no rekordbox folder found: run again with -RekordboxDir `"<folder holding rekordbox.exe>`"" }
foreach ($d in $installs) { Out-Line "folder: $d" }

Out-Section "rekordbox processes running now"
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'rekordbox|Pioneer|AlphaTheta' } | ForEach-Object {
    $path = "?"
    try { $path = $_.Path } catch {}
    Out-Line "$($_.ProcessName) pid=$($_.Id) path=$path"
    try {
        $_.Modules | Where-Object { $_.ModuleName -match 'onnx|directml|dml' } | ForEach-Object { Out-Line "    loaded module: $($_.FileName)" }
    } catch { Out-Line "    (its modules couldn't be listed: $($_.Exception.Message))" }
}

foreach ($d in $installs) {
    Out-Section "In $d"
    # top-level listing: names and sizes only
    Out-Line "top level:"
    Get-ChildItem -LiteralPath $d -Force -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object {
        if ($_.PSIsContainer) { Out-Line ("    <dir>       {0}" -f $_.Name) }
        else { Out-Line ("    {0,11} {1}" -f $_.Length, $_.Name) }
    }
    $all = @(Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue)
    Out-Line ""
    Out-Line "files under it: $($all.Count)"

    $exes = @($all | Where-Object { $_.Name -match '^rekordbox.*\.exe$' })
    foreach ($e in $exes) { Describe-Binary $e.FullName -WithImports -WithStrings }

    $orts = @($all | Where-Object { $_.Name -match 'onnx|directml|dml|ort[_.]' -and $_.Extension -match '\.(dll|so|exe)$' })
    Out-Line ""
    Out-Line "ONNX Runtime / DirectML related binaries: $($orts.Count)"
    foreach ($o in $orts) { Describe-Binary $o.FullName -WithExports -WithImports }

    $models = @($all | Where-Object { $_.Extension -match '\.(onnx|ort|pt|bin|mlmodel|mlmodelc)$' -or $_.Name -match 'demucs|stem' })
    if ($models.Count) {
        Out-Line ""
        Out-Line "model-like files in the install folder:"
        foreach ($m in ($models | Select-Object -First 50)) { Out-Line ("    {0,11} {1}  sha256={2}" -f $m.Length, $m.FullName, (Get-Sha256 $m.FullName)) }
    }

    # which binaries import ONNX Runtime (rekordbox.exe itself, or a plugin DLL)
    Out-Line ""
    Out-Line "binaries importing an onnxruntime DLL:"
    $cands = if ($Deep) { @($all | Where-Object { $_.Extension -match '\.(dll|exe)$' }) } else { @($all | Where-Object { $_.Extension -match '\.(dll|exe)$' -and $_.Length -lt 64MB }) }
    $hits = 0
    foreach ($c in $cands) {
        try {
            $im = Get-Imports $c.FullName
            foreach ($k in $im.Keys) {
                if ($k -match 'onnxruntime') {
                    $hits++
                    Out-Line "    $($c.FullName) imports $k : $((@($im[$k])) -join ', ')"
                }
            }
        } catch {}
    }
    if ($hits -eq 0) { Out-Line "    none found (rekordbox may load it at run time with LoadLibrary: see the strings above)" }
    if ($Deep) {
        Out-Line ""
        Out-Line "binaries mentioning onnxruntime or hdemucs (deep scan):"
        foreach ($c in $cands) {
            $st = @(Find-Strings $c.FullName @('onnxruntime', 'hdemucs', 'OrtGetApiBase'))
            if ($st.Count -and $st[0] -notmatch '^\(not read') { Out-Line "    $($c.FullName):"; foreach ($s in ($st | Select-Object -First 5)) { Out-Line "        $s" } }
        }
    }
}

# the STEMS model and engine files rekordbox downloads
Out-Section "STEMS model and engine (downloaded by rekordbox)"
$dataRoots = @("$env:APPDATA\Pioneer", "$env:LOCALAPPDATA\Pioneer", "$env:ProgramData\Pioneer", "$env:APPDATA\AlphaTheta", "$env:LOCALAPPDATA\AlphaTheta", "$env:ProgramData\AlphaTheta")
foreach ($r in $dataRoots) {
    if (-not (Test-Path -LiteralPath $r)) { Out-Line "(none) $r"; continue }
    Out-Line ""
    Out-Line "$r :"
    Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue | ForEach-Object { Out-Line "    <dir> $($_.Name)" }
    $f = @(Get-ChildItem -LiteralPath $r -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object {
        $_.Extension -match '\.(onnx|ort|mlmodel)$' -or $_.Name -match 'demucs|stem|_ver\.txt$|model' -or $_.DirectoryName -match '\\models(\\|$)'
    })
    foreach ($m in ($f | Select-Object -First 80)) {
        $line = "    {0,11} {1}" -f $m.Length, $m.FullName
        if ($m.Length -gt 1MB -or $m.Extension -match '\.(onnx|ort)$') { $line += "  sha256=" + (Get-Sha256 $m.FullName) }
        Out-Line $line
        if ($m.Length -lt 4096 -and $m.Extension -match '\.(txt|json|ini|cfg|xml)$') {
            try { (Get-Content -LiteralPath $m.FullName -TotalCount 20 -ErrorAction Stop) | ForEach-Object { Out-Line "        | $_" } } catch {}
        }
    }
}
$mdl = Join-Path $env:APPDATA 'Pioneer\rekordbox6\models'
Out-Line ""
Out-Line "expected model folder (as on macOS): $mdl  exists=$(Test-Path -LiteralPath $mdl)"

# where rekordbox keeps its own settings and logs (names only)
Out-Section "rekordbox settings folders (names only)"
foreach ($r in @("$env:APPDATA\Pioneer\rekordbox6", "$env:APPDATA\Pioneer\rekordbox", "$env:LOCALAPPDATA\Pioneer")) {
    if (Test-Path -LiteralPath $r) {
        Out-Line "$r :"
        Get-ChildItem -LiteralPath $r -Force -ErrorAction SilentlyContinue | Select-Object -First 60 | ForEach-Object {
            if ($_.PSIsContainer) { Out-Line "    <dir> $($_.Name)" } else { Out-Line ("    {0,11} {1}" -f $_.Length, $_.Name) }
        }
    }
}

# save, with the user name replaced
Out-Section "end"
$text = ($script:lines -join "`r`n")
if ($env:USERNAME) { $text = $text -replace [regex]::Escape($env:USERNAME), '<user>' }
try {
    $desk = [Environment]::GetFolderPath('Desktop')
    if (-not $desk) { $desk = $env:TEMP }
    $out = Join-Path $desk 'rbstemsplus-probe.txt'
    [System.IO.File]::WriteAllText($out, $text, (New-Object System.Text.UTF8Encoding $false))
    Write-Host ""
    Write-Host "Saved: $out  (your user name is replaced by <user>). Please send that file back." -ForegroundColor Green
} catch { Write-Host "Couldn't save the report: $($_.Exception.Message). Copy the text above instead." -ForegroundColor Yellow }
