#requires -Version 5.1
# Self-contained checks for ShellConnect.psm1 using a loopback mock of the Shell
# serverinfo endpoint and a loopback UDP listener. No packet leaves this machine:
# every network call targets 127.0.0.1 and broadcast is disabled.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ShellConnect.psm1') -Force

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [bool]$Condition, [string]$Detail = '') {
    if ($Condition) { $script:Pass++; Write-Host "  ok   $Name" }
    else { $script:Fail++; Write-Host "  FAIL $Name $Detail" -ForegroundColor Red }
}
function Throws([string]$Name, [scriptblock]$Block, [string]$Pattern = '') {
    try { & $Block | Out-Null; Check $Name $false '(no exception)' }
    catch { Check $Name ($Pattern -eq '' -or $_.Exception.Message -match $Pattern) "($($_.Exception.Message))" }
}
function Get-FreePort {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start(); $port = ([System.Net.IPEndPoint]$l.LocalEndpoint).Port; $l.Stop(); $port
}

$ServerInfoXml = '<?xml version="1.0" encoding="utf-8"?><root status_code="200"><hostname>MOCK-HOST</hostname><appversion>7.1.431.-1</appversion><GfeVersion>3.23.0.74</GfeVersion><uniqueid>11111111-2222-3333-4444-555555555555</uniqueid><HttpsPort>47984</HttpsPort><ExternalPort>47989</ExternalPort><mac>00:00:00:00:00:00</mac><LocalIP>127.0.0.1</LocalIP><state>SUNSHINE_SERVER_FREE</state><PairStatus>0</PairStatus></root>'

# Minimal HTTP/1.1 responder in a separate runspace. $State is a synchronized hashtable:
#   Port, Body, StatusLine, DelaySec, Stop, Requests
function Start-MockServer([hashtable]$State) {
    $ps = [powershell]::Create()
    $null = $ps.AddScript({
        param($State)
        if ($State.DelaySec -gt 0) { Start-Sleep -Seconds $State.DelaySec }
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $State.Port)
        $listener.Start()
        $State.Listening = $true
        try {
            while (-not $State.Stop) {
                if (-not $listener.Pending()) { Start-Sleep -Milliseconds 30; continue }
                $client = $listener.AcceptTcpClient()
                # Port probes connect and close without sending anything; treat every client
                # failure as that client's problem so the server keeps serving.
                try {
                    $client.ReceiveTimeout = 2000
                    $stream = $client.GetStream()
                    $buf = [byte[]]::new(4096); $got = ''
                    while ($got -notmatch "`r`n`r`n") {
                        $n = $stream.Read($buf, 0, $buf.Length); if ($n -le 0) { break }
                        $got += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
                    }
                    if ($got -match '^GET ') {
                        $State.Requests++
                        $body = [System.Text.Encoding]::UTF8.GetBytes([string]$State.Body)
                        $head = "$($State.StatusLine)`r`nContent-Type: text/xml`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n"
                        $hb = [System.Text.Encoding]::ASCII.GetBytes($head)
                        $stream.Write($hb, 0, $hb.Length); $stream.Write($body, 0, $body.Length); $stream.Flush()
                    }
                } catch { $State.ClientErrors++ }
                finally { $client.Close() }
            }
        } finally { $listener.Stop() }
    }).AddArgument($State)
    $handle = $ps.BeginInvoke()
    [pscustomobject]@{ PowerShell = $ps; Handle = $handle; State = $State }
}
function Stop-MockServer($server) {
    $server.State.Stop = $true
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $server.Handle.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if ($server.Handle.IsCompleted) { try { $server.PowerShell.EndInvoke($server.Handle) | Out-Null } catch { Write-Host "  mock server error: $($_.Exception.Message)" } }
    else { $server.PowerShell.Stop() }
    $server.PowerShell.Dispose()
}
function New-MockState([int]$Port, [string]$Body = $ServerInfoXml, [string]$StatusLine = 'HTTP/1.1 200 OK', [int]$DelaySec = 0) {
    [hashtable]::Synchronized(@{ Port = $Port; Body = $Body; StatusLine = $StatusLine; DelaySec = $DelaySec; Stop = $false; Requests = 0; ClientErrors = 0; Listening = $false })
}
function Wait-Listening([hashtable]$State, [int]$Sec = 5) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Sec)
    while (-not $State.Listening -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 30 }
}

