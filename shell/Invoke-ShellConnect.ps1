#requires -Version 5.1
<#
.SYNOPSIS
Entry point for connection probing, Wake-on-LAN and readiness waiting.
.EXAMPLE
./shell/Invoke-ShellConnect.ps1 -Action Probe -Target host.example.com
.EXAMPLE
./shell/Invoke-ShellConnect.ps1 -Action Sequence -Target host.example.com -MacAddress AA:BB:CC:DD:EE:FF -LanAddress 192.168.0.10 -Scenario LongOffline -NoBroadcast -OutputPath wake-longoffline.json
#>
[CmdletBinding()]
param(
    [ValidateSet('Probe', 'Wake', 'Wait', 'Sequence', 'Readiness')][string]$Action = 'Probe',
    [string[]]$Target,
    [string]$MacAddress,
    [int]$BasePort = 47989,
    [string]$LanAddress,
    [ValidateSet('Unspecified', 'JustShutDown', 'LongOffline', 'Sleep', 'Hibernate')][string]$Scenario = 'Unspecified',
    [switch]$NoBroadcast,
    [int]$TimeoutSec = 180,
    [int]$IntervalSec = 3,
    [string]$ExpectedUniqueId,
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ShellConnect.psm1') -Force

function Require-Target { if (-not $Target -or $Target.Count -eq 0) { throw "-Target is required for action $Action." } }

$result = switch ($Action) {
    'Probe' {
        Require-Target
        foreach ($t in $Target) { Test-ShellHost -Target $t -BasePort $BasePort -ExpectedUniqueId $ExpectedUniqueId }
    }
    'Wake' {
        if (-not $MacAddress) { throw '-MacAddress is required for Wake.' }
        Send-ShellWake -MacAddress $MacAddress -Target @($Target) -BasePort $BasePort -NoBroadcast:$NoBroadcast
    }
    'Wait' {
        Require-Target
        Wait-ShellReady -Target $Target[0] -BasePort $BasePort -TimeoutSec $TimeoutSec -IntervalSec $IntervalSec -ExpectedUniqueId $ExpectedUniqueId
    }
    'Sequence' {
        Require-Target
        Invoke-ShellWakeSequence -Target $Target -MacAddress $MacAddress -BasePort $BasePort -LanAddress $LanAddress -Scenario $Scenario `
            -NoBroadcast:$NoBroadcast -TimeoutSec $TimeoutSec -IntervalSec $IntervalSec -ExpectedUniqueId $ExpectedUniqueId
    }
    'Readiness' { Get-ShellWakeReadiness }
}

$json = $result | ConvertTo-Json -Depth 8
if ($OutputPath) {
    $full = [IO.Path]::GetFullPath($OutputPath)
    $stream = [IO.File]::Open($full, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $bytes = [Text.Encoding]::UTF8.GetBytes($json); $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}
$json
