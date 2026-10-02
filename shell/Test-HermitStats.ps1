#requires -Version 5.1
# Checks Get-HermitStats.ps1 against a synthetic moonlight-qt log.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Get-HermitStats.ps1')

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [bool]$Condition, [string]$Detail = '') {
    if ($Condition) { $script:Pass++; Write-Host "  ok   $Name" }
    else { $script:Fail++; Write-Host "  FAIL $Name $Detail" -ForegroundColor Red }
}

$log = @'
00:00:01 - SDL Info (0): Moonlight Qt v6.1.0
00:00:05 - SDL Info (0): Initialized HEVC decoder
00:31:22 - SDL Info (0):
Global video stats
------------------
Video stream: 2560x1440 59.98 FPS (Codec: HEVC)
Incoming frame rate from network: 59.98 FPS
Decoding frame rate: 59.98 FPS
Rendering frame rate: 59.97 FPS
Host processing latency min/max/average: 1.2/8.5/2.3 ms
Frames dropped by your network connection: 0.05%
Frames dropped due to network jitter: 0.10%
Average network latency: 25 ms (variance: 3 ms)
Average decoding time: 1.50 ms
Average frame queue delay: 4.20 ms
Average rendering time (including monitor V-sync latency): 3.10 ms
00:31:23 - SDL Info (0): Session ended
00:40:00 - SDL Info (0):
Global video stats
------------------
Video stream: 1920x1080 100.01 FPS (Codec: H.264)
Bitrate: 38.2 Mbps, Peak (5s): 55.7
Incoming frame rate from network: 100.01 FPS
Decoding frame rate: 100.00 FPS
Rendering frame rate: 99.90 FPS
Frames dropped by your network connection: 1.25%
Frames dropped due to network jitter: 0.00%
Average network latency: N/A
Average decoding time: 0.90 ms
Average frame queue delay: 2.00 ms
Average rendering time (including monitor V-sync latency): 1.00 ms
00:41:00 - SDL Info (0):
Video stats
------------------
Video stream: 1280x720 30.00 FPS (Codec: HEVC 10-bit SDR)
Incoming frame rate from network: 30.00 FPS
Decoding frame rate: 30.00 FPS
Rendering frame rate: 30.00 FPS
'@

Write-Host 'Parsing'
$r = @(ConvertFrom-HermitStatsText -Text $log -LogFile 'Moonlight-1.log' -Label 'synthetic')
Check 'three blocks found' ($r.Count -eq 3) "(got $($r.Count))"
Check 'label and file recorded' ($r[0].Label -eq 'synthetic' -and $r[0].LogFile -eq 'Moonlight-1.log')
Check 'resolution and codec' ($r[0].Width -eq 2560 -and $r[0].Height -eq 1440 -and $r[0].Codec -eq 'HEVC')
Check 'fps fields' ($r[0].TotalFps -eq 59.98 -and $r[0].IncomingFps -eq 59.98 -and $r[0].DecodedFps -eq 59.98 -and $r[0].RenderedFps -eq 59.97)
Check 'host latency' ($r[0].HostLatencyMinMs -eq 1.2 -and $r[0].HostLatencyMaxMs -eq 8.5 -and $r[0].HostLatencyAvgMs -eq 2.3)
Check 'drop percentages' ($r[0].NetworkDroppedPct -eq 0.05 -and $r[0].JitterDroppedPct -eq 0.10)
Check 'rtt and variance' ($r[0].RttMs -eq 25 -and $r[0].RttVarianceMs -eq 3)
Check 'decode/queue/render' ($r[0].DecodeMs -eq 1.5 -and $r[0].QueueMs -eq 4.2 -and $r[0].RenderMs -eq 3.1)
Check 'second block codec H.264 100fps' ($r[1].Codec -eq 'H.264' -and $r[1].TotalFps -eq 100.01 -and $r[1].Width -eq 1920)
Check 'bitrate line parsed when present' ($r[1].AvgBitrateMbps -eq 38.2 -and $r[1].PeakBitrateMbps -eq 55.7)
Check 'missing host latency stays null' ($null -eq $r[1].HostLatencyAvgMs)
Check 'N/A rtt stays null' ($null -eq $r[1].RttMs)
Check 'network drop 1.25 parsed' ($r[1].NetworkDroppedPct -eq 1.25)
Check 'partial trailing block kept' ($r[2].Block -eq 'Video stats' -and $r[2].Codec -eq 'HEVC 10-bit SDR' -and $null -eq $r[2].RenderMs)
Check 'no blocks in unrelated text' (@(ConvertFrom-HermitStatsText -Text "hello`nworld`n").Count -eq 0)
$real = @'
01:58:23 - SDL Info (0): Quit event received
01:58:23 - SDL Info (0):
Global video stats
01:58:23 - SDL Info (0): ----------------------------------------------------------
Incoming frame rate from network: 57.59 FPS
Decoding frame rate: 57.59 FPS
Rendering frame rate: 57.59 FPS
Host processing latency min/max/average: 0.1/20.7/4.0 ms
Frames dropped by your network connection: 0.62%
Frames dropped due to network jitter: 0.00%
Average network latency: 1 ms (variance: 5 ms)
Average decoding time: 0.16 ms
Average frame queue delay: 0.01 ms
Average rendering time (including monitor V-sync latency): 0.04 ms
01:58:23 - SDL Info (0): Stopping input stream...
'@
$rr = @(ConvertFrom-HermitStatsText -Text $real)
Check 'real log format with prefixed dashes parsed' ($rr.Count -eq 1 -and $rr[0].IncomingFps -eq 57.59 -and $rr[0].NetworkDroppedPct -eq 0.62 -and $rr[0].HostLatencyMaxMs -eq 20.7 -and $rr[0].RenderMs -eq 0.04)
Check 'missing Video stream line leaves codec null' ($null -eq $rr[0].Codec -and $null -eq $rr[0].Width)

