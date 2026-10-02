#requires -Version 5.1
# Connection probe, Wake-on-LAN and readiness wait for a Shell host (or another GameStream host).
# Works on Windows PowerShell 5.1 and PowerShell 7. Read-only except for the
# UDP magic packets that Send-ShellWake emits on request.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Same ports as the Hermit and Moonlight clients, so results are comparable
# with their "Wake PC" action.
$script:StaticWolPorts = @(9, 47009)
$script:DynamicWolPortOffsets = @(9, 10, 11, 13, 21)   # 47998, 47999, 48000, 48002, 48010 relative to 47989
$script:DefaultBasePort = 47989
$script:HttpsPortOffset = -5
$script:MaxServerInfoBytes = 65536

function ConvertTo-ShellMacBytes {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$MacAddress)
    $hex = ($MacAddress -replace '[:\-\.\s]', '').ToUpperInvariant()
    if ($hex -notmatch '^[0-9A-F]{12}$') { throw "MAC address must contain 12 hex digits: '$MacAddress'" }
    if ($hex -eq '000000000000') { throw 'MAC 00:00:00:00:00:00 is the Shell placeholder for unpaired requests and cannot wake anything.' }
    if ($hex -eq 'FFFFFFFFFFFF') { throw 'Broadcast MAC is not a host address.' }
    [byte[]]$bytes = for ($i = 0; $i -lt 12; $i += 2) { [Convert]::ToByte($hex.Substring($i, 2), 16) }
    if ($bytes[0] -band 1) { throw 'Multicast MAC is not a host address.' }
    return ,$bytes
}

function New-ShellMagicPacket {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$MacAddress)
    $mac = ConvertTo-ShellMacBytes $MacAddress
    $packet = [byte[]]::new(102)
    for ($i = 0; $i -lt 6; $i++) { $packet[$i] = 0xFF }
    for ($r = 0; $r -lt 16; $r++) { [Array]::Copy($mac, 0, $packet, 6 + $r * 6, 6) }
    return ,$packet
}

function Get-ShellAddressClass {
    # Classifies an IP literal so results can say which kind of path was exercised.
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Net.IPAddress]$Address)
    if ([System.Net.IPAddress]::IsLoopback($Address)) { return 'Loopback' }
    if ($Address.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork) {
        $b = $Address.GetAddressBytes()
        if ($b[0] -eq 255 -and $b[1] -eq 255 -and $b[2] -eq 255 -and $b[3] -eq 255) { return 'LimitedBroadcast' }
        if ($b[0] -eq 10) { return 'LAN' }
        if ($b[0] -eq 172 -and $b[1] -ge 16 -and $b[1] -le 31) { return 'LAN' }
        if ($b[0] -eq 192 -and $b[1] -eq 168) { return 'LAN' }
        if ($b[0] -eq 169 -and $b[1] -eq 254) { return 'LinkLocal' }
        if ($b[0] -eq 100 -and $b[1] -ge 64 -and $b[1] -le 127) { return 'TailscaleOrCgnat' }
        return 'Internet'
    }
    if ($Address.IsIPv6LinkLocal) { return 'LinkLocal' }
    $b = $Address.GetAddressBytes()
    if ($b[0] -eq 0xfd -and $b[1] -eq 0x7a -and $b[2] -eq 0x11 -and $b[3] -eq 0x5c -and $b[4] -eq 0xa1 -and $b[5] -eq 0xe0) { return 'Tailscale' }
    if (($b[0] -band 0xfe) -eq 0xfc) { return 'UniqueLocal' }
    return 'Internet'
}

