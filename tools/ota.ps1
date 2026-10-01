<#
.SYNOPSIS
    Update a node's firmware over the home WiFi, with no cable to it.

.DESCRIPTION
    For a node that has power and nothing else -- plugged into a stereo across
    the house, say. Any other node on USB relays the request over the mesh; the
    image itself goes over the home network. See D15 in docs/decisions.md.

    What it does:
      1. builds the image for -Env, and reads that environment's ROOM_NAME
      2. finds a node on USB with the mesh up, and has it broadcast `U<name>`
      3. the named node reboots into update mode and joins the home WiFi; this
         finds it by asking every address on the LAN for GET / (no mDNS: it
         costs 1.5 KB of DRAM on every boot, see D15)
      4. checks it is the node the image was built for, and posts the image
      5. asks again once the new image is running, reads its version back,
         and sends it back to the mesh. That second round is also what keeps
         the new image: until it answers, any reset rolls it back.

    The node needs the home network stored first, once, over USB:
    tools/ota-wifi.ps1. And it needs the two-slot partition table, which only a
    USB flash of a build from D15 onwards gives it.

.PARAMETER Env
    PlatformIO environment of the node to update, e.g. esp32wrover2.

.PARAMETER Relay
    Port of the node on USB that sends the request. It must be in the same
    mesh. Default: the first board found with ESP-NOW up.

.PARAMETER Target
    Name or MAC to ask for instead of the environment's ROOM_NAME -- for a node
    running under another name. bench-mesh.ps1 -Flash names every classic
    board SonoLoco-WROOM, for one.

.PARAMETER Ip
    The node's address, if the LAN scan cannot find it (another subnet, or a
    network wider than /24). The router's client list shows it under the
    lowercased room name.

.PARAMETER NoBuild
    Upload the image already built for -Env.

.PARAMETER Status
    No image at all: ask the node into update mode, print what it reports --
    its WiFi signal where it stands, and its stream counters up to the
    request -- and send it back to the mesh. How a speaker with no cable is
    measured: stream at it, then ask while the stream still plays.

.PARAMETER NoVerify
    Stop once the image is written, without the second round. The new image
    then stays on probation until it has run OTA_CONFIRM_MS with its radio up.

.EXAMPLE
    ./tools/ota.ps1 -Env esp32wrover2
    ./tools/ota.ps1 -Env esp32stereo -Status
    ./tools/ota.ps1 -Env esp32dev -Target 0A:1B:2C:3D:4E:60 -Relay COM9
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Env,
    [string]$Relay = '',
    [string]$Target = '',
    [string]$Ip = '',
    [switch]$NoBuild,
    [switch]$NoVerify,
    [switch]$Status
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # PowerShell 5.1's upload progress bar costs more than the upload
# .NET sends `Expect: 100-continue` on a POST and waits for an answer that
# esp_http_server never gives.
[System.Net.ServicePointManager]::Expect100Continue = $false

$repoRoot   = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot 'esp32-code'

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

