#requires -Version 5.1
<#
.SYNOPSIS
Extracts the "Global video stats" blocks that Hermit (and moonlight-qt) writes at the end of each
streaming session into objects, so runs can be compared before and after a change.
.DESCRIPTION
Hermit keeps its logs as Hermit-<epoch>.log in the temp directory of the client PC; moonlight-qt
uses Moonlight-<epoch>.log. Both are read.
Each session ends with a block such as:

  Global video stats
  ------------------
  Video stream: 2560x1440 59.98 FPS (Codec: HEVC)
  Incoming frame rate from network: 59.98 FPS
  ...
  Average rendering time (including monitor V-sync latency): 3.10 ms

Only these blocks are read; nothing else in the log is reported. Works on Windows PowerShell 5.1.
.PARAMETER Path
A log file, a directory containing Hermit-*.log or Moonlight-*.log files, or omitted to use the current user's
temp directory.
.PARAMETER Label
Free text stored with every record (for example "baseline" or "preset-p4").
.PARAMETER OutputPath
Optional new JSON file to write the records to. Existing files are never overwritten.
.EXAMPLE
./shell/Get-HermitStats.ps1 -Label baseline -OutputPath stats-baseline.json
#>
[CmdletBinding()]
param(
    [string]$Path,
    [string]$Label = '',
    [string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertFrom-HermitStatsText {
    # Parses one log's text and returns one object per stats block.
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Text, [string]$LogFile = '', [string]$Label = '')
    $records = [System.Collections.Generic.List[object]]::new()
    $lines = $Text -split "`r?`n"
    # The log name carries the start time as a Unix timestamp (Hermit-<epoch>.log); line
    # prefixes are elapsed hh:mm:ss, so each block can be given a wall-clock time.
    $logStart = $null
    if ($LogFile -match '(?:Hermit|Moonlight)-(\d{9,10})\.log$') { $logStart = [DateTimeOffset]::FromUnixTimeSeconds([long]$matches[1]).ToLocalTime() }
    # Per-block context: a log holds one block per decoder lifetime (each session start and each
    # window resize), so bitrate, packet size and loss counts are tracked as the scan proceeds.
    $curBitrate = $null; $curPacket = $null
    $blockUnrecoverable = 0; $blockNetworkDrops = 0; $blockIdr = 0; $blockMissing = [System.Collections.Generic.List[int]]::new()
    $i = 0
    while ($i -lt $lines.Count) {
        $line = $lines[$i]
        if ($line -match 'Video bitrate:\s*(\d+)\s*kbps') { $curBitrate = [int]$matches[1] }
        elseif ($line -match 'Packet size capped at (\d+) bytes') { $curPacket = [int]$matches[1] }
        elseif ($line -match 'Using custom packet size:\s*(\d+)') { $curPacket = [int]$matches[1] }
        elseif ($line -match 'Using (\d+) byte video packets for remote streaming') { $curPacket = [int]$matches[1] }
        elseif ($line -match 'Unrecoverable frame \d+: \d+\+\d+=(\d+) received < (\d+) needed') { $blockUnrecoverable++; $blockMissing.Add([int]$matches[2] - [int]$matches[1]) }
        elseif ($line -match 'Network dropped \d+ frame') { $blockNetworkDrops++ }
        elseif ($line -match 'IDR frame request sent') { $blockIdr++ }
        # Real logs prefix both the title and the dashed line with "hh:mm:ss - SDL Info (0): ".
        if ($lines[$i] -match '(Global video stats|Video stats)\s*$' -and ($i + 1) -lt $lines.Count -and $lines[$i + 1] -match '-{5,}\s*$') {
            $title = if ($lines[$i] -match 'Global video stats') { 'Global video stats' } else { 'Video stats' }
            # The SDL message starts with a newline, so the "hh:mm:ss - SDL Info (0): " prefix
            # usually sits on the line before the title; older builds put it on the same line.
            $elapsed = $null; $wall = $null
            $stamped = if ($lines[$i] -match '^(\d\d):(\d\d):(\d\d) - ') { $lines[$i] } elseif ($i -gt 0 -and $lines[$i - 1] -match '^(\d\d):(\d\d):(\d\d) - \S+ \S+ \(\d+\):\s*$') { $lines[$i - 1] } else { $null }
            if ($stamped -and $stamped -match '^(\d\d):(\d\d):(\d\d) - ') {
                $elapsed = [TimeSpan]::new([int]$matches[1], [int]$matches[2], [int]$matches[3])
                if ($logStart) { $wall = $logStart.Add($elapsed).ToString('yyyy-MM-dd HH:mm:ss') }
            }
            $sortedMissing = @($blockMissing | Sort-Object)
            $rec = [ordered]@{
                LogFile = $LogFile; Label = $Label; Block = $title; BlockLine = $i + 1
                EndTime = $wall; Elapsed = $(if ($elapsed) { $elapsed.ToString() } else { $null })
                BitrateKbps = $curBitrate; PacketSize = $curPacket
                BlockUnrecoverableFrames = $blockUnrecoverable; BlockNetworkDropEvents = $blockNetworkDrops; BlockIdrRequests = $blockIdr
                BlockMissingShardsP90 = $(if ($sortedMissing.Count) { $sortedMissing[[Math]::Min($sortedMissing.Count - 1, [int][Math]::Floor($sortedMissing.Count * 0.9))] } else { $null })
                Width = $null; Height = $null; TotalFps = $null; Codec = $null
                IncomingFps = $null; DecodedFps = $null; RenderedFps = $null
                HostLatencyMinMs = $null; HostLatencyMaxMs = $null; HostLatencyAvgMs = $null
                NetworkDroppedPct = $null; JitterDroppedPct = $null
                RttMs = $null; RttVarianceMs = $null
                DecodeMs = $null; QueueMs = $null; RenderMs = $null
                AvgBitrateMbps = $null; PeakBitrateMbps = $null
            }
            $j = $i + 2
            while ($j -lt $lines.Count) {
                $l = $lines[$j]
                if ($l -match 'Video stream:\s*(\d+)x(\d+)\s+([\d.]+)\s+FPS\s+\(Codec:\s*([^)]+)\)') {
                    $rec.Width = [int]$matches[1]; $rec.Height = [int]$matches[2]; $rec.TotalFps = [double]$matches[3]; $rec.Codec = $matches[4].Trim()
                } elseif ($l -match 'Bitrate:\s*([\d.]+)\s*Mbps,\s*Peak\s*\(\d+s\):\s*([\d.]+)') {
                    $rec.AvgBitrateMbps = [double]$matches[1]; $rec.PeakBitrateMbps = [double]$matches[2]
                } elseif ($l -match 'Incoming frame rate from network:\s*([\d.]+)') { $rec.IncomingFps = [double]$matches[1] }
                elseif ($l -match 'Decoding frame rate:\s*([\d.]+)') { $rec.DecodedFps = [double]$matches[1] }
                elseif ($l -match 'Rendering frame rate:\s*([\d.]+)') { $rec.RenderedFps = [double]$matches[1] }
                elseif ($l -match 'Host processing latency min/max/average:\s*([\d.]+)/([\d.]+)/([\d.]+)') {
                    $rec.HostLatencyMinMs = [double]$matches[1]; $rec.HostLatencyMaxMs = [double]$matches[2]; $rec.HostLatencyAvgMs = [double]$matches[3]
                } elseif ($l -match 'Frames dropped by your network connection:\s*([\d.]+)%') { $rec.NetworkDroppedPct = [double]$matches[1] }
                elseif ($l -match 'Frames dropped due to network jitter:\s*([\d.]+)%') { $rec.JitterDroppedPct = [double]$matches[1] }
                elseif ($l -match 'Average network latency:\s*(\d+)\s*ms\s*\(variance:\s*(\d+)\s*ms\)') { $rec.RttMs = [int]$matches[1]; $rec.RttVarianceMs = [int]$matches[2] }
                elseif ($l -match 'Average network latency:\s*N/A') { }
                elseif ($l -match 'Average decoding time:\s*([\d.]+)') { $rec.DecodeMs = [double]$matches[1] }
                elseif ($l -match 'Average frame queue delay:\s*([\d.]+)') { $rec.QueueMs = [double]$matches[1] }
                elseif ($l -match 'Average rendering time.*?:\s*([\d.]+)\s*ms') { $rec.RenderMs = [double]$matches[1]; $j++; break }
                elseif ($l -match '^\s*$' -or $l -match '(Global video stats|Video stats)\s*$') { break }
                $j++
            }
            $records.Add([pscustomobject]$rec)
            # Loss counters restart for the next decoder lifetime.
            $blockUnrecoverable = 0; $blockNetworkDrops = 0; $blockIdr = 0; $blockMissing.Clear()
            $i = $j
            continue
        }
        $i++
    }

    # Log-wide loss details, attached to every record of this log. A frame is unrecoverable when
    # fewer shards arrived than its data shards; "missing" is how many more packets were needed
    # than FEC could supply, which separates random loss (small) from burst loss (large).
    $missing = [System.Collections.Generic.List[int]]::new()
    foreach ($m in [regex]::Matches($Text, 'Unrecoverable frame \d+: \d+\+\d+=(\d+) received < (\d+) needed')) {
        $missing.Add([int]$m.Groups[2].Value - [int]$m.Groups[1].Value)
    }
    $sorted = @($missing | Sort-Object)
    $percentile = { param($p) if ($sorted.Count -eq 0) { $null } else { $sorted[[Math]::Min($sorted.Count - 1, [int][Math]::Floor($sorted.Count * $p))] } }
    $bitrate = [regex]::Match($Text, 'Video bitrate:\s*(\d+)\s*kbps')
    # PacketSize is already recorded per block above (the value in force when the block ended).
    $logWide = [ordered]@{
        VideoBitrateKbps = $(if ($bitrate.Success) { [int]$bitrate.Groups[1].Value } else { $null })
        UnrecoverableFrames = $missing.Count
        MissingShardsMedian = & $percentile 0.5
        MissingShardsP90 = & $percentile 0.9
        IdrRequests = ([regex]::Matches($Text, 'IDR frame request sent')).Count
    }
    foreach ($r in $records) {
        foreach ($k in $logWide.Keys) { $r | Add-Member -NotePropertyName $k -NotePropertyValue $logWide[$k] }
    }
    return $records.ToArray()
}

function Get-HermitLogFiles([string]$Path) {
    if (-not $Path) { $Path = $env:TEMP }
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return @(Get-Item -LiteralPath $Path) }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        return @(Get-ChildItem -LiteralPath $Path -File | Where-Object { $_.Name -match '^(Hermit|Moonlight)-\d+\.log$' } | Sort-Object LastWriteTime)
    }
    throw "Path not found: $Path"
}

if ($MyInvocation.InvocationName -ne '.') {
    $all = [System.Collections.Generic.List[object]]::new()
    foreach ($file in Get-HermitLogFiles $Path) {
        if ($file.Length -gt 64MB) { Write-Warning "Skipping oversized log $($file.Name)"; continue }
        $text = [IO.File]::ReadAllText($file.FullName)
        foreach ($r in ConvertFrom-HermitStatsText -Text $text -LogFile $file.Name -Label $Label) { $all.Add($r) }
    }
    if ($OutputPath) {
        # Serialise each record separately and join, so the file is always a JSON array on 5.1
        # regardless of how many records there are.
        $parts = foreach ($rec in $all) { ConvertTo-Json -InputObject $rec -Depth 4 }
        $json = '[' + (@($parts) -join ",`n") + ']'
        $full = [IO.Path]::GetFullPath($OutputPath)
        $stream = [IO.File]::Open($full, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $bytes = [Text.Encoding]::UTF8.GetBytes($json); $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    }
    $all.ToArray()
}
