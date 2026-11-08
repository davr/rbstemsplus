# RB Stems Plus for Windows: the install command.
#
#   irm https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.ps1 | iex
#
# Downloads payload-windows.json and its signature from this project's latest GitHub Release,
# checks the signature with the public keys written in below (scripts/build-windows.sh puts them
# here), checks the app's zip against the SHA-256 it names, then puts RB Stems Plus in
# %LOCALAPPDATA%\Programs\RB Stems Plus, adds it to the Start menu and opens it. No administrator
# rights: only Stems Cache asks for them later, through Windows's own prompt.
# Works in Windows PowerShell 5.1 (Windows 10 and 11) and PowerShell 7.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$ProgressPreference = 'Continue'

# written by scripts/build-windows.sh: the base64 DER of each trusted P-256 public key (space-
# separated), the oldest payload version this installer accepts, and where it downloads from
$SigningKeys = ""
$PayloadMinimum = ""
$DownloadBase = "https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/"

function Fail([string]$why) {
    Write-Host ""
    Write-Host "RB Stems Plus wasn't installed: $why" -ForegroundColor Red
    throw "RB Stems Plus wasn't installed"
}

if (-not $SigningKeys) { Fail "this copy of the installer has no signing keys (use the one from the release)." }
if (-not [Environment]::Is64BitOperatingSystem) { Fail "RB Stems Plus needs 64-bit Windows." }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.ServicePointManager]::SecurityProtocol

$script:StepCount = 7
$script:Step = 0
function Step([string]$what) {
    $script:Step++
    Write-Host ("[{0}/{1}] {2}" -f $script:Step, $script:StepCount, $what) -ForegroundColor Cyan
}
function Done([string]$what) { Write-Host "      $what" -ForegroundColor DarkGray }

function Format-Size([double]$bytes) {
    if ($bytes -ge 1MB) { return ("{0:N1} MB" -f ($bytes / 1MB)) }
    if ($bytes -ge 1KB) { return ("{0:N0} KB" -f ($bytes / 1KB)) }
    return ("{0} bytes" -f $bytes)
}