function Get-ShellLocalNetwork {
    # IPv4 interfaces that are up, with their subnet broadcast address.
    [CmdletBinding()]
    param()
    $result = [System.Collections.Generic.List[object]]::new()
    foreach ($nic in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
        if ($nic.OperationalStatus -ne [System.Net.NetworkInformation.OperationalStatus]::Up) { continue }
        if ($nic.NetworkInterfaceType -eq [System.Net.NetworkInformation.NetworkInterfaceType]::Loopback) { continue }
        foreach ($ua in $nic.GetIPProperties().UnicastAddresses) {
            if ($ua.Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { continue }
            if ($null -eq $ua.IPv4Mask) { continue }
            $ip = $ua.Address.GetAddressBytes(); $mask = $ua.IPv4Mask.GetAddressBytes()
            if (($mask -join '.') -eq '0.0.0.0' -or ($mask -join '.') -eq '255.255.255.255') { continue }
            $bcast = [byte[]]::new(4); $net = [byte[]]::new(4)
            for ($i = 0; $i -lt 4; $i++) { $net[$i] = $ip[$i] -band $mask[$i]; $bcast[$i] = $net[$i] -bor (255 -bxor $mask[$i]) }
            $result.Add([pscustomobject]@{
                Interface = $nic.Name; Address = $ua.Address; Mask = $ua.IPv4Mask
                Network = [System.Net.IPAddress]::new($net); Broadcast = [System.Net.IPAddress]::new($bcast)
            })
        }
    }
    return $result.ToArray()
}

function Test-ShellSameSubnet {
    # $true when the local machine has an IPv4 interface on the same subnet as $Address.
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Net.IPAddress]$Address, [object[]]$LocalNetworks)
    if ($Address.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) { return $false }
    if ($null -eq $LocalNetworks) { $LocalNetworks = @(Get-ShellLocalNetwork) }
    $a = $Address.GetAddressBytes()
    foreach ($n in $LocalNetworks) {
        $mask = $n.Mask.GetAddressBytes(); $net = $n.Network.GetAddressBytes(); $match = $true
        for ($i = 0; $i -lt 4; $i++) { if (($a[$i] -band $mask[$i]) -ne $net[$i]) { $match = $false; break } }
        if ($match) { return $true }
    }
    return $false
}

function Resolve-ShellTarget {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Target)
    $literal = $null
    if ([System.Net.IPAddress]::TryParse($Target, [ref]$literal)) {
        return [pscustomobject]@{ Target = $Target; Addresses = @($literal); Resolved = $true; Error = $null }
    }
    if ($Target -notmatch '^[A-Za-z0-9]([A-Za-z0-9\-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9\-]*[A-Za-z0-9])?)*$') {
        return [pscustomobject]@{ Target = $Target; Addresses = @(); Resolved = $false; Error = 'Invalid host name.' }
    }
    try {
        $addrs = @([System.Net.Dns]::GetHostAddresses($Target))
        return [pscustomobject]@{ Target = $Target; Addresses = $addrs; Resolved = ($addrs.Count -gt 0); Error = $null }
    } catch {
        return [pscustomobject]@{ Target = $Target; Addresses = @(); Resolved = $false; Error = $_.Exception.Message }
    }
}

function Test-ShellTcpPort {
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Net.IPAddress]$Address, [Parameter(Mandatory)][int]$Port, [int]$TimeoutMs = 2000)
    $client = [System.Net.Sockets.TcpClient]::new($Address.AddressFamily)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $ar = $client.BeginConnect($Address, $Port, $null, $null)
        if (-not $ar.AsyncWaitHandle.WaitOne($TimeoutMs)) { return [pscustomobject]@{ Open = $false; Ms = $sw.ElapsedMilliseconds; Error = 'timeout' } }
        $client.EndConnect($ar)
        return [pscustomobject]@{ Open = $true; Ms = $sw.ElapsedMilliseconds; Error = $null }
    } catch {
        return [pscustomobject]@{ Open = $false; Ms = $sw.ElapsedMilliseconds; Error = $_.Exception.GetBaseException().Message }
    } finally { $client.Close() }
}

function ConvertFrom-ShellServerInfo {
    # Parses the unauthenticated /serverinfo XML. DTDs and external resolution are disabled.
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Xml)
    $settings = [System.Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.MaxCharactersFromEntities = 0
    $reader = [System.Xml.XmlReader]::Create([System.IO.StringReader]::new($Xml), $settings)
    try {
        $doc = [System.Xml.XmlDocument]::new(); $doc.XmlResolver = $null; $doc.Load($reader)
    } finally { $reader.Dispose() }
    $root = $doc.DocumentElement
    if ($null -eq $root -or $root.LocalName -ne 'root') { throw 'Response is not a GameStream serverinfo document.' }
    $pick = { param($name) $n = $root.SelectSingleNode($name); if ($n) { $n.InnerText } else { $null } }
    [pscustomobject]@{
        StatusCode = $(if ($root.HasAttribute('status_code')) { [int]$root.GetAttribute('status_code') } else { $null })
        Hostname = & $pick 'hostname'; AppVersion = & $pick 'appversion'; UniqueId = & $pick 'uniqueid'
        State = & $pick 'state'; HttpsPort = & $pick 'HttpsPort'; PairStatus = & $pick 'PairStatus'
        CurrentGame = & $pick 'currentgame'
    }
}

