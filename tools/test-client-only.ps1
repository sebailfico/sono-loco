<#
.SYNOPSIS
    Prove a BT-capable board works as a mesh client in NORMAL mode.

.DESCRIPTION
    The D3 amendment says a WROOM can be a mesh client as long as Bluetooth never
    starts, and bench mode demonstrated the mechanism -- but bench mode is a test
    mode. This checks the shipping path: client-only set in NVS, node booted
    normally, Bluetooth never started, and a real ESP-NOW stream played.

    bench-mesh.ps1 cannot cover this, because it puts every node into bench mode
    by design. Here only the *source* runs in bench mode; the node under test
    runs exactly as it would in a room.

    What it does:
      1. puts the client into client-only mode ('c') and waits for the reboot
      2. confirms from its banner that BT is off and ESP-NOW is up
      3. starts the source streaming in bench mode
      4. watches the client's ordinary status output for CLIENT mode and a
         healthy buffer
      5. puts the client back to normal ('c' again) unless -Keep

.PARAMETER Client
    Port of the board under test -- the one that should become a client.

.PARAMETER Source
    Port of the board that generates the stream.

.PARAMETER Duration
    Seconds to watch the client once the stream is running. Default 60. Long
    enough to see the buffer settle; drift needs bench-mesh.ps1 and 600 s.

.PARAMETER Keep
    Leave the client in client-only mode at the end.

.PARAMETER Flash
    Upload the application firmware to both boards first. Worth doing after
    `pio test`, which leaves the *unit-test* binary on the board -- that firmware
    runs its tests once at boot and then sits in an empty loop(), so the node goes
    silent and every check here fails for a reason that has nothing to do with
    the feature under test.

.PARAMETER ClientEnv
    PlatformIO environment for the client board. Required with -Flash.

.PARAMETER SourceEnv
    PlatformIO environment for the source board. Required with -Flash.

.EXAMPLE
    ./tools/test-client-only.ps1 -Client COM8 -Source COM10
    ./tools/test-client-only.ps1 -Client COM8 -Source COM10 -Flash -ClientEnv esp32dev -SourceEnv esp32c3
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Client,
    [Parameter(Mandatory = $true)][string]$Source,
    [int]$Duration = 60,
    [switch]$Keep,
    [switch]$Flash,
    [string]$ClientEnv = '',
    [string]$SourceEnv = ''
)

$ErrorActionPreference = 'Stop'

function Open-Port {
    param([string]$Port, [int]$RetrySeconds = 25)
    $deadline = (Get-Date).AddSeconds($RetrySeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
            $sp.ReadTimeout = 200
            # Same reason as bench-mesh.ps1: on the classic auto-reset circuit
            # DTR drives GPIO0 and RTS drives EN, so asserting either can drop the
            # board into the ROM bootloader instead of running the firmware.
            $sp.DtrEnable = $false
            $sp.RtsEnable = $false
            $sp.Open()
            return $sp
        } catch { Start-Sleep -Milliseconds 500 }
    }
    throw "Could not open $Port within $RetrySeconds s"
}

function Read-For {
    param($Sp, [int]$Seconds, [string]$Pattern = $null)
    $buf = ''
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        try { $buf += $Sp.ReadExisting() } catch {}
        if ($Pattern -and $buf -match $Pattern) { break }
        Start-Sleep -Milliseconds 50
    }
    return $buf
}

Write-Host ''
Write-Host '=== client-only mode check ===' -ForegroundColor Cyan
Write-Host "client under test : $Client"
Write-Host "stream source     : $Source"

$repoRoot = Split-Path -Parent $PSScriptRoot
$version  = (& git -C $repoRoot describe --tags --always --dirty=* 2>$null)
Write-Host "repo              : $version"

$fail = $false

