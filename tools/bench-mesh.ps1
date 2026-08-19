<#
.SYNOPSIS
    Automated multi-board mesh test: flash, stream, and measure clock drift.

.DESCRIPTION
    Drives every SonoLoco node attached to this PC. There is no fixed board
    limit -- ports are discovered at run time and each board is identified by its
    chip, so adding a third or fourth node needs no change here.

    What it does:
      1. discovers ESP32 serial ports and identifies the chip on each
      2. optionally flashes the right firmware for that chip (-Flash)
      3. reboots every node into bench mode, where Bluetooth is never started
      4. elects one node to generate a synthetic 22.05 kHz test stream over
         ESP-NOW; the rest fall into CLIENT and play it
      5. collects [BENCH] telemetry for -Duration seconds
      6. reports stream health and per-board clock drift

    Why a synthetic source: the normal SERVER role requires a phone to connect
    over A2DP, which cannot be automated. Bench mode also skips Bluetooth
    entirely, which is what allows a WROOM to take part at all -- the BT/WiFi
    coexistence problem that keeps it off the mesh only exists while the BT
    stack is running.

    Drift measurement: every node reports its own millis() alongside its
    counters. Regressing that against PC time gives each board's clock error in
    ppm; the difference between two boards is their relative drift, which is
    what eventually empties or overflows a client's jitter buffer. The client's
    jitter fill slope is reported as an independent cross-check, measured in the
    audio domain rather than the logging domain.

.PARAMETER Duration
    Seconds to stream and measure. Drift precision improves roughly linearly
    with this, so 60 s tells you whether audio flows and 600 s tells you whether
    the clocks agree. Default 180.

.PARAMETER Flash
    Build and upload firmware to every discovered board first.

.PARAMETER Source
    COM port of the node that should generate the stream. Default: the first
    discovered node that reports ESP-NOW active.

.PARAMETER Ports
    Explicit port list, bypassing discovery. e.g. -Ports COM8,COM9

.PARAMETER KeepBenchMode
    Leave the nodes in bench mode at the end instead of rebooting them back to
    normal speaker behaviour.

.EXAMPLE
    ./tools/bench-mesh.ps1 -Flash -Duration 120
    ./tools/bench-mesh.ps1 -Duration 600 -Source COM9
#>

[CmdletBinding()]
param(
    [int]$Duration = 180,
    [switch]$Flash,
    [string]$Source = '',
    [string[]]$Ports = @(),
    [int]$PollMs = 10,
    [switch]$KeepBenchMode
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot 'esp32-code'
$logDir     = Join-Path $repoRoot 'logs'

# Mesh audio format, needed to turn byte counts into rates. Keep in step with
# config.h -- CLIENT_SAMPLE_RATE and ESPNOW_PAYLOAD_SIZE.
$SampleRate  = 22050
$BytesPerSec = $SampleRate * 2          # 16-bit mono
$PayloadSize = 200
$PktPerSec   = $BytesPerSec / $PayloadSize
$JitterBufSize = 8192

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Get-PioExe {
    $c = Get-Command pio -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (Test-Path $p) { return $p }
    throw "PlatformIO not found on PATH or at $p"
}

function Get-PythonExe {
    $p = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
    if (Test-Path $p) { return $p }
    $c = Get-Command python -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    throw "No Python found for esptool"
}

function Get-EsptoolPath {
    $p = Join-Path $env:USERPROFILE '.platformio\packages\tool-esptoolpy\esptool.py'
    if (Test-Path $p) { return $p }
    throw "esptool.py not found at $p -- run a PlatformIO build once first"
}

function Get-CandidatePorts {
    $pio = Get-PioExe
    $json = & $pio device list --json-output 2>$null
    $devices = $json | ConvertFrom-Json
    $result = @()
    foreach ($d in $devices) {
        # USB-serial bridges and native USB seen on ESP32 boards:
        #   10C4:EA60 CP210x, 1A86:* CH34x, 0403:* FTDI, 303A:* Espressif native
        if ($d.hwid -match 'VID:PID=(10C4|1A86|0403|303A)') {
            $result += $d.port
        }
    }
    return ($result | Sort-Object -Unique)
}

function Get-ChipOnPort {
    param([string]$Port)
    $py = Get-PythonExe
    $et = Get-EsptoolPath
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $py $et --port $Port --no-stub chip_id 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $prevEap
    }
    if ($out -match 'Chip is ([^\r\n(]+)') { return $matches[1].Trim() }
    return $null
}