function Get-RoomName {
    param([string]$EnvName)
    $ini = Get-Content (Join-Path $projectDir 'platformio.ini')
    $inSection = $false
    foreach ($line in $ini) {
        if ($line -match '^\s*\[(.+)\]') { $inSection = ($matches[1] -eq "env:$EnvName"); continue }
        if ($inSection -and $line -match "ROOM_NAME='`"(.+)`"'") { return $matches[1] }
    }
    return $null
}

function Open-Port {
    param([string]$Port)
    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
    $sp.ReadTimeout  = 200
    $sp.WriteTimeout = 500
    # As in bench-mesh.ps1: on the classic auto-reset circuit DTR drives GPIO0
    # and RTS drives EN, so asserting either can reset the relay mid-request.
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Open()
    return $sp
}

function Close-Port {
    param($Sp)
    if ($null -eq $Sp) { return }
    try { if ($Sp.IsOpen) { $Sp.Close() } } catch {}
    try { $Sp.Dispose() } catch {}
}

function Wait-ForLine {
    param($Sp, [string]$Pattern, [int]$TimeoutSec = 5)
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

function Get-Field {
    param([string]$Line, [string]$Key)
    if ($Line -match "(?:^|\s)$Key=(\S+)") { return $matches[1] }
    return $null
}

<# A node on USB with ESP-NOW up, that is not the node being updated. #>
function Find-Relay {
    param([string]$Want)
    $pio  = Get-PioExe
    $json = & $pio device list --json-output 2>$null
    $ports = @()
    foreach ($d in ($json | ConvertFrom-Json)) {
        # The same bridges bench-mesh.ps1 looks for: CP210x, CH34x, FTDI, Espressif native.
        if ($d.hwid -match 'VID:PID=(10C4|1A86|0403|303A)') { $ports += $d.port }
    }
    foreach ($p in ($ports | Sort-Object -Unique)) {
        $sp = $null
        try { $sp = Open-Port $p } catch { Write-Host "  $p busy, skipped"; continue }
        Start-Sleep -Milliseconds 300
        $null = $sp.ReadExisting()
        $sp.Write('?')
        $id = Wait-ForLine -Sp $sp -Pattern '\[BENCH\] id ' -TimeoutSec 3
        if ($id -and (Get-Field $id 'espnow') -eq '1' -and (Get-Field $id 'name') -ne $Want -and
            (Get-Field $id 'mac') -ne $Want) {
            Write-Host ("  relay {0}: {1} mesh={2}" -f $p, (Get-Field $id 'name'), (Get-Field $id 'mesh'))
            return $sp
        }
        Close-Port $sp
    }
    return $null
}

<# GET / on one address. The identity line, or $null. #>
function Get-Identity {
    param([string]$Address, [int]$TimeoutSec = 2)
    try {
        $r = Invoke-WebRequest -Uri "http://$Address/" -UseBasicParsing -TimeoutSec $TimeoutSec
        $text = [string]$r.Content
        if ($text -match '(?:^|\s)fw=\S+' -and $text -match '(?:^|\s)name=\S+') { return $text.Trim() }
    } catch {}
    return $null
}

function Test-IsTarget {
    param([string]$Identity, [string]$Want)
    return ((Get-Field $Identity 'name') -eq $Want) -or ((Get-Field $Identity 'mac') -eq $Want)
}

<#
The /24 of every interface with a default route, as its first three octets --
the home LAN, and not the virtual switches Docker and WSL add. A wider network
is scanned only in the PC's own /24; -Ip covers the rest.
#>
function Get-LocalPrefixes {
    $routed = @((Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue).InterfaceIndex)
    $prefixes = @()
    foreach ($a in (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue)) {
        if ($routed -notcontains $a.InterfaceIndex) { continue }
        if ($a.IPAddress -like '169.254.*') { continue }
        $prefixes += ($a.IPAddress -replace '\.\d+$', '')
    }
    return ($prefixes | Sort-Object -Unique)
}

<#
Who on the LAN answers GET / as the node we want? Port 80 is tried on every
address at once, and only the few that accept are asked. A few seconds for a
/24, and it needs nothing from the router or from Windows' name resolution.
#>
function Find-Node {
    param([string]$Want, [string[]]$TryFirst)
    foreach ($a in $TryFirst) {
        if (-not $a) { continue }
        $id = Get-Identity $a
        if ($id -and (Test-IsTarget $id $Want)) { return @($a, $id) }
    }
    foreach ($prefix in (Get-LocalPrefixes)) {
        $pending = @()
        foreach ($i in 1..254) {
            $c = New-Object System.Net.Sockets.TcpClient
            $pending += ,@("$prefix.$i", $c, $c.ConnectAsync("$prefix.$i", 80))
        }
        Start-Sleep -Milliseconds 800
        $open = @()
        foreach ($p in $pending) {
            if ($p[2].Status -eq 'RanToCompletion' -and $p[1].Connected) { $open += $p[0] }
            try { $p[1].Dispose() } catch {}
        }
        foreach ($a in $open) {
            $id = Get-Identity $a
            if ($id -and (Test-IsTarget $id $Want)) { return @($a, $id) }
        }
    }
    return $null
}

<#
Send `U<name>` and wait until the node answers on the LAN, or give up. The
request is repeated every 20 s: broadcast is unacknowledged, and a node that
was still booting when it went out never heard it.
#>
function Request-UpdateMode {
    param($Sp, [string]$Want, [string[]]$TryFirst, [int]$TimeoutSec = 80)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $nextAsk  = Get-Date
    while ((Get-Date) -lt $deadline) {
        if ((Get-Date) -ge $nextAsk) {
            $null = $Sp.ReadExisting()
            $Sp.Write("U$Want`n")
            $ack = Wait-ForLine -Sp $Sp -Pattern '\[OTA\] (request|error)' -TimeoutSec 3
            if (-not $ack)           { throw "The relay did not acknowledge U$Want -- is it running D15 firmware?" }
            if ($ack -match 'error') { throw "The relay refused: $ack" }
            $nextAsk = (Get-Date).AddSeconds(20)
        }
        $found = Find-Node -Want $Want -TryFirst $TryFirst
        if ($found) { return $found }
        Start-Sleep -Seconds 2
    }
    return $null
}

# ---------------------------------------------------------------------------
# 1. What to build, and for whom
# ---------------------------------------------------------------------------

$room = Get-RoomName $Env
if (-not $room) { throw "No ROOM_NAME for [env:$Env] in platformio.ini" }
if (-not $Target) { $Target = $room }

$version = (& git -C $projectDir describe --tags --always --dirty=* 2>$null)
Write-Host ''
Write-Host '=== update over WiFi ===' -ForegroundColor Cyan
Write-Host "image   : $Env ($room) $version"
Write-Host "target  : $Target"
if ($version -like '*`*') {
    Write-Host '  note: dirty tree -- the version on the board will not name a commit' -ForegroundColor Yellow
}