Write-Host 'Magic packet'
$pkt = New-ShellMagicPacket 'aa:bb:cc:dd:ee:ff'
Check 'packet is 102 bytes' ($pkt.Length -eq 102)
Check 'packet header is 6 x FF' ((($pkt[0..5] | ForEach-Object { $_ -eq 0xFF }) -notcontains $false))
$mac = @(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF); $repeatOk = $true
for ($r = 0; $r -lt 16; $r++) { for ($i = 0; $i -lt 6; $i++) { if ($pkt[6 + $r * 6 + $i] -ne $mac[$i]) { $repeatOk = $false } } }
Check 'MAC repeated 16 times' $repeatOk
Check 'dash format accepted' ((New-ShellMagicPacket 'AA-BB-CC-DD-EE-FF')[101] -eq 0xFF)
Check 'dot format accepted' ((New-ShellMagicPacket 'aabb.ccdd.eeff')[6] -eq 0xAA)
Check 'raw hex accepted' ((New-ShellMagicPacket 'AABBCCDDEEFF')[7] -eq 0xBB)
Throws 'too short rejected' { New-ShellMagicPacket 'AA:BB:CC' } '12 hex'
Throws 'non-hex rejected' { New-ShellMagicPacket 'GG:BB:CC:DD:EE:FF' } '12 hex'
Throws 'Shell placeholder MAC rejected' { New-ShellMagicPacket '00:00:00:00:00:00' } 'placeholder'
Throws 'broadcast MAC rejected' { New-ShellMagicPacket 'FF:FF:FF:FF:FF:FF' } 'Broadcast'
Throws 'multicast MAC rejected' { New-ShellMagicPacket '01:00:5E:00:00:01' } 'Multicast'

Write-Host 'Address classes'
$classes = @{
    '127.0.0.1' = 'Loopback'; '192.168.0.5' = 'LAN'; '10.1.2.3' = 'LAN'; '172.16.0.1' = 'LAN'; '172.32.0.1' = 'Internet'
    '169.254.7.7' = 'LinkLocal'; '100.100.1.1' = 'TailscaleOrCgnat'; '100.128.0.1' = 'Internet'; '8.8.8.8' = 'Internet'
    '255.255.255.255' = 'LimitedBroadcast'; '::1' = 'Loopback'; 'fe80::1' = 'LinkLocal'; 'fd7a:115c:a1e0::1' = 'Tailscale'
    'fd00::1' = 'UniqueLocal'; '2001:db8::1' = 'Internet'
}
foreach ($k in $classes.Keys) { Check "class $k = $($classes[$k])" ((Get-ShellAddressClass ([ipaddress]$k)) -eq $classes[$k]) }

Write-Host 'Server info parsing'
$info = ConvertFrom-ShellServerInfo $ServerInfoXml
Check 'status_code parsed' ($info.StatusCode -eq 200)
Check 'hostname parsed' ($info.Hostname -eq 'MOCK-HOST')
Check 'uniqueid parsed' ($info.UniqueId -eq '11111111-2222-3333-4444-555555555555')
Check 'state parsed' ($info.State -eq 'SUNSHINE_SERVER_FREE')
Throws 'DTD rejected' { ConvertFrom-ShellServerInfo '<!DOCTYPE root [<!ENTITY x "y">]><root status_code="200"><hostname>&x;</hostname></root>' }
Throws 'wrong root rejected' { ConvertFrom-ShellServerInfo '<html><body>captive portal</body></html>' } 'serverinfo'
Throws 'garbage rejected' { ConvertFrom-ShellServerInfo 'not xml at all' }