function Get-EnvForChip {
    param([string]$Chip)
    if (-not $Chip) { return $null }
    if ($Chip -match 'ESP32-S3') { return 'esp32s3' }
    if ($Chip -match 'ESP32-C')  { return $null }     # no build for C-series yet
    if ($Chip -match 'ESP32')    { return 'esp32dev' } # classic: WROOM or WROVER
    return $null
}

function Open-Port {
    param([string]$Port, [int]$RetrySeconds = 25)
    $deadline = (Get-Date).AddSeconds($RetrySeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
            $sp.ReadTimeout  = 200
            $sp.WriteTimeout = 500
            # Both lines deasserted. On the classic auto-reset circuit DTR drives
            # GPIO0 and RTS drives EN, so asserting either can drop the board into
            # the ROM bootloader instead of running the firmware.
            $sp.DtrEnable = $false
            $sp.RtsEnable = $false
            $sp.Open()
            return $sp
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    throw "Could not open $Port within $RetrySeconds s"
}

function Close-Port {
    param($Sp)
    if ($null -eq $Sp) { return }
    try { if ($Sp.IsOpen) { $Sp.Close() } } catch {}
    try { $Sp.Dispose() } catch {}
}

function Send-Cmd {
    param($Sp, [string]$Cmd)
    try { $Sp.Write($Cmd) } catch { Write-Warning "write to $($Sp.PortName) failed: $_" }
}

<#
Ordinary least squares plus the standard error of the slope. The SE matters:
without it a drift figure is unfalsifiable, and at short durations the
measurement noise is larger than the crystal error being measured.
#>
function Get-Slope {
    param([double[]]$X, [double[]]$Y)
    $n = $X.Count
    if ($n -lt 3) { return $null }
    $mx = ($X | Measure-Object -Average).Average
    $my = ($Y | Measure-Object -Average).Average
    $sxx = 0.0; $sxy = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $dx = $X[$i] - $mx
        $sxx += $dx * $dx
        $sxy += $dx * ($Y[$i] - $my)
    }
    if ($sxx -le 0) { return $null }
    $slope = $sxy / $sxx
    $intercept = $my - $slope * $mx
    $ss = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $r = $Y[$i] - ($slope * $X[$i] + $intercept)
        $ss += $r * $r
    }
    $se = 0.0
    if ($n -gt 2) { $se = [math]::Sqrt(($ss / ($n - 2)) / $sxx) }
    return [pscustomobject]@{ Slope = $slope; SE = $se; N = $n }
}

