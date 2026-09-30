<#
.SYNOPSIS
    Store the home WiFi on a node, for update mode. Once per node, over USB.

.DESCRIPTION
    Update mode (D15) joins the home network to receive an image, so a node has
    to know it before it goes anywhere without a cable. This asks for the
    network and password, sends them with the `W` command, and reads back what
    the node stored -- the SSID, and only whether there is a password.

    The password is typed here, masked, and goes straight to the board. It is
    not echoed, not logged, and not an argument, so it stays out of the shell
    history. On the board it sits in NVS, in plain text: anyone holding the
    board with a USB cable can read it back, as with any ESP32 device.

.PARAMETER Port
    The node's port, e.g. COM22.

.PARAMETER Ssid
    The network name. Asked for when omitted.

.EXAMPLE
    ./tools/ota-wifi.ps1 -Port COM22
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Ssid = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Ssid) { $Ssid = Read-Host 'Home WiFi name (SSID)' }
if (-not $Ssid) { throw 'No SSID' }
if ($Ssid.Length -gt 32) { throw 'An SSID is at most 32 characters' }
$secure = Read-Host 'WiFi password (blank for an open network)' -AsSecureString

$sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
$sp.Encoding     = [System.Text.Encoding]::UTF8   # an SSID may not be ASCII
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 500
$sp.DtrEnable    = $false   # either line can reset a classic board, see bench-mesh.ps1
$sp.RtsEnable    = $false
$sp.Open()

$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    Start-Sleep -Milliseconds 300
    $null = $sp.ReadExisting()
    $pass = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    if ($pass.Length -gt 64) { throw 'A WiFi password is at most 64 characters' }
    # "\n" only: the node reads the password as the line after the SSID, and a
    # "\r\n" would end the SSID at the \r and leave an empty password line.
    $sp.Write("W$Ssid`n$pass`n")
} finally {
    $pass = $null
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}

$buf = ''
$deadline = (Get-Date).AddSeconds(3)
$reply = $null
while ((Get-Date) -lt $deadline -and -not $reply) {
    try { $buf += $sp.ReadExisting() } catch {}
    foreach ($l in ($buf -split "`n")) { if ($l -match '\[OTA\] wifi ') { $reply = $l.Trim() } }
    Start-Sleep -Milliseconds 50
}
$sp.Close()
$sp.Dispose()

if (-not $reply) { throw "$Port did not answer -- is it running D15 firmware?" }
Write-Host "  $reply"
if ($reply -notmatch [regex]::Escape("ssid=$Ssid ")) { throw 'The node did not store that SSID' }
