# Builds a self-contained healer report (one HTML file) from a SimulationCraft JSON report.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_healer_report.ps1 -Json <report.json> [-Out <report.html>] [-Open]

param(
  [Parameter(Mandatory = $true)][string]$Json,
  [string]$Out,
  [switch]$Open
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$templatePath = Join-Path $here 'healer_report_template.html'

if (-not (Test-Path -LiteralPath $Json)) {
  Write-Error "SimC JSON report not found: $Json. Run the simulation with json=<file> first."
}
$jsonPath = (Resolve-Path -LiteralPath $Json).Path
if (-not $Out) { $Out = [IO.Path]::ChangeExtension($jsonPath, '.healer.html') }
$outPath = [IO.Path]::GetFullPath($Out)

$template = [IO.File]::ReadAllText($templatePath)
# Escape "</" so the JSON can never close the <script> tag it is embedded in.
$data = [IO.File]::ReadAllText($jsonPath).Replace('</', '<\/')
$html = $template.Replace('__SIMC_DATA__', $data)

[IO.File]::WriteAllText($outPath, $html, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Healer report written to $outPath"

if ($Open) { Start-Process $outPath }
