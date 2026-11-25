# Runs a capture script inside RenderDoc's own Python (qrenderdoc --python), waits for it, prints its report.
#   powershell -File tools\rdc_run.ps1 -Capture build\rdc\mp2_frame123.rdc [-Script tools\rdc_summary.py] [-Out build\rdc\out]
param([Parameter(Mandatory = $true)][string]$Capture, [string]$Script = "$PSScriptRoot\rdc_summary.py",
      [string]$Out = '', [int]$TimeoutSeconds = 300)
$cap = (Resolve-Path $Capture).Path
if (-not $Out) { $Out = Join-Path (Split-Path $cap) ([IO.Path]::GetFileNameWithoutExtension($cap)) }
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RDC_FILE = $cap
$env:RDC_OUT = (Resolve-Path $Out).Path
$p = Start-Process -FilePath 'C:\Program Files\RenderDoc\qrenderdoc.exe' -ArgumentList @('--python', "`"$((Resolve-Path $Script).Path)`"") -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $p.Id -Force; Write-Output "timed out" }
$rep = Join-Path $Out 'summary.txt'
if (Test-Path $rep) { Get-Content $rep } else { Write-Output "no report in $Out" }