function Get-ShellServerInfo {
    [CmdletBinding()]
    param([Parameter(Mandatory)][System.Net.IPAddress]$Address, [int]$Port = $script:DefaultBasePort, [int]$TimeoutMs = 2000)
    $hostPart = if ($Address.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) { "[$Address]" } else { "$Address" }
    $request = [System.Net.HttpWebRequest]::Create("http://$hostPart`:$Port/serverinfo")
    $request.Method = 'GET'; $request.Timeout = $TimeoutMs; $request.ReadWriteTimeout = $TimeoutMs
    $request.KeepAlive = $false; $request.AllowAutoRedirect = $false; $request.UserAgent = 'Shell-probe'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $response = $null
    try {
        $response = $request.GetResponse()
        $stream = $response.GetResponseStream()
        try {
            $buffer = [byte[]]::new(4096); $ms = [System.IO.MemoryStream]::new()
            while (($n = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $ms.Write($buffer, 0, $n)
                if ($ms.Length -gt $script:MaxServerInfoBytes) { throw 'serverinfo response is too large.' }
            }
            $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
        } finally { $stream.Dispose() }
        $info = ConvertFrom-ShellServerInfo $text
        [pscustomobject]@{ Ok = ($info.StatusCode -eq 200); Ms = $sw.ElapsedMilliseconds; Info = $info; Error = $null }
    } catch {
        [pscustomobject]@{ Ok = $false; Ms = $sw.ElapsedMilliseconds; Info = $null; Error = $_.Exception.GetBaseException().Message }
    } finally { if ($response) { $response.Close() } }
}

function Test-ShellHost {
    <#
    .SYNOPSIS
    Probes a Shell host over the unauthenticated HTTP serverinfo endpoint and the HTTPS port.
    .DESCRIPTION
    Resolves the target, classifies each address (LAN, Internet, Tailscale...), checks TCP on the
    HTTP and HTTPS ports and parses /serverinfo. Nothing is written to the host. The MAC address is
    never reported because the HTTP endpoint only returns a placeholder anyway.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Target,
        [int]$BasePort = $script:DefaultBasePort,
        [int]$TimeoutMs = 2000,
        [string]$ExpectedUniqueId
    )
    if ($BasePort -lt 1024 -or $BasePort -gt 65530) { throw 'BasePort must be between 1024 and 65530.' }
    $localNets = @(Get-ShellLocalNetwork)
    $resolved = Resolve-ShellTarget $Target
    $attempts = [System.Collections.Generic.List[object]]::new()
    $reachable = $false; $matched = $null; $used = $null; $info = $null
    foreach ($addr in $resolved.Addresses) {
        $class = Get-ShellAddressClass $addr
        $http = Test-ShellTcpPort -Address $addr -Port $BasePort -TimeoutMs $TimeoutMs
        $https = Test-ShellTcpPort -Address $addr -Port ($BasePort + $script:HttpsPortOffset) -TimeoutMs $TimeoutMs
        $si = if ($http.Open) { Get-ShellServerInfo -Address $addr -Port $BasePort -TimeoutMs $TimeoutMs } else { $null }
        $idOk = $null
        if ($si -and $si.Ok -and $ExpectedUniqueId) { $idOk = ($si.Info.UniqueId -eq $ExpectedUniqueId) }
        $attempts.Add([pscustomobject]@{
            Address = $addr.ToString(); Class = $class; SameSubnet = (Test-ShellSameSubnet -Address $addr -LocalNetworks $localNets)
            HttpPortOpen = $http.Open; HttpsPortOpen = $https.Open; ConnectMs = $http.Ms
            ServerInfoOk = $(if ($si) { $si.Ok } else { $false }); ServerInfoMs = $(if ($si) { $si.Ms } else { $null })
            UniqueIdMatches = $idOk
            Error = $(if ($si -and $si.Error) { $si.Error } elseif ($http.Error) { $http.Error } else { $null })
        })
        if (-not $reachable -and $si -and $si.Ok -and ($idOk -ne $false)) {
            $reachable = $true; $used = $addr; $info = $si.Info; $matched = $idOk
        }
    }
    [pscustomobject]@{
        Target = $Target; Resolved = $resolved.Resolved; ResolveError = $resolved.Error
        CheckedUtc = [DateTime]::UtcNow.ToString('o')
        Reachable = $reachable
        AddressUsed = $(if ($used) { $used.ToString() } else { $null })
        PathClass = $(if ($used) { Get-ShellAddressClass $used } else { $null })
        SenderOnSameSubnet = $(if ($used) { Test-ShellSameSubnet -Address $used -LocalNetworks $localNets } else { $null })
        Hostname = $(if ($info) { $info.Hostname } else { $null })
        AppVersion = $(if ($info) { $info.AppVersion } else { $null })
        UniqueId = $(if ($info) { $info.UniqueId } else { $null })
        State = $(if ($info) { $info.State } else { $null })
        UniqueIdMatches = $matched
        Attempts = @($attempts.ToArray())
        Note = 'HTTP serverinfo succeeding means the Shell service answered; it does not test the UDP streaming ports.'
    }
}