Write-Host 'Target resolution'
Check 'IP literal resolves to itself' ((Resolve-ShellTarget '127.0.0.1').Addresses[0].ToString() -eq '127.0.0.1')
Check 'invalid host name rejected' (-not (Resolve-ShellTarget 'bad name!').Resolved)
Check 'unresolvable name reported' (-not (Resolve-ShellTarget 'host.does-not-exist.invalid').Resolved)
$nets = @(Get-ShellLocalNetwork)
Check 'local networks carry broadcast addresses' ((@($nets | Where-Object { -not ($_.Broadcast -is [ipaddress]) }).Count -eq 0))
Check 'local networks exclude loopback' ((@($nets | Where-Object { [ipaddress]::IsLoopback($_.Address) }).Count -eq 0))
Check 'loopback is not same subnet' (-not (Test-ShellSameSubnet -Address ([ipaddress]'127.0.0.1')))
$fakeNet = @([pscustomobject]@{ Interface = 'x'; Address = [ipaddress]'192.168.50.10'; Mask = [ipaddress]'255.255.255.0'; Network = [ipaddress]'192.168.50.0'; Broadcast = [ipaddress]'192.168.50.255' })
Check 'same subnet detected with supplied networks' (Test-ShellSameSubnet -Address ([ipaddress]'192.168.50.77') -LocalNetworks $fakeNet)
Check 'other subnet rejected with supplied networks' (-not (Test-ShellSameSubnet -Address ([ipaddress]'192.168.51.77') -LocalNetworks $fakeNet))

Write-Host 'Host probe'
$closed = Get-FreePort
$probe = Test-ShellHost -Target 127.0.0.1 -BasePort $closed -TimeoutMs 500
Check 'closed port is not reachable' (-not $probe.Reachable)
Check 'closed port attempt recorded' ($probe.Attempts.Count -eq 1 -and -not $probe.Attempts[0].HttpPortOpen)
Throws 'privileged base port rejected' { Test-ShellHost -Target 127.0.0.1 -BasePort 80 } 'BasePort'

$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port); Wait-Listening $srv.State
try {
    $probe = Test-ShellHost -Target 127.0.0.1 -BasePort $port
    Check 'mock host reachable' $probe.Reachable
    Check 'hostname reported' ($probe.Hostname -eq 'MOCK-HOST')
    Check 'path class loopback' ($probe.PathClass -eq 'Loopback')
    Check 'HTTPS port reported closed on mock' (-not $probe.Attempts[0].HttpsPortOpen)
    Check 'no MAC field in result' (-not ($probe.PSObject.Properties.Name -contains 'Mac'))
    $probe = Test-ShellHost -Target 127.0.0.1 -BasePort $port -ExpectedUniqueId '11111111-2222-3333-4444-555555555555'
    Check 'expected uniqueid matches' ($probe.Reachable -and $probe.UniqueIdMatches -eq $true)
    $probe = Test-ShellHost -Target 127.0.0.1 -BasePort $port -ExpectedUniqueId 'other-id'
    Check 'unexpected uniqueid is not reachable' (-not $probe.Reachable -and $probe.Attempts[0].UniqueIdMatches -eq $false)
    Check 'mock served requests' ($srv.State.Requests -ge 3)
} finally { Stop-MockServer $srv }

$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port -StatusLine 'HTTP/1.1 503 Service Unavailable'); Wait-Listening $srv.State
try { Check 'HTTP 503 is not reachable' (-not (Test-ShellHost -Target 127.0.0.1 -BasePort $port).Reachable -and $srv.State.Requests -eq 1) } finally { Stop-MockServer $srv }
$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port -Body '<root status_code="401"></root>'); Wait-Listening $srv.State
try { Check 'status_code 401 is not reachable' (-not (Test-ShellHost -Target 127.0.0.1 -BasePort $port).Reachable -and $srv.State.Requests -eq 1) } finally { Stop-MockServer $srv }
$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port -Body '<html>not hermit</html>'); Wait-Listening $srv.State
try {
    $probe = Test-ShellHost -Target 127.0.0.1 -BasePort $port
    Check 'non-serverinfo body is not reachable and does not throw' (-not $probe.Reachable -and $probe.Attempts[0].Error -match 'serverinfo')
} finally { Stop-MockServer $srv }