# --- 0. Flash, if asked -----------------------------------------------------
if ($Flash) {
    if (-not $ClientEnv -or -not $SourceEnv) {
        throw '-Flash needs -ClientEnv and -SourceEnv (e.g. -ClientEnv esp32dev -SourceEnv esp32c3)'
    }
    $pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (-not (Test-Path $pio)) {
        $c = Get-Command pio -ErrorAction SilentlyContinue
        if (-not $c) { throw 'PlatformIO not found' }
        $pio = $c.Source
    }
    $projectDir = Join-Path $repoRoot 'esp32-code'
    foreach ($pair in @(@($Client, $ClientEnv), @($Source, $SourceEnv))) {
        Write-Host ("  flashing {0} with {1} ..." -f $pair[0], $pair[1]) -NoNewline
        # The toolchain writes warnings to stderr, which $ErrorActionPreference =
        # 'Stop' would turn into a terminating error despite a zero exit code.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $pio run -d $projectDir -e $pair[1] --target upload --upload-port $pair[0] 2>&1 | Out-Null
        } finally { $ErrorActionPreference = $prevEap }
        if ($LASTEXITCODE -ne 0) { throw "Upload to $($pair[0]) failed (exit $LASTEXITCODE)" }
        Write-Host ' done'
    }
    Start-Sleep -Seconds 2
}

# --- 1. Put the client into client-only mode --------------------------------
$cli = Open-Port -Port $Client
Start-Sleep -Milliseconds 800
$null = $cli.ReadExisting()
$cli.Write('?')
$id = Read-For -Sp $cli -Seconds 6 -Pattern 'conly=\d'

# Fail here rather than 600 s later. A board that says nothing at all is almost
# always running the unit-test firmware: `pio test` uploads it, it prints its
# results once at boot and then sits in an empty loop(). Every check below would
# fail, none of them for a reason to do with client-only mode.
if (-not $id) {
    try { $cli.Close() } catch {}
    try { $cli.Dispose() } catch {}
    throw ("$Client said nothing in 6 s. If `pio test` has run since the last " +
           "upload, the board is running the test firmware -- reflash the app " +
           "(pio run -e <env> --target upload --upload-port $Client) or pass -Flash.")
}

if ($id -match 'conly=1') {
    Write-Host "  already in client-only mode"
} else {
    Write-Host "  switching to client-only ..." -NoNewline
    $cli.Write('c')
    Start-Sleep -Milliseconds 500
    try { $cli.Close() } catch {}
    try { $cli.Dispose() } catch {}
    Start-Sleep -Seconds 4          # the port can disappear across the restart
    $cli = Open-Port -Port $Client
    Write-Host " done"
}

# --- 2. Confirm from the banner, not from hope ------------------------------
Start-Sleep -Milliseconds 500
$null = $cli.ReadExisting()
$cli.Write('?')
$id = Read-For -Sp $cli -Seconds 8 -Pattern 'conly=\d'

if ($id -match 'conly=1')  { Write-Host "  PASS client-only is set" -ForegroundColor Green }
else { Write-Host "  FAIL client-only did not stick: $id" -ForegroundColor Red; $fail = $true }

if ($id -match 'espnow=1') { Write-Host "  PASS ESP-NOW is up on a board with no PSRAM" -ForegroundColor Green }
else { Write-Host "  FAIL ESP-NOW is not active -- the PSRAM guard still refused it" -ForegroundColor Red; $fail = $true }

if ($id -match 'bench=1') {
    Write-Host "  FAIL node is in bench mode -- that is not what this test proves" -ForegroundColor Red
    $fail = $true
} else {
    Write-Host "  PASS node is in normal mode, not bench mode" -ForegroundColor Green
}

# --- 3. Start the stream ----------------------------------------------------
Write-Host "  starting the source ..." -NoNewline
$src = Open-Port -Port $Source
Start-Sleep -Milliseconds 800
$null = $src.ReadExisting()
$src.Write('?')
$srcId = Read-For -Sp $src -Seconds 4 -Pattern 'bench=\d'
if ($srcId -notmatch 'bench=1') {
    $src.Write('b')
    Start-Sleep -Milliseconds 400
    try { $src.Close() } catch {}
    try { $src.Dispose() } catch {}
    Start-Sleep -Seconds 5
    $src = Open-Port -Port $Source
    Start-Sleep -Milliseconds 800
    $null = $src.ReadExisting()
}
$src.Write('s')
Write-Host " done"

