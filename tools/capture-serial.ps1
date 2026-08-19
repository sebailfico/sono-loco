<#
.SYNOPSIS
    Capture a serial session from a SonoLoco node to a timestamped log file.

.DESCRIPTION
    Wraps `pio device monitor` so bench runs can be compared between firmware
    changes — the counters in the status lines only mean something relative to a
    previous run. See docs/bench-test.md.

    Output goes to logs/<environment>-<timestamp>.log and to the console at the
    same time. Press Ctrl+C to stop the capture.

.PARAMETER Environment
    PlatformIO environment to monitor: esp32dev, esp32wrover or esp32s3.

.PARAMETER Note
    Optional label folded into the filename, e.g. -Note "adpcm-trial".

.EXAMPLE
    ./tools/capture-serial.ps1 -Environment esp32wrover
    ./tools/capture-serial.ps1 -Environment esp32s3 -Note "5min-stream"
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('esp32dev', 'esp32wrover', 'esp32s3')]
    [string]$Environment,

    [string]$Note = ''
)

$ErrorActionPreference = 'Stop'

# PlatformIO is not on PATH on the dev machine.
$pio = Get-Command pio -ErrorAction SilentlyContinue
if ($pio) {
    $pioExe = $pio.Source
} else {
    $pioExe = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
}
if (-not (Test-Path $pioExe)) {
    throw "PlatformIO not found. Looked on PATH and at $pioExe"
}

$repoRoot   = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot 'esp32-code'
$logDir     = Join-Path $repoRoot 'logs'

if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$suffix  = if ($Note) { "-$($Note -replace '[^\w\-]', '_')" } else { '' }
$logPath = Join-Path $logDir "$Environment-$stamp$suffix.log"

# Header, so a log is self-describing months later.
$commit = & git -C $repoRoot rev-parse --short HEAD 2>$null
$dirty  = & git -C $repoRoot status --porcelain 2>$null
$header = @(
    "# SonoLoco serial capture"
    "# environment : $Environment"
    "# started     : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "# commit      : $commit$(if ($dirty) { ' (working tree dirty)' })"
    "# note        : $(if ($Note) { $Note } else { '-' })"
    "#"
)
$header | Out-File -FilePath $logPath -Encoding utf8

Write-Host "Capturing $Environment -> $logPath" -ForegroundColor Cyan
Write-Host "Ctrl+C to stop." -ForegroundColor DarkGray

# monitor_filters in platformio.ini already add timestamps and exception
# decoding, so the log is directly comparable between runs.
& $pioExe device monitor -d $projectDir -e $Environment |
    Tee-Object -FilePath $logPath -Append

Write-Host "Saved $logPath" -ForegroundColor Green