Write-Host 'Wake packets'
$udp = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
$udpPort = ([System.Net.IPEndPoint]$udp.Client.LocalEndPoint).Port
try {
    $wake = Send-ShellWake -MacAddress '12:34:56:78:9A:BC' -Target 127.0.0.1 -Port $udpPort -NoBroadcast
    Check 'wake reports sent' $wake.AnySent
    Check 'one delivery for one port' ($wake.Deliveries.Count -eq 1 -and $wake.Deliveries[0].Kind -eq 'Directed')
    Check 'directed class recorded' ($wake.DirectedClasses -contains 'Loopback')
    Check 'broadcast flag off' (-not $wake.BroadcastUsed)
    $ar = $udp.BeginReceive($null, $null)
    Check 'datagram received' ($ar.AsyncWaitHandle.WaitOne(2000))
    $ep = $null; $data = $udp.EndReceive($ar, [ref]$ep)
    Check 'received 102 bytes' ($data.Length -eq 102)
    $expected = New-ShellMagicPacket '12:34:56:78:9A:BC'
    Check 'received payload equals magic packet' (@(Compare-Object $data $expected -SyncWindow 0).Count -eq 0)
} finally { $udp.Close() }
$wake = Send-ShellWake -MacAddress '12:34:56:78:9A:BC' -Target 127.0.0.1 -NoBroadcast
$ports = @($wake.Deliveries | Select-Object -ExpandProperty Port)
Check 'default ports match Hermit' ((($ports | Sort-Object) -join ',') -eq '9,47009,47998,47999,48000,48002,48010')
$wake = Send-ShellWake -MacAddress '12:34:56:78:9A:BC' -Target 127.0.0.1 -BasePort 48989 -Port @(9) -NoBroadcast
Check 'custom port list honoured' ($wake.Deliveries.Count -eq 1 -and $wake.Deliveries[0].Port -eq 9)
$wake = Send-ShellWake -MacAddress '12:34:56:78:9A:BC' -Target 'host.does-not-exist.invalid' -Port @(9) -NoBroadcast
Check 'unresolved target reported, nothing sent' (-not $wake.AnySent -and $wake.Deliveries[0].Kind -eq 'Unresolved')
Throws 'invalid port rejected' { Send-ShellWake -MacAddress '12:34:56:78:9A:BC' -Target 127.0.0.1 -Port @(70000) -NoBroadcast } 'port'

Write-Host 'Readiness wait'
$closed = Get-FreePort
$w = Wait-ShellReady -Target 127.0.0.1 -BasePort $closed -TimeoutSec 2 -IntervalSec 1 -ProbeTimeoutMs 300
Check 'timeout reported when nothing listens' (-not $w.Ready -and $w.TimedOut -and $w.ElapsedSec -ge 2 -and $w.Attempts -ge 2)
$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port -DelaySec 3)
try {
    $w = Wait-ShellReady -Target 127.0.0.1 -BasePort $port -TimeoutSec 20 -IntervalSec 1 -ProbeTimeoutMs 300
    Check 'ready after delayed start' ($w.Ready -and -not $w.TimedOut)
    Check 'first response after the delay' ($w.FirstResponseSec -ge 2)
    Check 'ready confirmed twice' ($w.Attempts -ge 3)
} finally { Stop-MockServer $srv }
Throws 'zero timeout rejected' { Wait-ShellReady -Target 127.0.0.1 -TimeoutSec 0 } 'TimeoutSec'

