# Installs a CUDA ONNX Runtime for the bridge's Demucs v4 on an NVIDIA GPU, by hand (it isn't part of the
# RB Stems Plus package). Run from an elevated PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File scripts\windows-install-cuda.ps1 -Source C:\dj\ort-cuda
#   powershell -ExecutionPolicy Bypass -File scripts\windows-install-cuda.ps1 -Remove
#
# -Source: a folder with onnxruntime.dll (ONNX Runtime 1.18.0, the CUDA 12 build), onnxruntime_providers_*.dll,
# and the CUDA 12 / cuDNN 8 DLLs it loads (cublas64_12, cublasLt64_12, cudart64_12, cufft64_11, curand64_10, cudnn*64_8).
# They are copied to %ProgramData%\rbstemsplus\ort-cuda, owned by Administrators, writable only by Administrators
# and SYSTEM: the bridge refuses a library others can change (like ort\onnxruntime.dll).
param([string]$Source, [switch]$Remove)
$ErrorActionPreference = "Stop"
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw "run this from an elevated (Administrator) PowerShell" }
$root = Join-Path $env:ProgramData "rbstemsplus"
$dest = Join-Path $root "ort-cuda"
if ($Remove) { if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }; "removed $dest"; return }
if (-not $Source -or -not (Test-Path (Join-Path $Source "onnxruntime.dll"))) { throw "-Source must be a folder with the CUDA onnxruntime.dll" }
if (-not (Test-Path $root)) { throw "$root doesn't exist: install RB Stems Plus first" }
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory $dest | Out-Null
Copy-Item (Join-Path $Source "*.dll") $dest
icacls $dest /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-545:(OI)(CI)RX" /T /C | Out-Null
icacls $dest /setowner "*S-1-5-32-544" /T /C | Out-Null
"installed $((Get-ChildItem $dest).Count) files in $dest"