function Send-ShellWake {
    <#
    .SYNOPSIS
    Sends Wake-on-LAN magic packets the same way the Hermit and Moonlight clients do.
    .DESCRIPTION
    Targets are each resolved address of -Target plus, unless -NoBroadcast, the limited broadcast
    255.255.255.255 and every local IPv4 subnet broadcast. Ports are 9, 47009 and the five
    GameStream ports relative to -BasePort. A "sent" datagram only means the local socket accepted
    it; delivery through a router is never implied.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$MacAddress,
        [string[]]$Target = @(),
        [int]$BasePort = $script:DefaultBasePort,
        [int[]]$Port,
        [switch]$NoBroadcast
    )
    $packet = New-ShellMagicPacket $MacAddress
    $ports = if ($Port) { @($Port) } else { @($script:StaticWolPorts) + @($script:DynamicWolPortOffsets | ForEach-Object { $BasePort + $_ }) }
    foreach ($p in $ports) { if ($p -lt 1 -or $p -gt 65535) { throw "Invalid UDP port $p" } }
    $targets = [System.Collections.Generic.List[object]]::new()
    foreach ($t in $Target) {
        $r = Resolve-ShellTarget $t
        if (-not $r.Resolved) { $targets.Add([pscustomobject]@{ Name = $t; Address = $null; Kind = 'Unresolved'; Class = $null; Error = $r.Error }); continue }
        foreach ($a in $r.Addresses) { $targets.Add([pscustomobject]@{ Name = $t; Address = $a; Kind = 'Directed'; Class = (Get-ShellAddressClass $a); Error = $null }) }
    }
    if (-not $NoBroadcast) {
        $targets.Add([pscustomobject]@{ Name = '255.255.255.255'; Address = [System.Net.IPAddress]::Broadcast; Kind = 'LimitedBroadcast'; Class = 'LimitedBroadcast'; Error = $null })
        foreach ($n in Get-ShellLocalNetwork) {
            $targets.Add([pscustomobject]@{ Name = "$($n.Interface) $($n.Broadcast)"; Address = $n.Broadcast; Kind = 'SubnetBroadcast'; Class = 'LAN'; Error = $null })
        }
    }
    $deliveries = [System.Collections.Generic.List[object]]::new()
    foreach ($t in $targets) {
        if ($t.Kind -eq 'Unresolved') {
            $deliveries.Add([pscustomobject]@{ Target = $t.Name; Address = $null; Port = $null; Kind = $t.Kind; Class = $null; Sent = $false; Error = $t.Error }); continue
        }
        $sock = [System.Net.Sockets.UdpClient]::new($t.Address.AddressFamily)
        try {
            if ($t.Kind -ne 'Directed') { $sock.EnableBroadcast = $true }
            foreach ($p in $ports) {
                $sent = $false; $err = $null
                try { $sent = ($sock.Send($packet, $packet.Length, [System.Net.IPEndPoint]::new($t.Address, $p)) -eq $packet.Length) }
                catch { $err = $_.Exception.GetBaseException().Message }
                $deliveries.Add([pscustomobject]@{ Target = $t.Name; Address = $t.Address.ToString(); Port = $p; Kind = $t.Kind; Class = $t.Class; Sent = $sent; Error = $err })
            }
        } finally { $sock.Close() }
    }
    $sentAny = @($deliveries | Where-Object Sent).Count -gt 0
    [pscustomobject]@{
        SentUtc = [DateTime]::UtcNow.ToString('o'); PacketBytes = $packet.Length; AnySent = $sentAny
        BroadcastUsed = (-not $NoBroadcast)
        DirectedClasses = @($deliveries | Where-Object { $_.Kind -eq 'Directed' -and $_.Sent } | Select-Object -ExpandProperty Class -Unique)
        Deliveries = @($deliveries.ToArray())
        Note = 'Sent means the local socket accepted the datagram. Nothing here proves the packet crossed a router or reached the NIC.'
    }
}

