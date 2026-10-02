<#
.SYNOPSIS
    Run a command on any node of the mesh, or see who is in it and how they hear each other.

.DESCRIPTION
    Every serial command runs on another node too, over the mesh: any node on USB
    sends `@<target> <command>` and prints the answers (D16 in docs/decisions.md).
    This script is that, from the PC.

    With no -Command it asks every node for `N` and draws the mesh as the star it
    is: each source -- a server's phone, or a bench tone -- with the clients locked
    to it, and for each client what hearing it costs in the current stream,
    packets received and lost. Nodes in DISCOVERY are listed apart.

    Only nodes in the relay's own mesh hear the question, and only firmware from
    D16 onwards answers it: a node on older firmware is simply missing from the
    list. Update it with tools/ota.ps1, which still reaches it.

.PARAMETER Command
    The command, exactly as it would be typed on the node's own port: `v-20`,
    `?`, `r`, `m`. Omitted: `N`, drawn as the mesh.

.PARAMETER Target
    Room name or MAC of the node to run it on, or * for every node (the default).
    A command that reboots a node or takes it off the mesh is refused for *.

.PARAMETER Relay
    Port of the node on USB that asks. Default: the first board found with the
    mesh up.

.EXAMPLE
    ./tools/mesh.ps1                                        # who is there, and who hears whom
    ./tools/mesh.ps1 -Target SonoLoco-Stereo -Command v-20  # that speaker's volume trim
    ./tools/mesh.ps1 -Command v                             # every node's trim
    ./tools/mesh.ps1 -Target SonoLoco-Stereo -Command r     # its full status line
#>

[CmdletBinding()]
param(
    [string]$Command = '',
    [string]$Target  = '*',
    [string]$Relay   = ''
)

$ErrorActionPreference = 'Stop'

# Get-PioExe, Open-Port, Close-Port, Wait-ForLine, Get-Field, Find-Relay
. (Join-Path $PSScriptRoot 'common.ps1')

$listing = -not $Command
if ($listing) { $Command = 'N' }

<# Send the question; every line up to `[CMD] done`, or the error that stopped it. #>
function Invoke-MeshCommand {
    param($Sp, [string]$Line)
    $null = $Sp.ReadExisting()
    $Sp.Write("@$Line`n")
    $lines = New-Object System.Collections.Generic.List[string]
    $buf = ''
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        try { $buf += $Sp.ReadExisting() } catch {}
        while ($buf.Contains("`n")) {
            $i = $buf.IndexOf("`n")
            $l = $buf.Substring(0, $i).TrimEnd("`r")
            $buf = $buf.Substring($i + 1)
            $lines.Add($l)
            if ($l -match '^\[CMD\] (done|error)') { return $lines }
        }
        Start-Sleep -Milliseconds 50
    }
    return $lines
}

<# `[@name] text` lines into (name, text) pairs, a line cut over two packets (`[@name]+`) joined again. #>
function Get-Replies {
    param($Lines)
    $replies = New-Object System.Collections.Generic.List[object]
    foreach ($l in $Lines) {
        if ($l -notmatch '^\[@([^\]]+)\](\+?) ?(.*)$') { continue }
        $name, $cont, $text = $matches[1], $matches[2], $matches[3]
        $last = $null
        for ($i = $replies.Count - 1; $i -ge 0; $i--) {
            if ($replies[$i].Name -eq $name) { $last = $replies[$i]; break }
        }
        if ($cont -and $last) { $last.Text += $text }
        else { $replies.Add([pscustomobject]@{ Name = $name; Text = $text }) }
    }
    return $replies
}

function Format-Loss {
    param($Node)
    $rx = [long]$Node.rx; $lost = [long]$Node.lost
    if ($rx + $lost -eq 0) { return 'no stream' }
    $pct = 100.0 * $lost / ($rx + $lost)
    return ('rx={0:N0} lost={1:N0} ({2:N2}%) und={3}' -f $rx, $lost, $pct, $Node.und)
}

$sp = $null
try {
    if ($Relay) {
        $sp = Open-Port $Relay
        Start-Sleep -Milliseconds 300
    } else {
        $sp = Find-Relay
        if (-not $sp) { throw 'No node on USB with the mesh up (or pass -Relay COMx)' }
    }
    $null = $sp.ReadExisting()
    $sp.Write('?')
    $relayId = Wait-ForLine -Sp $sp -Pattern '\[BENCH\] id ' -TimeoutSec 3

    $lines = Invoke-MeshCommand -Sp $sp -Line "$Target $Command"
    $err = $lines | Where-Object { $_ -match '^\[CMD\] error' } | Select-Object -First 1
    if ($err) { throw "The relay refused: $err" }
    $replies = Get-Replies $lines

    if (-not $listing) {
        foreach ($r in $replies) { Write-Host ('{0,-18} {1}' -f $r.Name, $r.Text) }
        if ($replies.Count -eq 0) { Write-Host 'No answer.' -ForegroundColor Yellow }
        return
    }

    # -----------------------------------------------------------------------
    # The listing, drawn as the mesh
    # -----------------------------------------------------------------------

    $nodes = @()
    foreach ($r in $replies) {
        if ($r.Text -notmatch '^\[NODE\] ') { continue }
        $n = [ordered]@{}
        foreach ($k in 'name', 'mac', 'fw', 'mode', 'src', 'rx', 'lost', 'und', 'up', 'trim', 'mute') {
            $n[$k] = Get-Field $r.Text $k
        }
        $nodes += [pscustomobject]$n
    }

    Write-Host ''
    Write-Host ('mesh {0} ({1}), asked through {2}: {3} node(s) answered' -f
                (Get-Field $relayId 'mesh'), (Get-Field $relayId 'meshname'), $sp.PortName, $nodes.Count) -ForegroundColor Cyan
    Write-Host ''

    function Write-Node {
        param($N, [string]$Indent, [string]$What)
        $mute = ''
        if ($N.mute -eq '1') { $mute = '  MUTED' }
        Write-Host ('{0}{1,-18} {2,-38} trim={3,3}dB  {4}{5}' -f $Indent, $N.name, $What, $N.trim, $N.fw, $mute)
    }

    $shown = @{}
    foreach ($s in ($nodes | Where-Object { $_.mode -eq 'SERVER' })) {
        Write-Node $s '' ("SERVER ({0})" -f $s.src)
        $shown[$s.mac] = $true
        foreach ($c in ($nodes | Where-Object { $_.mode -eq 'CLIENT' -and $_.src -eq $s.mac })) {
            Write-Node $c '  +-- ' (Format-Loss $c)
            $shown[$c.mac] = $true
        }
    }
    # Clients of a source that did not answer: older firmware, or out of the relay's reach.
    foreach ($src in ($nodes | Where-Object { $_.mode -eq 'CLIENT' -and -not $shown.ContainsKey($_.mac) } |
                      Select-Object -ExpandProperty src -Unique)) {
        Write-Host ('{0,-18} source that did not answer' -f $src) -ForegroundColor DarkGray
        foreach ($c in ($nodes | Where-Object { $_.mode -eq 'CLIENT' -and $_.src -eq $src })) {
            Write-Node $c '  +-- ' (Format-Loss $c)
            $shown[$c.mac] = $true
        }
    }
    foreach ($d in ($nodes | Where-Object { -not $shown.ContainsKey($_.mac) })) {
        Write-Node $d '' ("{0}, up {1}" -f $d.mode, $d.up)
    }
    Write-Host ''
} finally {
    Close-Port $sp
}
