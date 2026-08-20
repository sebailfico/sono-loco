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

.EXAMPLE
    ./tools/test-client-only.ps1 -Client COM8 -Source COM10
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Client,
    [Parameter(Mandatory = $true)][string]$Source,
    [int]$Duration = 60,
    [switch]$Keep
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

# --- 1. Put the client into client-only mode --------------------------------
$cli = Open-Port -Port $Client
Start-Sleep -Milliseconds 800
$null = $cli.ReadExisting()
$cli.Write('?')
$id = Read-For -Sp $cli -Seconds 4 -Pattern 'conly=\d'
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
$id = Read-For -Sp $cli -Seconds 5 -Pattern 'conly=\d'

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
Write-Host "  watching $Client for $Duration s ..."
$null = $cli.ReadExisting()
$out = Read-For -Sp $cli -Seconds $Duration

if ($out -match '=== CLIENT') {
    Write-Host "  PASS entered CLIENT mode from a normal boot" -ForegroundColor Green
} else {
    Write-Host "  FAIL never entered CLIENT mode" -ForegroundColor Red
    $fail = $true
}

# The status line is the ordinary one, not bench telemetry: this node is not in
# bench mode, which is the entire point.
$statuses = [regex]::Matches($out, 'Status: mode=CLIENT heap=(\d+) jitter=(\d+)B rx=(\d+) lost=(\d+) ovf=(\d+) und=(\d+)')
if ($statuses.Count -gt 0) {
    $last = $statuses[$statuses.Count - 1]
    $jit  = [int]$last.Groups[2].Value
    $rx   = [int]$last.Groups[3].Value
    $ovf  = [int]$last.Groups[5].Value
    $und  = [int]$last.Groups[6].Value
    Write-Host ("  last status: rx={0} jitter={1}B ovf={2} und={3} heap={4}" -f `
        $rx, $jit, $ovf, $und, $last.Groups[1].Value)

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

Write-Host ''
if ($fail) { Write-Host 'Overall: FAIL' -ForegroundColor Red; exit 1 }
Write-Host 'Overall: PASS' -ForegroundColor Green