function Wait-ShellReady {
    <#
    .SYNOPSIS
    Polls Test-ShellHost until the host answers serverinfo or the timeout elapses.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Target,
        [int]$BasePort = $script:DefaultBasePort,
        [int]$TimeoutSec = 180,
        [int]$IntervalSec = 3,
        [int]$ProbeTimeoutMs = 2000,
        [string]$ExpectedUniqueId,
        [int]$ConfirmCount = 2
    )
    if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 3600) { throw 'TimeoutSec must be between 1 and 3600.' }
    if ($IntervalSec -lt 1) { throw 'IntervalSec must be at least 1.' }
    if ($ConfirmCount -lt 1) { throw 'ConfirmCount must be at least 1.' }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $attempts = 0; $consecutive = 0; $firstSeenMs = $null; $last = $null
    while ($true) {
        $attempts++
        $last = Test-ShellHost -Target $Target -BasePort $BasePort -TimeoutMs $ProbeTimeoutMs -ExpectedUniqueId $ExpectedUniqueId
        if ($last.Reachable) {
            $consecutive++
            if ($null -eq $firstSeenMs) { $firstSeenMs = $sw.ElapsedMilliseconds }
            if ($consecutive -ge $ConfirmCount) { break }
        } else { $consecutive = 0; $firstSeenMs = $null }
        if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) { break }
        $remaining = $TimeoutSec - $sw.Elapsed.TotalSeconds
        Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min($IntervalSec * 1000, $remaining * 1000)))
    }
    [pscustomobject]@{
        Target = $Target; Ready = ($consecutive -ge $ConfirmCount); Attempts = $attempts
        ElapsedSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        FirstResponseSec = $(if ($null -ne $firstSeenMs) { [Math]::Round($firstSeenMs / 1000.0, 1) } else { $null })
        TimedOut = ($consecutive -lt $ConfirmCount)
        LastProbe = $last
    }
}

function Get-ShellWakeVerdict {
    # Pure decision function so the labelling rules can be unit tested.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][bool]$InitiallyOnline,
        [Parameter(Mandatory)][bool]$MacProvided,
        [Parameter(Mandatory)][bool]$AnyPacketSent,
        [Parameter(Mandatory)][bool]$BroadcastUsed,
        [string[]]$DirectedClasses = @(),
        [AllowNull()][object]$SenderOnHostLan,     # $true / $false / $null when unknown
        [Parameter(Mandatory)][bool]$Ready
    )
    if ($InitiallyOnline) { return 'AlreadyOnline' }
    if (-not $MacProvided) { return 'NoMacAddress' }
    if (-not $AnyPacketSent) { return 'NoPacketSent' }
    if (-not $Ready) { return 'WakeNotConfirmed' }
    $lanEvidence = $BroadcastUsed -or ($SenderOnHostLan -eq $true) -or ($DirectedClasses -contains 'LAN') -or ($DirectedClasses -contains 'LinkLocal')
    if ($lanEvidence) { return 'WokeOnLan' }
    if ($null -eq $SenderOnHostLan) { return 'WokeUnknownPath' }
    $remoteOnly = @($DirectedClasses | Where-Object { $_ -in @('Internet', 'TailscaleOrCgnat', 'Tailscale', 'UniqueLocal') })
    if ($remoteOnly.Count -gt 0 -and $remoteOnly.Count -eq @($DirectedClasses).Count) { return 'WokeViaRemotePath' }
    return 'WokeUnknownPath'
}