$buildDir = if ($env:PLATFORMIO_BUILD_DIR) { $env:PLATFORMIO_BUILD_DIR } else { Join-Path $projectDir '.pio\build' }
$bin = Join-Path (Join-Path $buildDir $Env) 'firmware.bin'
if ($Status) { $NoBuild = $true }
if (-not $NoBuild) {
    Write-Host '  building ...' -NoNewline
    $pio = Get-PioExe
    # The toolchain writes warnings to stderr, which 'Stop' would turn into a
    # terminating error despite a zero exit code.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = & $pio run -d $projectDir -e $Env 2>&1 | Out-String } finally { $ErrorActionPreference = $prevEap }
    if ($LASTEXITCODE -ne 0) { Write-Host ''; Write-Host $out; throw "Build failed (exit $LASTEXITCODE)" }
    Write-Host ' done'
}
if (-not $Status) {
    if (-not (Test-Path $bin)) { throw "No image at $bin -- build without -NoBuild" }
    Write-Host ("  image {0:N0} bytes" -f (Get-Item $bin).Length)
}

# ---------------------------------------------------------------------------
# 2. The relay
# ---------------------------------------------------------------------------

$relaySp = $null
try {
    if ($Relay) {
        $relaySp = Open-Port $Relay
        Start-Sleep -Milliseconds 300
    } else {
        $relaySp = Find-Relay -Want $Target
        if (-not $relaySp) { throw 'No node on USB with the mesh up to relay the request (or pass -Relay COMx)' }
    }

    # -----------------------------------------------------------------------
    # 3. Ask, and find it on the LAN
    # -----------------------------------------------------------------------

    Write-Host "  asking $Target to enter update mode ..." -NoNewline
    $found = Request-UpdateMode -Sp $relaySp -Want $Target -TryFirst @($Ip)
    if (-not $found) {
        Write-Host ''
        throw ("$Target did not appear on the LAN. It ignores the request while it is serving a phone, " +
               "and a node in another mesh never hears it. If it beeped twice then fell, it could not " +
               "join the WiFi: store it again with tools/ota-wifi.ps1. Otherwise pass -Ip.")
    }
    $addr, $before = $found
    Write-Host " at $addr"
    Write-Host "  running : $before"

    if ($Status) {
        try { $null = Invoke-WebRequest -Uri "http://$addr/exit" -Method Post -UseBasicParsing -TimeoutSec 5 } catch {}
        Write-Host '  sent back to the mesh, nothing uploaded'
        return
    }

    # -----------------------------------------------------------------------
    # 4. Upload
    # -----------------------------------------------------------------------

    Write-Host '  uploading ...' -NoNewline
    $t0 = Get-Date
    try {
        $r = Invoke-WebRequest -Uri "http://$addr/update" -Method Post -InFile $bin `
                               -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec 180
        $reply = ([string]$r.Content).Trim()
    } catch {
        $reply = "$_"
        try {
            $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
            $reply = $reader.ReadToEnd().Trim()
        } catch {}
    }
    $secs = ((Get-Date) - $t0).TotalSeconds
    if ($reply -notmatch '^OK') {
        Write-Host ''
        throw "Upload refused: $reply (the node waits 5 minutes for another attempt, then goes back to the mesh)"
    }
    Write-Host (" {0} in {1:N1} s" -f $reply, $secs)

    if ($NoVerify) {
        Write-Host '  written; not verified (-NoVerify). It is kept once it has run a minute with its radio up.'
        return
    }

    # -----------------------------------------------------------------------
    # 5. Is it running, and is it the new one?
    # -----------------------------------------------------------------------

    Write-Host '  waiting for the new image to boot ...' -NoNewline
    Start-Sleep -Seconds 12   # boot, the mesh up, and the node listening again
    $found = Request-UpdateMode -Sp $relaySp -Want $Target -TryFirst @($addr)
    if (-not $found) {
        Write-Host ''
        Write-Host ("  FAIL the new image did not answer. If its radio never came up it has not been kept, " +
                    "and the next power cycle boots the previous one.") -ForegroundColor Red
        exit 1
    }
    $addr, $after = $found
    Write-Host ' up'
    Write-Host "  running : $after"
    try { $null = Invoke-WebRequest -Uri "http://$addr/exit" -Method Post -UseBasicParsing -TimeoutSec 5 } catch {}

    # Answering the second request is what kept the image (otaRebootIntoUpdate),
    # so pending=0 here is expected, not evidence.
    # The slot as well as the version: an image of the version already running
    # passes the version check whether or not it was ever booted.
    $fw = Get-Field $after 'fw'
    $rolledBack = Get-Field $after 'rolledback'
    $slotBefore = Get-Field $before 'app'
    $slotAfter  = Get-Field $after 'app'
    if ($fw -eq $version -and $slotAfter -ne $slotBefore) {
        Write-Host "  PASS $Target runs $fw from $slotAfter, and keeps it" -ForegroundColor Green
    } elseif ($slotAfter -eq $slotBefore -or $rolledBack -ne '-') {
        Write-Host "  FAIL the new image was rolled back ($rolledBack); $Target is back on $fw" -ForegroundColor Red
        exit 1
    } else {
        Write-Host "  FAIL $Target runs $fw, expected $version" -ForegroundColor Red
        exit 1
    }
} finally {
    Close-Port $relaySp
}