# Downloads a release file with a progress bar (size, percent, speed); its bytes.
function Get-Bytes([string]$name, [string]$what) {
    $u = [Uri]($DownloadBase + $name)
    try {
        $req = [Net.HttpWebRequest]::Create($u)
        $req.UserAgent = "RB Stems Plus installer"
        $req.AllowAutoRedirect = $true
        $resp = $req.GetResponse()
        $total = $resp.ContentLength
        $src = $resp.GetResponseStream()
        $buf = New-Object byte[] 262144
        $out = New-Object IO.MemoryStream
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $last = -1
        while (($n = $src.Read($buf, 0, $buf.Length)) -gt 0) {
            $out.Write($buf, 0, $n)
            $got = $out.Length
            $secs = [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
            $speed = (Format-Size ($got / $secs)) + "/s"
            if ($total -gt 0) {
                $pct = [int](100 * $got / $total)
                if ($pct -ne $last) {
                    Write-Progress -Activity "Downloading $what" -Status ("{0} of {1}, {2}%, {3}" -f (Format-Size $got), (Format-Size $total), $pct, $speed) -PercentComplete $pct
                    $last = $pct
                }
            } else {
                Write-Progress -Activity "Downloading $what" -Status ("{0}, {1}" -f (Format-Size $got), $speed)
            }
        }
        $src.Close(); $resp.Close()
        Write-Progress -Activity "Downloading $what" -Completed
        $bytes = $out.ToArray()
        Done ("downloaded {0} ({1})" -f $name, (Format-Size $bytes.Length))
        return ,$bytes
    } catch {
        Write-Progress -Activity "Downloading $what" -Completed
        Fail "couldn't download $name ($($_.Exception.Message)). Check your internet connection and try again."
    }
}

# ---------------------------------------------------------------- the signature (ECDSA P-256, SHA-256)

# A P-256 SubjectPublicKeyInfo (91 bytes) as a CNG ECCPublicBlob: "ECS1", 32, X, Y.
function ConvertTo-EccBlob([byte[]]$spki) {
    if ($spki.Length -ne 91 -or $spki[26] -ne 4) { return $null }
    $blob = New-Object byte[] 72
    [BitConverter]::GetBytes([uint32]0x31534345).CopyTo($blob, 0)
    [BitConverter]::GetBytes([uint32]32).CopyTo($blob, 4)
    [Array]::Copy($spki, 27, $blob, 8, 64)
    return ,$blob
}

# A DER ECDSA signature (SEQUENCE { INTEGER r, INTEGER s }) as r||s, 32 bytes each.
function ConvertFrom-DerSignature([byte[]]$der) {
    try {
        if ($der[0] -ne 0x30) { return $null }
        $i = 2
        if ($der[1] -band 0x80) { $i = 2 + ($der[1] -band 0x7f) }
        $out = New-Object byte[] 64
        foreach ($k in 0, 1) {
            if ($der[$i] -ne 0x02) { return $null }
            $len = $der[$i + 1]; $start = $i + 2
            while ($len -gt 32 -and $der[$start] -eq 0) { $start++; $len-- }
            if ($len -gt 32) { return $null }
            [Array]::Copy($der, $start, $out, 32 * $k + (32 - $len), $len)
            $i = $start + $len
        }
        return ,$out
    } catch { return $null }
}

function Test-Signature([byte[]]$data, [byte[]]$sig) {
    $rs = ConvertFrom-DerSignature $sig
    if ($null -eq $rs) { return $false }
    foreach ($k in ($SigningKeys -split ' ')) {
        if (-not $k) { continue }
        $blob = ConvertTo-EccBlob ([Convert]::FromBase64String($k))
        if ($null -eq $blob) { continue }
        $key = [Security.Cryptography.CngKey]::Import($blob, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
        $ecdsa = New-Object Security.Cryptography.ECDsaCng($key)
        $ecdsa.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
        if ($ecdsa.VerifyData($data, $rs)) { return $true }
    }
    return $false
}

# the same refusal of a key given twice as the app's (Payload.PlainJsonKeys): JSON readers disagree
# on which one counts
function Test-PlainJsonKeys([string]$json) {
    $stack = New-Object Collections.Stack
    $i = 0; $n = $json.Length; $expectKey = $false
    while ($i -lt $n) {
        $c = $json[$i]
        if ($c -eq '{') { $stack.Push(@{}); $expectKey = $true; $i++; continue }
        if ($c -eq '[') { $stack.Push($null); $expectKey = $false; $i++; continue }
        if ($c -eq '}' -or $c -eq ']') { [void]$stack.Pop(); $i++; continue }
        if ($c -eq ',') { $expectKey = ($stack.Count -gt 0 -and $null -ne $stack.Peek()); $i++; continue }
        if ($c -eq '"') {
            $j = $i + 1
            while ($j -lt $n -and $json[$j] -ne '"') { if ($json[$j] -eq '\') { $j++ }; $j++ }
            $s = $json.Substring($i + 1, $j - $i - 1)
            if ($expectKey) {
                if ($s -match '[^\x20-\x7e]' -or $s.Contains('\') -or $s.Length -eq 0) { return $false }
                $obj = $stack.Peek()
                if ($obj.ContainsKey($s)) { return $false }
                $obj[$s] = 1; $expectKey = $false
            }
            $i = $j + 1; continue
        }
        $i++
    }
    return $true
}

function Compare-Version([string]$a, [string]$b) {
    $x = $a.Split('.'); $y = $b.Split('.')
    for ($k = 0; $k -lt [Math]::Max($x.Length, $y.Length); $k++) {
        $p = 0; $q = 0
        if ($k -lt $x.Length) { [void][int]::TryParse($x[$k], [ref]$p) }
        if ($k -lt $y.Length) { [void][int]::TryParse($y[$k], [ref]$q) }
        if ($p -ne $q) { return [Math]::Sign($p - $q) }
    }
    return 0
}

function Get-Sha256([byte[]]$data) {
    $h = [Security.Cryptography.SHA256]::Create().ComputeHash($data)
    return (($h | ForEach-Object { $_.ToString("x2") }) -join "")
}

# ---------------------------------------------------------------- install

Write-Host ""
Write-Host "RB Stems Plus for Windows: installing from $DownloadBase" -ForegroundColor White
Write-Host ""

Step "Downloading the release description"
$jsonBytes = Get-Bytes "payload-windows.json" "the release description"
$sigBytes = Get-Bytes "payload-windows.json.sig" "its signature"

Step "Checking the release's signature"
if (-not (Test-Signature $jsonBytes $sigBytes)) { Fail "the download couldn't be verified (its signature doesn't match RB Stems Plus's keys)." }
$jsonText = [Text.Encoding]::UTF8.GetString($jsonBytes)
if (-not (Test-PlainJsonKeys $jsonText)) { Fail "the release description isn't valid." }
$payload = $jsonText | ConvertFrom-Json
if ($PayloadMinimum -and (Compare-Version $payload.payload_version $PayloadMinimum) -lt 0) { Fail "the release is older than this installer." }
$zipName = $payload.app.zip
if ($zipName -notmatch '^[A-Za-z0-9._-]+$') { Fail "the release description isn't valid." }
Done ("signature valid: RB Stems Plus {0}" -f $payload.app.version)

Step ("Downloading RB Stems Plus {0}" -f $payload.app.version)
$zipBytes = Get-Bytes $zipName "RB Stems Plus"

Step "Checking the download"
if ((Get-Sha256 $zipBytes) -ne $payload.app.sha256) { Fail "the app didn't download correctly (its checksum doesn't match)." }
Done "checksum matches the signed release"

Step "Unpacking"
$dest = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "Programs\RB Stems Plus"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("rbstemsplus-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $zipPath = Join-Path $tmp $zipName
    [IO.File]::WriteAllBytes($zipPath, $zipBytes)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Write-Progress -Activity "Unpacking RB Stems Plus" -Status "please wait" -PercentComplete 50
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, (Join-Path $tmp "app"))
    Write-Progress -Activity "Unpacking RB Stems Plus" -Completed
    $newExe = Join-Path $tmp "app\RBStemsPlus.exe"
    if (-not (Test-Path $newExe)) { Fail "the app's zip has no RBStemsPlus.exe." }
    Done "unpacked"
    Step "Installing in $dest"
    Get-Process -Name RBStemsPlus -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dest) -and $_.MainWindowHandle -ne 0 } | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 500
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $exe = Join-Path $dest "RBStemsPlus.exe"
    if (Test-Path $exe) { Move-Item -Force $exe "$exe.old" }
    Copy-Item -Force (Join-Path $tmp "app\*") $dest
    # the Start menu shortcut
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) "RB Stems Plus.lnk"
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk); $s.TargetPath = $exe; $s.WorkingDirectory = $dest; $s.Description = "RB Stems Plus"; $s.Save()
    Done "installed RBStemsPlus.exe"
    Done "added RB Stems Plus to the Start menu"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Step "Opening RB Stems Plus"
Start-Process -FilePath $exe
Write-Host ""
Write-Host "RB Stems Plus is in your Start menu and open. You can close this window." -ForegroundColor Green
