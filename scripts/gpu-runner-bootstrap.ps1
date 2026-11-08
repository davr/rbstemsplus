# Run once, in an elevated PowerShell on the GPU machine (see docs/windows-port/GPU-RUNNER.md):
#   .\gpu-runner-bootstrap.ps1 -Token <registration token from GitHub> [-Repo davr/rbstemsplus]
# Installs the GitHub Actions runner as a Windows service (labels: windows,gpu), running as SYSTEM so it is an
# administrator. Makes no other change to the machine; everything after this is done by workflows.
param(
  [Parameter(Mandatory)][string]$Token,
  [string]$Repo = 'davr/rbstemsplus',
  [string]$Dir = 'C:\actions-runner'
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this in an elevated PowerShell (Run as administrator).' }
Write-Host '1/4 The newest runner release'
$rel = Invoke-RestMethod 'https://api.github.com/repos/actions/runner/releases/latest' -Headers @{ 'User-Agent' = 'rbstems-bootstrap' }
$asset = $rel.assets | Where-Object { $_.name -like 'actions-runner-win-x64-*.zip' -and $_.name -notlike '*noruntime*' } | Select-Object -First 1
if (-not $asset) { throw 'no Windows x64 runner asset found' }
New-Item -ItemType Directory -Force $Dir | Out-Null
$zip = Join-Path $Dir 'runner.zip'
Write-Host "2/4 Downloading $($asset.name)"
Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing
Write-Host '3/4 Unpacking'
Expand-Archive $zip -DestinationPath $Dir -Force
Remove-Item $zip
Write-Host '4/4 Registering it as a service (labels windows,gpu)'
Push-Location $Dir
try {
  & .\config.cmd --unattended --url "https://github.com/$Repo" --token $Token --name "$env:COMPUTERNAME-gpu" --labels 'windows,gpu' `
    --runasservice --windowslogonaccount 'NT AUTHORITY\SYSTEM' --replace
  if ($LASTEXITCODE -ne 0) { throw "config.cmd failed ($LASTEXITCODE): the token expires after an hour; get a new one" }
} finally { Pop-Location }
Get-Service 'actions.runner.*' | Format-Table Name, Status -AutoSize
Write-Host "`nDone. In GitHub: Settings > Actions > Runners should show '$env:COMPUTERNAME-gpu' as Idle."