Write-Host 'Loss details'
$lossLog = @'
00:00:01 - SDL Info (0): Video bitrate: 60000 kbps
00:00:02 - SDL Info (0): Packet size capped at 1024 bytes for remote IPv4 streaming
00:10:00 - SDL Info (0): Unrecoverable frame 100: 40+0=40 received < 50 needed
00:10:01 - SDL Info (0): Unrecoverable frame 200: 45+3=48 received < 50 needed
00:10:02 - SDL Info (0): Unrecoverable frame 300: 30+2=32 received < 50 needed
00:10:03 - SDL Info (0): IDR frame request sent
'@ + "`n" + $real
$lr = @(ConvertFrom-HermitStatsText -Text $lossLog)
Check 'bitrate and packet size read from the log' ($lr[0].VideoBitrateKbps -eq 60000 -and $lr[0].PacketSize -eq 1024)
Check 'unrecoverable frames counted' ($lr[0].UnrecoverableFrames -eq 3)
Check 'missing shards median and p90' ($lr[0].MissingShardsMedian -eq 10 -and $lr[0].MissingShardsP90 -eq 18)
Check 'IDR requests counted' ($lr[0].IdrRequests -eq 1)
$lr2 = @(ConvertFrom-HermitStatsText -Text ("00:00:01 - SDL Info (0): Using 1392 byte video packets for remote streaming (user preference)`n" + $real))
Check 'large remote packet preference recognised' ($lr2[0].PacketSize -eq 1392 -and $lr2[0].UnrecoverableFrames -eq 0 -and $null -eq $lr2[0].MissingShardsMedian)

