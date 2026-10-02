<#
    Helpers shared by the scripts that talk to a node on USB and, through it,
    to the mesh: ota.ps1 and mesh.ps1. Dot-sourced, not run:

        . (Join-Path $PSScriptRoot 'common.ps1')

    bench-mesh.ps1 has its own versions, written before these, and keeps them.
#>

function Get-PioExe {
    $c = Get-Command pio -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (Test-Path $p) { return $p }
    throw "PlatformIO not found on PATH or at $p"
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

<#
The first whole line matching -Pattern. Whole: a line still arriving can match
on its first half, and then reads as an identity line with no name= in it.
#>
function Wait-ForLine {
    param($Sp, [string]$Pattern, [int]$TimeoutSec = 5)
    $buf = ''
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try { $buf += $Sp.ReadExisting() } catch {}
        $parts = $buf -split "`n"
        for ($i = 0; $i -lt $parts.Count - 1; $i++) {
            if ($parts[$i] -match $Pattern) { return $parts[$i].Trim() }
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

<#
A node on USB with ESP-NOW up, its port open. Not the node named -Exclude
(room name or MAC), when one is: a node about to reboot cannot relay for itself.
#>
function Find-Relay {
    param([string]$Exclude = '')
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
        if ($id -and (Get-Field $id 'espnow') -eq '1' -and
            (-not $Exclude -or ((Get-Field $id 'name') -ne $Exclude -and (Get-Field $id 'mac') -ne $Exclude))) {
            Write-Host ("  relay {0}: {1} mesh={2}" -f $p, (Get-Field $id 'name'), (Get-Field $id 'mesh'))
            return $sp
        }
        Close-Port $sp
    }
    return $null
}