function Wait-ForLine {
    param($Sp, [string]$Pattern, [int]$TimeoutSec = 10)
    $buf = ''
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try { $buf += $Sp.ReadExisting() } catch {}
        foreach ($l in ($buf -split "`n")) {
            if ($l -match $Pattern) { return $l.Trim() }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

function Parse-BenchLine {
    param([string]$Line)
    $kv = @{}
    foreach ($m in [regex]::Matches($Line, '([A-Za-z_]+)=([^\s]+)')) {
        $kv[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $kv
}

function Fmt {
    param($Value, [int]$Decimals = 1)
    if ($null -eq $Value) { return 'n/a' }
    return [string]::Format("{0:N$Decimals}", $Value)
}

# ---------------------------------------------------------------------------
# 1. Discover and identify
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '=== SonoLoco mesh bench ===' -ForegroundColor Cyan

if ($Ports.Count -eq 0) { $Ports = Get-CandidatePorts }
if ($Ports.Count -eq 0) { throw 'No ESP32-looking serial ports found.' }

Write-Host "Discovered ports: $($Ports -join ', ')"

$nodes = @()
foreach ($p in $Ports) {
    Write-Host "  identifying $p ..." -NoNewline
    $chip = Get-ChipOnPort -Port $p
    $envName = Get-EnvForChip -Chip $chip
    if (-not $envName) {
        Write-Host " $chip -- no firmware build for this chip, skipping" -ForegroundColor Yellow
        continue
    }
    Write-Host " $chip -> $envName"
    $nodes += [pscustomobject]@{
        Port = $p; Chip = $chip; Env = $envName
        Sp = $null; Buffer = ''; Samples = @(); Ident = $null; IsSource = $false
    }
}

if ($nodes.Count -lt 2) {
    Write-Warning "Only $($nodes.Count) usable node(s). A mesh test needs at least two: one source, one client."
    if ($nodes.Count -eq 0) { throw 'Nothing to test.' }
}

# ---------------------------------------------------------------------------
# 2. Flash
# ---------------------------------------------------------------------------

if ($Flash) {
    $pio = Get-PioExe
    foreach ($n in $nodes) {
        Write-Host "Flashing $($n.Port) with $($n.Env) ..." -ForegroundColor Cyan
        # The toolchain writes compiler warnings to stderr, and with
        # $ErrorActionPreference = 'Stop' PowerShell turns any stderr from a
        # native command into a terminating error even when it exits 0. Drop to
        # Continue for the call and judge it by the exit code instead.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $pio run -d $projectDir -e $n.Env --target upload --upload-port $n.Port 2>&1 | Out-Null
        } finally {
            $ErrorActionPreference = $prevEap
        }
        if ($LASTEXITCODE -ne 0) { throw "Upload to $($n.Port) failed (exit $LASTEXITCODE)" }
    }
    Start-Sleep -Seconds 2
}

# ---------------------------------------------------------------------------
# 3. Into bench mode
# ---------------------------------------------------------------------------

Write-Host 'Rebooting nodes into bench mode ...' -ForegroundColor Cyan

# Ask, verify, retry. Fixed sleeps are not enough here: a classic board spends a
# couple of seconds in BT init before it reads the command, and an S3 drops its
# USB port entirely across the restart.
foreach ($n in $nodes) {
    $ok = $false
    for ($attempt = 1; $attempt -le 3 -and -not $ok; $attempt++) {
        $sp = Open-Port -Port $n.Port
        Start-Sleep -Milliseconds 900
        $null = $sp.ReadExisting()
        Send-Cmd -Sp $sp -Cmd '?'
        $idLine = Wait-ForLine -Sp $sp -Pattern '^\[BENCH\] id ' -TimeoutSec 6
        if ($idLine) {
            $kv = Parse-BenchLine -Line $idLine
            if ($kv['bench'] -eq '1') {
                $n.Sp = $sp
                $n.Ident = $kv
                $ok = $true
                break
            }
        }
        Send-Cmd -Sp $sp -Cmd 'b'
        Start-Sleep -Milliseconds 400
        Close-Port -Sp $sp        # the port may disappear across the restart
        Start-Sleep -Seconds 5
    }
    if (-not $ok) {
        Write-Warning "$($n.Port) never reported bench=1 after 3 attempts"
        if ($null -eq $n.Sp) { $n.Sp = Open-Port -Port $n.Port }
    }
}

foreach ($n in $nodes) {
    if ($n.Ident) {
        Write-Host ("  {0}  {1,-14} psram={2,-8} espnow={3} bench={4}  {5}" -f `
            $n.Port, $n.Ident['chip'], $n.Ident['psram'], $n.Ident['espnow'],
            $n.Ident['bench'], $n.Ident['mac'])
        if ($n.Ident['espnow'] -ne '1') {
            Write-Warning "$($n.Port) has ESP-NOW inactive -- it cannot take part"
        }
    } else {
        Write-Warning "$($n.Port) never identified itself"
    }
}

# ---------------------------------------------------------------------------
# 4. Elect a source
# ---------------------------------------------------------------------------

$sourceNode = $null
if ($Source) {
    $sourceNode = $nodes | Where-Object { $_.Port -eq $Source } | Select-Object -First 1
    if (-not $sourceNode) { throw "-Source $Source is not among the discovered nodes" }
} else {
    $sourceNode = $nodes | Where-Object { $_.Ident -and $_.Ident['espnow'] -eq '1' } | Select-Object -First 1
}
if (-not $sourceNode) { throw 'No node with ESP-NOW active could be the source.' }
$sourceNode.IsSource = $true

Write-Host "Source: $($sourceNode.Port) ($($sourceNode.Chip))" -ForegroundColor Green
foreach ($n in $nodes) { $null = $n.Sp.ReadExisting() }
Send-Cmd -Sp $sourceNode.Sp -Cmd 's'

# ---------------------------------------------------------------------------
# 5. Collect
# ---------------------------------------------------------------------------

if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $logDir "bench-$stamp.log"
$commit  = & git -C $repoRoot rev-parse --short HEAD 2>$null
@(
    "# SonoLoco mesh bench"
    "# started  : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "# commit   : $commit"
    "# duration : $Duration s"
    "# source   : $($sourceNode.Port)"
    "# nodes    : $(($nodes | ForEach-Object { "$($_.Port)=$($_.Chip)" }) -join ' ')"
    "#"
) | Out-File -FilePath $logPath -Encoding utf8

Write-Host "Streaming for $Duration s (log: $logPath) ..." -ForegroundColor Cyan

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$writer = [System.IO.StreamWriter]::new($logPath, $true)
$nextTick = 10
try {
    while ($sw.Elapsed.TotalSeconds -lt $Duration) {
        foreach ($n in $nodes) {
            $chunk = ''
            try { $chunk = $n.Sp.ReadExisting() } catch {}
            if ($chunk.Length -eq 0) { continue }

            # Timestamp when the chunk arrived, then split. Serial latency adds
            # noise to this, not bias, so the regression averages it out.
            $t = $sw.Elapsed.TotalSeconds
            $n.Buffer += $chunk
            while ($true) {
                $idx = $n.Buffer.IndexOf("`n")
                if ($idx -lt 0) { break }
                $line = $n.Buffer.Substring(0, $idx).Trim()
                $n.Buffer = $n.Buffer.Substring($idx + 1)
                if ($line.Length -eq 0) { continue }
                # Extra parentheses are load-bearing: without them PowerShell
                # splits on the commas as method arguments and -f sees one arg.
                $writer.WriteLine(("{0,9:F3} {1} {2}" -f $t, $n.Port, $line))
                if ($line -match '^\[BENCH\] ms=') {
                    $kv = Parse-BenchLine -Line $line
                    $kv['_t'] = $t
                    $n.Samples += ,$kv
                }
            }
        }
        if ($sw.Elapsed.TotalSeconds -ge $nextTick) {
            $counts = ($nodes | ForEach-Object { "$($_.Port):$($_.Samples.Count)" }) -join ' '
            Write-Host ("  {0,5:N0}s  samples {1}" -f $sw.Elapsed.TotalSeconds, $counts)
            $nextTick += 30
        }
        Start-Sleep -Milliseconds $PollMs
    }
} finally {
    $writer.Flush(); $writer.Close()
}

Send-Cmd -Sp $sourceNode.Sp -Cmd 'x'
Start-Sleep -Milliseconds 300

if (-not $KeepBenchMode) {
    foreach ($n in $nodes) { Send-Cmd -Sp $n.Sp -Cmd 'n' }
    Start-Sleep -Milliseconds 300
}
foreach ($n in $nodes) { Close-Port -Sp $n.Sp }

# ---------------------------------------------------------------------------
# 6. Analyse
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '=== Results ===' -ForegroundColor Cyan

$results = @()
foreach ($n in $nodes) {
    $s = $n.Samples
    if ($s.Count -lt 3) {
        Write-Warning "$($n.Port): only $($s.Count) telemetry samples -- nothing to analyse"
        continue
    }

    $tArr  = [double[]]($s | ForEach-Object { [double]$_['_t'] })
    $msArr = [double[]]($s | ForEach-Object { [double]$_['ms'] / 1000.0 })
    $clock = Get-Slope -X $tArr -Y $msArr

    $ppm = $null; $ppmSe = $null
    if ($clock) {
        $ppm   = ($clock.Slope - 1.0) * 1e6
        $ppmSe = $clock.SE * 1e6
    }

    $first = $s[0]; $last = $s[-1]
    $span  = [double]$last['_t'] - [double]$first['_t']

    $dRx   = [double]$last['rx']   - [double]$first['rx']
    $dLost = [double]$last['lost'] - [double]$first['lost']
    $dOvf  = [double]$last['ovf']  - [double]$first['ovf']
    $dUnd  = [double]$last['und']  - [double]$first['und']
    $dDup  = [double]$last['dup']  - [double]$first['dup']
    $dRsy  = [double]$last['rsy']  - [double]$first['rsy']
    $dTx   = [double]$last['tx']   - [double]$first['tx']

    $jitArr   = [double[]]($s | ForEach-Object { [double]$_['jit'] })
    $jitSlope = Get-Slope -X $tArr -Y $jitArr
    $jitBps   = $null
    if ($jitSlope) { $jitBps = $jitSlope.Slope }

    $results += [pscustomobject]@{
        Port = $n.Port; Chip = $n.Chip; Role = $last['role']; Mode = $last['mode']
        Span = $span; Ppm = $ppm; PpmSe = $ppmSe
        Rx = $dRx; Lost = $dLost; Ovf = $dOvf; Und = $dUnd; Dup = $dDup; Rsy = $dRsy; Tx = $dTx
        JitBps = $jitBps; JitLast = [double]$last['jit']
        QFull = [double]$last['qfull']; SendErr = [double]$last['senderr']; RadioFail = [double]$last['radiofail']
        Heap = [double]$last['heap']
        IsSource = $n.IsSource
    }
}

Write-Host ''
Write-Host 'Stream'
Write-Host '------'
Write-Host ("{0,-6} {1,-14} {2,-7} {3,-10} {4,8} {5,7} {6,5} {7,5} {8,5} {9,5}" -f `
    'Port', 'Chip', 'Role', 'Mode', 'rx', 'lost', 'ovf', 'und', 'dup', 'rsy')
foreach ($r in $results) {
    Write-Host ("{0,-6} {1,-14} {2,-7} {3,-10} {4,8:N0} {5,7:N0} {6,5:N0} {7,5:N0} {8,5:N0} {9,5:N0}" -f `
        $r.Port, $r.Chip, $r.Role, $r.Mode, $r.Rx, $r.Lost, $r.Ovf, $r.Und, $r.Dup, $r.Rsy)
}

$src = $results | Where-Object { $_.IsSource } | Select-Object -First 1
if ($src) {
    $expected = $PktPerSec * $src.Span
    Write-Host ''
    Write-Host ("Source {0}: queued {1:N0} packets in {2:N1}s ({3:N1}/s, expected {4:N1}/s)" -f `
        $src.Port, $src.Tx, $src.Span, ($src.Tx / $src.Span), $PktPerSec)
    Write-Host ("  qfull={0:N0} senderr={1:N0} radiofail={2:N0}" -f $src.QFull, $src.SendErr, $src.RadioFail)
}

Write-Host ''
Write-Host 'Clock drift'
Write-Host '-----------'
Write-Host 'Two independent measures. "log" regresses each node''s millis() against'
Write-Host 'PC time; "audio" derives drift from how fast the jitter buffer fills or'
Write-Host 'empties, which is the one that actually causes dropouts.'
Write-Host ''
Write-Host ("{0,-6} {1,12} {2,10} {3,14} {4,12} {5,12}" -f `
    'Port', 'log ppm', '+/-', 'vs source', 'jitter B/s', 'audio ppm')
foreach ($r in $results) {
    if ($src -and $null -ne $r.Ppm -and $null -ne $src.Ppm) {
        $rel = '{0,14:N1}' -f ($r.Ppm - $src.Ppm)
    } else {
        $rel = '{0,14}' -f 'n/a'
    }
    if ($null -ne $r.JitBps) {
        $jb     = '{0,12:N2}' -f $r.JitBps
        $jitPpm = '{0,12:N1}' -f ($r.JitBps / $BytesPerSec * 1e6)
    } else {
        $jb     = '{0,12}' -f 'n/a'
        $jitPpm = '{0,12}' -f 'n/a'
    }
    Write-Host ("{0,-6} {1,12:N1} {2,10:N1} {3} {4} {5}" -f $r.Port, $r.Ppm, $r.PpmSe, $rel, $jb, $jitPpm)

    # An uncertainty larger than the estimate means the log-derived figure says
    # nothing. It happens on native-USB boards, where CDC latency jitter swamps
    # the crystal error being measured. Longer runs shrink it; the audio-domain
    # column does not have this problem.
    if ($null -ne $r.PpmSe -and $null -ne $r.Ppm -and $r.PpmSe -gt [math]::Abs($r.Ppm)) {
        Write-Host ("       log-derived drift for {0} is below its own noise floor -- use the audio column" -f $r.Port) -ForegroundColor DarkGray
    }
}

Write-Host ''
Write-Host 'Verdict'
Write-Host '-------'
$fail = $false
foreach ($r in $results) {
    if ($r.IsSource) {
        if ($r.Tx -le 0) { Write-Host "FAIL $($r.Port): source queued no packets" -ForegroundColor Red; $fail = $true }
        elseif ($r.QFull -gt 0 -or $r.SendErr -gt 0) {
            Write-Host "WARN $($r.Port): source hit qfull=$($r.QFull) senderr=$($r.SendErr) -- radio not keeping up" -ForegroundColor Yellow
        } else {
            Write-Host "PASS $($r.Port): source streamed cleanly" -ForegroundColor Green
        }
        continue
    }

    if ($r.Rx -le 0) {
        Write-Host "FAIL $($r.Port): received nothing -- no ESP-NOW audio reached this node" -ForegroundColor Red
        $fail = $true
        continue
    }
    $total = $r.Rx + $r.Lost
    $lossPct = 0.0
    if ($total -gt 0) { $lossPct = 100.0 * $r.Lost / $total }
    $issues = @()
    if ($lossPct -gt 1.0) { $issues += ("loss {0:N2}%" -f $lossPct) }
    if ($r.Ovf -gt 0)     { $issues += "ovf=$($r.Ovf)" }
    if ($r.Und -gt 0)     { $issues += "und=$($r.Und)" }
    if ($r.Mode -ne 'CLIENT') { $issues += "ended in $($r.Mode)" }

    if ($issues.Count -eq 0) {
        Write-Host ("PASS {0}: {1:N0} packets, loss {2:N2}%, buffer stable" -f $r.Port, $r.Rx, $lossPct) -ForegroundColor Green
    } else {
        Write-Host ("WARN {0}: {1}" -f $r.Port, ($issues -join ', ')) -ForegroundColor Yellow
    }

    # Time until the jitter buffer runs out of room, at the observed drift.
    if ($null -ne $r.JitBps -and [math]::Abs($r.JitBps) -gt 0.5) {
        $headroom = $JitterBufSize - $r.JitLast
        if ($r.JitBps -lt 0) { $headroom = $r.JitLast }
        $secs = $headroom / [math]::Abs($r.JitBps)
        $dir = 'overflow'
        if ($r.JitBps -lt 0) { $dir = 'underrun' }
        Write-Host ("     buffer drifting {0:N2} B/s -> {1} in about {2:N0} s ({3:N0} min)" -f `
            $r.JitBps, $dir, $secs, ($secs / 60)) -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host "Raw capture: $logPath"
if ($fail) { Write-Host 'Overall: FAIL' -ForegroundColor Red; exit 1 }
Write-Host 'Overall: PASS' -ForegroundColor Green