Write-Host 'Per-block context'
$multi = @'
00:00:01 - SDL Info (0): Video bitrate: 40000 kbps
00:00:02 - SDL Info (0): Packet size capped at 1024 bytes for remote IPv4 streaming
00:00:10 - SDL Info (0): Unrecoverable frame 10: 40+0=40 received < 50 needed
00:00:11 - SDL Info (0): Network dropped 1 frame (frame 10)
00:10:00 - SDL Info (0): Global video stats
00:10:00 - SDL Info (0): ----------------------------------------------------------
Incoming frame rate from network: 59.00 FPS
Average rendering time (including monitor V-sync latency): 0.04 ms
00:10:05 - SDL Info (0): Video bitrate: 50000 kbps
00:10:06 - SDL Info (0): Using 1392 byte video packets for remote streaming (user preference)
00:20:00 - SDL Info (0): Global video stats
00:20:00 - SDL Info (0): ----------------------------------------------------------
Incoming frame rate from network: 60.00 FPS
Average rendering time (including monitor V-sync latency): 0.05 ms
'@
$mb = @(ConvertFrom-HermitStatsText -Text $multi -LogFile 'Moonlight-1790744527.log')
Check 'two blocks with their own bitrate and packet size' ($mb.Count -eq 2 -and $mb[0].BitrateKbps -eq 40000 -and $mb[0].PacketSize -eq 1024 -and $mb[1].BitrateKbps -eq 50000 -and $mb[1].PacketSize -eq 1392)
Check 'loss counters belong to the first block only' ($mb[0].BlockUnrecoverableFrames -eq 1 -and $mb[0].BlockNetworkDropEvents -eq 1 -and $mb[0].BlockMissingShardsP90 -eq 10 -and $mb[1].BlockUnrecoverableFrames -eq 0 -and $null -eq $mb[1].BlockMissingShardsP90)
$expectedEnd = [DateTimeOffset]::FromUnixTimeSeconds(1790744527).ToLocalTime().AddMinutes(10).ToString('yyyy-MM-dd HH:mm:ss')
Check 'block end time derived from log name and elapsed prefix' ($mb[0].EndTime -eq $expectedEnd -and $mb[0].Elapsed -eq '00:10:00')
Check 'no time when the log name has no timestamp' ($null -eq $lr[0].EndTime)
$realNamed = @(ConvertFrom-HermitStatsText -Text $real -LogFile 'Moonlight-1790744527.log')
$expectedReal = [DateTimeOffset]::FromUnixTimeSeconds(1790744527).ToLocalTime().Add([TimeSpan]::new(1, 58, 23)).ToString('yyyy-MM-dd HH:mm:ss')
Check 'time taken from the prefix line before a bare title' ($realNamed[0].EndTime -eq $expectedReal -and $realNamed[0].Elapsed -eq '01:58:23')
$hermitNamed = @(ConvertFrom-HermitStatsText -Text $real -LogFile 'Hermit-1790744527.log')
Check 'Hermit log names give the same time' ($hermitNamed[0].EndTime -eq $expectedReal)
Check 'title without dashes is ignored' (@(ConvertFrom-HermitStatsText -Text "Global video stats`nnot a block`n").Count -eq 0)

Write-Host 'Files'
$dir = Join-Path $env:TEMP ("mlstats-" + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $dir
try {
    [IO.File]::WriteAllText((Join-Path $dir 'Moonlight-100.log'), $log)
    [IO.File]::WriteAllText((Join-Path $dir 'Moonlight-200.log'), "nothing here`n")
    [IO.File]::WriteAllText((Join-Path $dir 'Hermit-150.log'), $log)
    [IO.File]::WriteAllText((Join-Path $dir 'other.txt'), $log)
    [IO.File]::WriteAllText((Join-Path $dir 'Hermit-notes.log'), $log)
    $out = Join-Path $dir 'stats.json'
    $res = @(& (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path $dir -Label t -OutputPath $out)
    Check 'directory scan reads only Hermit-*.log and Moonlight-*.log' ($res.Count -eq 6 -and @($res | Where-Object { $_.LogFile -in 'other.txt', 'Hermit-notes.log' }).Count -eq 0 -and @($res | Where-Object LogFile -eq 'Hermit-150.log').Count -eq 3)
    $json = Get-Content $out -Raw | ConvertFrom-Json
    Check 'json written with records' (@($json).Count -eq 6 -and $json[0].Codec -eq 'HEVC')
    $threw = $false
    try { & (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path $dir -OutputPath $out | Out-Null } catch { $threw = $true }
    Check 'existing output is never overwritten' $threw
    $single = @(& (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path (Join-Path $dir 'Moonlight-100.log'))
    Check 'single file path works' ($single.Count -eq 3)
    $threw = $false
    try { & (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path (Join-Path $dir 'missing') | Out-Null } catch { $threw = $true }
    Check 'missing path rejected' $threw
    [IO.File]::WriteAllText((Join-Path $dir 'Moonlight-300.log'), $real)
    $one = Join-Path $dir 'one.json'
    & (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path (Join-Path $dir 'Moonlight-300.log') -OutputPath $one | Out-Null
    Check 'single record is written as a JSON array' ((Get-Content $one -Raw).TrimStart().StartsWith('['))
    $empty = Join-Path $dir 'empty.json'
    & (Join-Path $PSScriptRoot 'Get-HermitStats.ps1') -Path (Join-Path $dir 'Moonlight-200.log') -OutputPath $empty | Out-Null
    Check 'no records writes an empty JSON array' ((Get-Content $empty -Raw).Trim() -eq '[]')
} finally { Remove-Item -Recurse -Force $dir }

Write-Host ''
Write-Host "Passed $script:Pass, failed $script:Fail"
if ($script:Fail -gt 0) { exit 1 }
