#requires -Version 7.4
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ShellRecovery.psm1') -Force
$root = Join-Path ([IO.Path]::GetTempPath()) ('Shell-test-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path (Join-Path $root 'source/credentials') -Force
$source = Join-Path $root 'source'
$fixtures = @{
    'shell.conf' = "# test configuration`nencoder = nvenc"
    'apps.json' = '{"apps":[{"name":"테스트 게임"}]}'
    'shell_state.json' = '{"test-secret":"fixture-only"}'
    'credentials/cakey.pem' = 'fixture-private-key'
    'credentials/cacert.pem' = 'fixture-certificate'
    'shell.log' = 'This log must not be backed up.'
}
foreach ($name in $fixtures.Keys) { [IO.File]::WriteAllText((Join-Path $source $name), $fixtures[$name]) }
$script:passed = 0
function Assert-True($Condition, [string]$Label) {
    if (-not $Condition) { throw "FAIL: $Label" }
    $script:passed++; Write-Output "PASS: $Label"
}
function Assert-Rejected([scriptblock]$Action, [string]$Label) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert-True $rejected $Label
}
function Write-TestArchive([object[]]$Files, [string]$Path) {
    $payload = [Text.Encoding]::UTF8.GetBytes((@{SchemaVersion=1; Scope='full'; Files=$Files} | ConvertTo-Json -Depth 6))
    $cipher = [Security.Cryptography.ProtectedData]::Protect($payload, $null, 'CurrentUser')
    @{Format='Shell-v1';Mode='windows-current-user';Data=[Convert]::ToBase64String($cipher)} |
        ConvertTo-Json | Set-Content -LiteralPath $Path
}

$password = ConvertTo-SecureString 'test-password-fixture-2026' -AsPlainText -Force
foreach ($mode in @('local', 'portable')) {
    $argsForMode = @{}
    if ($mode -eq 'portable') { $argsForMode.Password = $password }
    $backup = Join-Path $root "$mode.apbackup"
    $result = Backup-ShellConfig -ConfigDirectory $source -OutputPath $backup @argsForMode
    Assert-True ($result.Files -eq 5) "${mode}: includes configuration and credentials, excludes logs"
    Assert-True (-not ([IO.File]::ReadAllText($backup).Contains('fixture-private-key'))) "${mode}: no plaintext key in envelope"
    $destination = Join-Path $root "recover-$mode"
    $recovered = Expand-ShellBackup -BackupPath $backup -Destination $destination @argsForMode
    Assert-True (-not $recovered.Activated) "${mode}: recovery never activates configuration"
    foreach ($name in $fixtures.Keys | Where-Object { $_ -ne 'shell.log' }) {
        Assert-True ((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash -eq
            (Get-FileHash -LiteralPath (Join-Path $destination $name)).Hash) "${mode}: exact roundtrip for $name"
    }
    Assert-Rejected { Backup-ShellConfig -ConfigDirectory $source -OutputPath $backup @argsForMode } "${mode}: backup overwrite rejected"
    Assert-Rejected { Expand-ShellBackup -BackupPath $backup -Destination $source @argsForMode } "${mode}: existing recovery destination rejected"
}
$portable = Join-Path $root 'portable.apbackup'
$wrong = ConvertTo-SecureString 'wrong-password-fixture' -AsPlainText -Force
$failedDest = Join-Path $root 'wrong-password'
Assert-Rejected { Expand-ShellBackup -BackupPath $portable -Destination $failedDest -Password $wrong } 'wrong password rejected'
Assert-True (-not (Test-Path -LiteralPath $failedDest)) 'wrong password writes no recovery directory'
Assert-Rejected { Expand-ShellBackup -BackupPath $portable -Destination $failedDest } 'missing password rejected'
$envelope = Get-Content -LiteralPath $portable -Raw | ConvertFrom-Json
$damaged = [Convert]::FromBase64String($envelope.Data)
$damaged[0] = $damaged[0] -bxor 1
$envelope.Data = [Convert]::ToBase64String($damaged)
$tampered = Join-Path $root 'tampered.apbackup'
$envelope | ConvertTo-Json | Set-Content -LiteralPath $tampered
Assert-Rejected { Expand-ShellBackup -BackupPath $tampered -Destination $failedDest -Password $password } 'modified ciphertext rejected'
Assert-True (-not (Test-Path -LiteralPath $failedDest)) 'modified ciphertext writes no recovery directory'

$data = [Text.Encoding]::UTF8.GetBytes('fixture')
$hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($data))
foreach ($badPath in @('credentials/../../escaped', 'credentials/CON.txt', 'credentials/key.', 'credentials//key')) {
    $malicious = Join-Path $root ('unsafe-' + [guid]::NewGuid() + '.apbackup')
    Write-TestArchive @(@{Path=$badPath;Data=[Convert]::ToBase64String($data);Sha256=$hash}) $malicious
    Assert-Rejected { Expand-ShellBackup -BackupPath $malicious -Destination $failedDest } "unsafe path rejected: $badPath"
}
$duplicates = @(
    @{Path='shell.conf';Data=[Convert]::ToBase64String($data);Sha256=$hash},
    @{Path='SHELL.CONF';Data=[Convert]::ToBase64String($data);Sha256=$hash}
)
$duplicateArchive = Join-Path $root 'duplicate.apbackup'
Write-TestArchive $duplicates $duplicateArchive
Assert-Rejected { Expand-ShellBackup -BackupPath $duplicateArchive -Destination $failedDest } 'case-insensitive duplicate rejected'
$badHashArchive = Join-Path $root 'bad-hash.apbackup'
Write-TestArchive @(@{Path='shell.conf';Data=[Convert]::ToBase64String($data);Sha256='invalid'}) $badHashArchive
Assert-Rejected { Expand-ShellBackup -BackupPath $badHashArchive -Destination $failedDest } 'invalid hash rejected'
$settingsBackup = Join-Path $root 'settings.apbackup'
$settingsResult = Backup-ShellConfig -ConfigDirectory $source -OutputPath $settingsBackup -SettingsOnly
Assert-True ($settingsResult.Scope -eq 'settings-only' -and $settingsResult.Files -eq 2) 'settings-only backup explicitly excludes pairing and certificates'
$settingsDest = Join-Path $root 'settings-recovery'
$settingsRecovery = Expand-ShellBackup -BackupPath $settingsBackup -Destination $settingsDest
Assert-True ($settingsRecovery.Scope -eq 'settings-only' -and -not (Test-Path (Join-Path $settingsDest 'shell_state.json'))) 'settings-only recovery preserves its limited scope'
[IO.File]::AppendAllText((Join-Path $source 'shell.conf'), "`nfile_state = outside.json")
Assert-Rejected { Backup-ShellConfig -ConfigDirectory $source -OutputPath (Join-Path $root 'custom.apbackup') } 'custom paths rejected instead of producing incomplete backup'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $root 'custom.apbackup'))) 'unsupported configuration writes no archive'
Write-Output "$script:passed checks passed. Synthetic fixtures retained at: $root"
