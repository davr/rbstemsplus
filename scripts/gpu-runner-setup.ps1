# Checks a Windows machine for use as the GPU benchmark runner (docs/windows-port/GPU-RUNNER.md).
# Prints the display adapters and whether a hardware DirectX 12 adapter is there; changes nothing.
$ErrorActionPreference = 'Stop'
Write-Host "Display adapters:"
$gpus = Get-CimInstance Win32_VideoController
$gpus | ForEach-Object { "  {0}  driver {1}  {2:N1} GB" -f $_.Name, $_.DriverVersion, ($_.AdapterRAM / 1GB) }
$real = $gpus | Where-Object { $_.Name -notmatch 'Basic Render|Basic Display|Remote Display|Hyper-V|Microsoft' }
if (-not $real) { Write-Host "`nNo hardware GPU: only Microsoft's basic display adapter. Install the GPU driver, then run this again." -ForegroundColor Yellow }
else { Write-Host "`nA hardware adapter is present: $($real[0].Name)" -ForegroundColor Green }
$dx = Join-Path $env:TEMP 'dxdiag.txt'
Start-Process dxdiag -ArgumentList "/t `"$dx`"" -Wait
$feat = Select-String -Path $dx -Pattern 'Feature Levels:' | Select-Object -First 1
if ($feat) { Write-Host $feat.Line.Trim() } else { Write-Host "dxdiag gave no feature levels" -ForegroundColor Yellow }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ("Running as an administrator: {0} (the runner's account must be one)" -f $isAdmin)
foreach ($f in 'onnxruntime.dll','DirectML.dll') {
  $where = Get-ChildItem 'C:\Program Files\rekordbox' -Recurse -Filter $f -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($where) { Write-Host "Found $f at $($where.FullName)" } else { Write-Host "$f not found under C:\Program Files\rekordbox: copy it from a rekordbox install" -ForegroundColor Yellow }
}
Write-Host "`nNext: register the runner (labels windows,gpu) and set the RBSTEMS_PIONEER_DIR variable; see docs/windows-port/GPU-RUNNER.md."