Write-Host 'Verdict rules'
$base = @{ InitiallyOnline = $false; MacProvided = $true; AnyPacketSent = $true; BroadcastUsed = $false; DirectedClasses = @('Internet'); SenderOnHostLan = $false; Ready = $true }
function Verdict([hashtable]$Override) { $p = $base.Clone(); foreach ($k in $Override.Keys) { $p[$k] = $Override[$k] }; Get-ShellWakeVerdict @p }
Check 'already online' ((Verdict @{ InitiallyOnline = $true }) -eq 'AlreadyOnline')
Check 'no MAC' ((Verdict @{ MacProvided = $false }) -eq 'NoMacAddress')
Check 'nothing sent' ((Verdict @{ AnyPacketSent = $false }) -eq 'NoPacketSent')
Check 'not ready' ((Verdict @{ Ready = $false }) -eq 'WakeNotConfirmed')
Check 'remote path from outside' ((Verdict @{}) -eq 'WokeViaRemotePath')
Check 'broadcast caps at LAN' ((Verdict @{ BroadcastUsed = $true }) -eq 'WokeOnLan')
Check 'same subnet caps at LAN' ((Verdict @{ SenderOnHostLan = $true }) -eq 'WokeOnLan')
Check 'LAN directed target caps at LAN' ((Verdict @{ DirectedClasses = @('Internet', 'LAN') }) -eq 'WokeOnLan')
Check 'unknown sender position is not remote' ((Verdict @{ SenderOnHostLan = $null }) -eq 'WokeUnknownPath')
Check 'loopback only is not remote' ((Verdict @{ DirectedClasses = @('Loopback') }) -eq 'WokeUnknownPath')
Check 'tailscale directed from outside counts as remote' ((Verdict @{ DirectedClasses = @('Tailscale') }) -eq 'WokeViaRemotePath')

Write-Host 'Wake sequence (mock)'
$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port); Wait-Listening $srv.State
try {
    $seq = Invoke-ShellWakeSequence -Target 127.0.0.1 -MacAddress '12:34:56:78:9A:BC' -BasePort $port -NoBroadcast -TimeoutSec 5
    Check 'online host yields AlreadyOnline and no wake' ($seq.Verdict -eq 'AlreadyOnline' -and $null -eq $seq.Wake)
} finally { Stop-MockServer $srv }
$closed = Get-FreePort
$seq = Invoke-ShellWakeSequence -Target 127.0.0.1 -BasePort $closed -NoBroadcast -TimeoutSec 2
Check 'offline host without MAC yields NoMacAddress' ($seq.Verdict -eq 'NoMacAddress' -and $null -eq $seq.Wake)
$port = Get-FreePort
$srv = Start-MockServer (New-MockState $port -DelaySec 3)
try {
    $seq = Invoke-ShellWakeSequence -Target 127.0.0.1 -MacAddress '12:34:56:78:9A:BC' -BasePort $port -NoBroadcast -TimeoutSec 20 -IntervalSec 1 -Scenario JustShutDown
    Check 'sequence woke mock host' ($seq.Wait.Ready)
    Check 'scenario recorded' ($seq.Scenario -eq 'JustShutDown')
    Check 'loopback wake is labelled unknown path, not remote' ($seq.Verdict -eq 'WokeUnknownPath')
    Check 'verdict text present' (-not [string]::IsNullOrEmpty($seq.VerdictText))
    Check 'no broadcast deliveries when disabled' (@($seq.Wake.Deliveries | Where-Object Kind -ne 'Directed').Count -eq 0)
} finally { Stop-MockServer $srv }
$closed = Get-FreePort
$seq = Invoke-ShellWakeSequence -Target 127.0.0.1 -MacAddress '12:34:56:78:9A:BC' -BasePort $closed -NoBroadcast -TimeoutSec 2 -IntervalSec 1
Check 'no answer yields WakeNotConfirmed' ($seq.Verdict -eq 'WakeNotConfirmed' -and $seq.Wait.TimedOut)
Throws 'bad LanAddress rejected' { Invoke-ShellWakeSequence -Target 127.0.0.1 -LanAddress 'not-an-ip' } 'LanAddress'
$json = $seq | ConvertTo-Json -Depth 8
Check 'result serialises to JSON' ($json.Length -gt 100 -and ($json | ConvertFrom-Json).Verdict -eq 'WakeNotConfirmed')

Write-Host ''
Write-Host "Passed $script:Pass, failed $script:Fail"
if ($script:Fail -gt 0) { exit 1 }