$script:VerdictText = @{
    AlreadyOnline     = 'Host answered before any wake packet was sent. Nothing about Wake-on-LAN was tested.'
    NoMacAddress      = 'No MAC address was supplied, so no wake packet could be built.'
    NoPacketSent      = 'No datagram left this machine; check the target list and local network.'
    WakeNotConfirmed  = 'Host did not answer within the timeout. The packet may not have arrived, the PC may not support waking from this power state, or boot took longer than the timeout.'
    WokeOnLan         = 'Host came up after a wake sent with LAN broadcast or from the same subnet. This is not evidence that waking from outside the local network works.'
    WokeUnknownPath   = 'Host came up, but the sender position relative to the host LAN is unknown (pass -LanAddress). If this ran from inside the host network, a public address only proves NAT loopback.'
    WokeViaRemotePath = 'Host came up after directed packets to non-LAN addresses only, sent from outside the host subnet.'
}

function Invoke-ShellWakeSequence {
    <#
    .SYNOPSIS
    Probe, wake if needed, wait for Shell, and label what the result shows.
    .PARAMETER LanAddress
    The host's LAN IPv4 address. Used only to decide whether this machine sits on the same subnet,
    which caps the verdict at WokeOnLan.
    .PARAMETER Scenario
    Free label recorded with the result so tests right after shutdown and after hours of downtime
    can be told apart.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string[]]$Target,
        [string]$MacAddress,
        [int]$BasePort = $script:DefaultBasePort,
        [string]$LanAddress,
        [ValidateSet('Unspecified', 'JustShutDown', 'LongOffline', 'Sleep', 'Hibernate')][string]$Scenario = 'Unspecified',
        [switch]$NoBroadcast,
        [int]$TimeoutSec = 180,
        [int]$IntervalSec = 3,
        [string]$ExpectedUniqueId
    )
    $startedUtc = [DateTime]::UtcNow
    $senderOnLan = $null
    if ($LanAddress) {
        $lanIp = $null
        if (-not [System.Net.IPAddress]::TryParse($LanAddress, [ref]$lanIp)) { throw 'LanAddress must be an IP literal.' }
        $senderOnLan = Test-ShellSameSubnet -Address $lanIp
    }
    $initial = $null
    foreach ($t in $Target) {
        $initial = Test-ShellHost -Target $t -BasePort $BasePort -ExpectedUniqueId $ExpectedUniqueId
        if ($initial.Reachable) { break }
    }
    $wake = $null; $wait = $null
    if (-not $initial.Reachable) {
        if ($MacAddress) {
            $wake = Send-ShellWake -MacAddress $MacAddress -Target $Target -BasePort $BasePort -NoBroadcast:$NoBroadcast
            if ($wake.AnySent) {
                $wait = Wait-ShellReady -Target $Target[0] -BasePort $BasePort -TimeoutSec $TimeoutSec -IntervalSec $IntervalSec -ExpectedUniqueId $ExpectedUniqueId
            }
        }
    }
    $verdict = Get-ShellWakeVerdict -InitiallyOnline $initial.Reachable -MacProvided ([bool]$MacAddress) `
        -AnyPacketSent ($(if ($wake) { $wake.AnySent } else { $false })) -BroadcastUsed ($(if ($wake) { $wake.BroadcastUsed } else { $false })) `
        -DirectedClasses ($(if ($wake) { @($wake.DirectedClasses) } else { @() })) -SenderOnHostLan $senderOnLan `
        -Ready ($(if ($wait) { $wait.Ready } else { $false }))
    [pscustomobject]@{
        SchemaVersion = 1; StartedUtc = $startedUtc.ToString('o'); Scenario = $Scenario
        Targets = @($Target); BasePort = $BasePort; SenderOnHostLan = $senderOnLan
        Verdict = $verdict; VerdictText = $script:VerdictText[$verdict]
        InitialProbe = $initial; Wake = $wake; Wait = $wait
        Note = 'Internet wake is only demonstrated by WokeViaRemotePath from a device outside the local network, repeated for JustShutDown and LongOffline.'
    }
}

function Get-ShellWakeReadiness {
    <#
    .SYNOPSIS
    Read-only check, run on the host PC, of the settings that commonly stop Wake-on-LAN from working.
    #>
    [CmdletBinding()]
    param()
    $issues = [System.Collections.Generic.List[string]]::new()
    $adapters = @()
    # Driver registry keywords are locale independent; DisplayValue is localized and only kept for reading.
    $keywords = @('*WakeOnMagicPacket', '*WakeOnPattern', 'S5WakeOnLan', '*ModernStandbyWoLMagicPacket', 'WolShutdownLinkSpeed', 'EnableGreenEthernet', '*EEE', 'PowerSavingMode')
    try {
        $adapters = @(Get-NetAdapter -Physical -ErrorAction Stop | Where-Object Status -eq 'Up' | ForEach-Object {
            $pm = $null; $pmError = $null
            try { $pm = Get-NetAdapterPowerManagement -Name $_.Name -ErrorAction Stop } catch { $pmError = $_.Exception.Message }
            $adv = [ordered]@{}
            try {
                foreach ($p in Get-NetAdapterAdvancedProperty -Name $_.Name -ErrorAction Stop) {
                    if ($p.RegistryKeyword -in $keywords) {
                        $adv[$p.RegistryKeyword] = [pscustomobject]@{ DisplayName = $p.DisplayName; Value = [string]($p.RegistryValue -join ','); DisplayValue = $p.DisplayValue }
                    }
                }
            } catch { }
            [pscustomobject]@{
                Name = $_.Name; Description = $_.InterfaceDescription; LinkSpeed = $_.LinkSpeed
                WakeOnMagicPacket = $(if ($pm) { [string]$pm.WakeOnMagicPacket } else { 'Unknown' })
                DeviceSleepOnDisconnect = $(if ($pm) { [string]$pm.DeviceSleepOnDisconnect } else { 'Unknown' })
                PowerManagementError = $pmError
                DriverSettings = $adv
            }
        })
    } catch { $issues.Add("Adapter query failed: $($_.Exception.Message)") }
    foreach ($a in $adapters) {
        $s = $a.DriverSettings
        if ($s.Contains('*WakeOnMagicPacket')) {
            if ($s['*WakeOnMagicPacket'].Value -ne '1') { $issues.Add("Adapter '$($a.Name)': driver setting *WakeOnMagicPacket is off.") }
        } elseif ($a.WakeOnMagicPacket -ne 'Enabled') { $issues.Add("Adapter '$($a.Name)': magic packet wake state unknown ($($a.WakeOnMagicPacket)).") }
        if ($s.Contains('S5WakeOnLan')) {
            if ($s['S5WakeOnLan'].Value -ne '1') { $issues.Add("Adapter '$($a.Name)': S5WakeOnLan is off; waking from full shutdown will not work.") }
        } else { $issues.Add("Adapter '$($a.Name)': no S5WakeOnLan keyword; shutdown wake support is unknown.") }
    }
    $fastStartup = $null
    try {
        $fastStartup = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -ErrorAction Stop).HiberbootEnabled
        if ($fastStartup -eq 1) { $issues.Add('Windows Fast Startup is enabled. Shutdown then becomes a hibernate variant on which many NICs do not wake; test JustShutDown and LongOffline separately.') }
    } catch { }
    $wolCapable = @()
    try { $wolCapable = @(& powercfg /devicequery wake_programmable 2>$null) } catch { }
    [pscustomobject]@{
        CheckedUtc = [DateTime]::UtcNow.ToString('o'); Adapters = $adapters
        FastStartupEnabled = $(if ($null -ne $fastStartup) { [bool]$fastStartup } else { $null })
        WakeProgrammableDevices = @($wolCapable | Where-Object { $_ -match 'Ethernet|Realtek|Intel|Killer|LAN' })
        Issues = @($issues.ToArray())
        Unverified = @('BIOS/UEFI wake settings', 'Router forwarding of UDP wake packets', 'Behaviour after long power-off')
    }
}

Export-ModuleMember -Function Test-ShellHost, Send-ShellWake, Wait-ShellReady, Invoke-ShellWakeSequence,
    Get-ShellWakeReadiness, Get-ShellWakeVerdict, Get-ShellAddressClass, New-ShellMagicPacket,
    ConvertFrom-ShellServerInfo, Get-ShellLocalNetwork, Test-ShellSameSubnet, Resolve-ShellTarget