# --- 4. Watch the client behave like a client -------------------------------
# Keep the capture. This is the only path that exercises the ordinary String-built
# status line -- bench telemetry replaces it with printf -- so it is the only
# place heap behaviour under normal logging can be observed.
$logDir = Join-Path $repoRoot 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$logPath = Join-Path $logDir ("clientonly-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

Write-Host "  watching $Client for $Duration s (log: $logPath) ..."
$null = $cli.ReadExisting()
$out = Read-For -Sp $cli -Seconds $Duration

@(
    "# SonoLoco client-only check"
    "# started : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "# version : $version"
    "# client  : $Client"
    "# source  : $Source"
    "#"
    $out
) | Out-File -FilePath $logPath -Encoding utf8

if ($out -match '=== CLIENT') {
    Write-Host "  PASS entered CLIENT mode from a normal boot" -ForegroundColor Green
} else {
    Write-Host "  FAIL never entered CLIENT mode" -ForegroundColor Red
    $fail = $true
}

# The status line is the ordinary one, not bench telemetry: this node is not in
# bench mode, which is the entire point.
$statuses = [regex]::Matches($out, 'Status: mode=CLIENT [^
]*')
if ($statuses.Count -gt 0) {
    $lastLine = $statuses[$statuses.Count - 1].Value
    function Field { param([string]$Line, [string]$Name)
        if ($Line -match ($Name + '=(\d+)')) { return [int]$matches[1] }
        return $null
    }
    $jit = Field $lastLine 'jitter'
    $rx  = Field $lastLine 'rx'
    $ovf = Field $lastLine 'ovf'
    $und = Field $lastLine 'und'
    Write-Host ("  last status: $lastLine")

    if ($rx -gt 0)   { Write-Host "  PASS packets are arriving" -ForegroundColor Green }
    else { Write-Host "  FAIL no packets received" -ForegroundColor Red; $fail = $true }

    if ($jit -gt 0)  { Write-Host "  PASS jitter buffer is holding audio" -ForegroundColor Green }
    else { Write-Host "  FAIL jitter buffer is empty" -ForegroundColor Red; $fail = $true }

    if ($ovf -eq 0 -and $und -eq 0) {
        Write-Host "  PASS no overflow, no underrun" -ForegroundColor Green
    } else {
        Write-Host "  WARN ovf=$ovf und=$und" -ForegroundColor Yellow
    }
} else {
    Write-Host "  FAIL no CLIENT status lines -- node never played anything" -ForegroundColor Red
    $fail = $true
}

# A node that started Bluetooth would say so, and on a WROOM would likely have
# crashed by now. Checking is cheap and the failure would be confusing.
if ($out -match 'BT discoverable|Bluetooth started') {
    Write-Host "  FAIL Bluetooth started on a client-only node" -ForegroundColor Red
    $fail = $true
} else {
    Write-Host "  PASS Bluetooth never started" -ForegroundColor Green
}

# --- 5. Tidy up -------------------------------------------------------------
$src.Write('x')
Start-Sleep -Milliseconds 300
if (-not $Keep) {
    $cli.Write('c')          # back to a normal speaker
    $src.Write('n')          # back out of bench mode
    Start-Sleep -Milliseconds 400
    Write-Host "  restored both nodes (use -Keep to leave the client configured)"
}

foreach ($p in @($cli, $src)) {
    try { if ($p.IsOpen) { $p.Close() } } catch {}
    try { $p.Dispose() } catch {}
}

# Heap trend over the run. The status line is built with String concatenation,
# which fragments the heap in principle; whether it does in practice is a
# question for a long run, not an argument (TODO.md).
# Heap trend. Free heap alone cannot answer the String-fragmentation question --
# fragmentation shows as the largest allocatable block falling while free heap
# stays flat -- so both are reported. TODO.md asks for the measurement, not an
# argument.
$heaps = [regex]::Matches($out, 'Status: mode=CLIENT heap=(\d+) maxalloc=(\d+)')
if ($heaps.Count -ge 3) {
    $h0 = [int]$heaps[0].Groups[1].Value
    $h1 = [int]$heaps[$heaps.Count - 1].Groups[1].Value
    $m0 = [int]$heaps[0].Groups[2].Value
    $m1 = [int]$heaps[$heaps.Count - 1].Groups[2].Value
    Write-Host ("  heap     : {0} -> {1} ({2:+#;-#;0} bytes) over {3} status lines" -f `
        $h0, $h1, ($h1 - $h0), $heaps.Count)
    Write-Host ("  maxalloc : {0} -> {1} ({2:+#;-#;0} bytes)  <- fragmentation shows here" -f `
        $m0, $m1, ($m1 - $m0))
}

Write-Host ''
Write-Host "Raw capture: $logPath"
if ($fail) { Write-Host 'Overall: FAIL' -ForegroundColor Red; exit 1 }
Write-Host 'Overall: PASS' -ForegroundColor Green
