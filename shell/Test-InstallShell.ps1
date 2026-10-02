#requires -Version 5.1
# Exercises Install-Shell.ps1 (first installation, update, rollback, uninstall and the opt-in
# migration from Apollo) against scratch folders with -NoService.
# Uses synthetic ZIPs only; the real Shell installation and service are never touched.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [bool]$Condition, [string]$Detail = '') {
    if ($Condition) { $script:Pass++; Write-Host "  ok   $Name" }
    else { $script:Fail++; Write-Host "  FAIL $Name $Detail" -ForegroundColor Red }
}
$installer = Join-Path $PSScriptRoot 'Install-Shell.ps1'
function Invoke-Installer([hashtable]$Params) {
    # Runs the installer in a child process; its exit code is left in $LASTEXITCODE.
    $argList = @()
    foreach ($k in $Params.Keys) {
        if ($Params[$k] -is [bool]) { if ($Params[$k]) { $argList += "-$k" } }
        else { $argList += "-$k"; $argList += [string]$Params[$k] }
    }
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer @argList 2>&1 | Out-String }
    finally { $ErrorActionPreference = $prev }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("shell-install-test-" + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $root 'Shell'
$backups = Join-Path $root 'backups'
function Write-Text([string]$Path, [string]$Text) { $null = New-Item -ItemType Directory -Path (Split-Path $Path) -Force; [IO.File]::WriteAllText($Path, $Text) }

# SIDs with an access rule on a file or folder (names are localized, SIDs are not)
function Get-AclSids([string]$Path) {
    @((Get-Acl -LiteralPath $Path).Access | ForEach-Object {
        $ref = $_.IdentityReference
        try { $ref.Translate([Security.Principal.SecurityIdentifier]).Value } catch { $ref.Value }
    })
}
# Readable by Administrators and SYSTEM, not by Users, Authenticated Users or Everyone
function Test-Private([string]$Path) {
    $sids = Get-AclSids $Path
    ($sids -contains 'S-1-5-32-544') -and ($sids -contains 'S-1-5-18') -and
        -not ($sids -contains 'S-1-5-32-545' -or $sids -contains 'S-1-5-11' -or $sids -contains 'S-1-1-0')
}
function Test-NoInherit([string]$Path) { (Get-Acl -LiteralPath $Path).AreAccessRulesProtected }

function New-TestZip([string]$Path, [hashtable]$Entries) {
    $stage = Join-Path $root ("zip-" + [Guid]::NewGuid().ToString('N'))
    foreach ($k in $Entries.Keys) { Write-Text (Join-Path $stage $k) $Entries[$k] }
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $Path)
}

try {
    $none = Join-Path $root 'no-apollo'   # an Apollo folder that does not exist
    # Users may read everything below the scratch root, as in Program Files and ProgramData
    $null = New-Item -ItemType Directory -Path $root -Force
    & icacls.exe $root /grant '*S-1-5-32-545:(OI)(CI)(RX)' /Q | Out-Null

    # Fake existing installation
    Write-Text "$install\shell.exe" 'OLD-EXE'
    Write-Text "$install\zlib1.dll" 'OLD-ZLIB'
    Write-Text "$install\Uninstall.exe" 'UNINSTALL'
    Write-Text "$install\assets\web\index.html" 'OLD-WEB'
    Write-Text "$install\assets\old-only.txt" 'OLD-ONLY'
    Write-Text "$install\tools\shellsvc.exe" 'OLD-SVC'
    Write-Text "$install\config\shell.conf" 'encoder = nvenc'
    Write-Text "$install\config\credentials\cakey.pem" 'KEY'
    Write-Text "$install\drivers\sudovda\driver.inf" 'OLD-DRIVER'

    $good = Join-Path $root 'good.zip'
    New-TestZip $good @{
        'Shell\shell.exe' = 'NEW-EXE'; 'Shell\zlib1.dll' = 'NEW-ZLIB'; 'Shell\assets\web\index.html' = 'NEW-WEB'
        'Shell\tools\shellsvc.exe' = 'NEW-SVC'; 'Shell\drivers\sudovda\driver.inf' = 'NEW-DRIVER'
    }

    Write-Host 'Rejects bad ZIPs'
    $bad1 = Join-Path $root 'bad-layout.zip'; New-TestZip $bad1 @{ 'Other\shell.exe' = 'X' }
    $bad2 = Join-Path $root 'bad-noexe.zip'; New-TestZip $bad2 @{ 'Shell\readme.txt' = 'X' }
    foreach ($z in @($bad1, $bad2)) {
        $out = Invoke-Installer @{ ZipPath = $z; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true } 2>&1
        Check "rejected $(Split-Path $z -Leaf)" ($LASTEXITCODE -ne 0)
    }
    Check 'install untouched after rejected ZIPs' ([IO.File]::ReadAllText("$install\shell.exe") -eq 'OLD-EXE')
    Check 'no backup made for rejected ZIPs' (-not (Test-Path $backups))

    Write-Host 'Update'
    $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true } 2>&1
    Check 'update exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'shell.exe replaced' ([IO.File]::ReadAllText("$install\shell.exe") -eq 'NEW-EXE')
    Check 'assets replaced' ([IO.File]::ReadAllText("$install\assets\web\index.html") -eq 'NEW-WEB')
    Check 'service wrapper replaced' ([IO.File]::ReadAllText("$install\tools\shellsvc.exe") -eq 'NEW-SVC')
    Check 'config untouched' ([IO.File]::ReadAllText("$install\config\shell.conf") -eq 'encoder = nvenc' -and (Test-Path "$install\config\credentials\cakey.pem"))
    Check 'drivers untouched' ([IO.File]::ReadAllText("$install\drivers\sudovda\driver.inf") -eq 'OLD-DRIVER')
    Check 'uninstaller kept' (Test-Path "$install\Uninstall.exe")
    $bk = @(Get-ChildItem $backups -Directory)
    Check 'one backup folder' ($bk.Count -eq 1)
    $bkFiles = Join-Path $bk[0].FullName 'files'
    Check 'backup has old exe' ([IO.File]::ReadAllText("$bkFiles\shell.exe") -eq 'OLD-EXE')
    Check 'backup includes config and credentials' (Test-Path "$bkFiles\config\credentials\cakey.pem")
    Check 'backup includes drivers' (Test-Path "$bkFiles\drivers\sudovda\driver.inf")
    $manifest = Get-Content (Join-Path $bk[0].FullName 'backup-manifest.json') -Raw | ConvertFrom-Json
    Check 'manifest records file count' ($manifest.FileCount -eq 9)
    Check 'backup folder no longer inherits' (Test-NoInherit $backups)
    Check 'backup folder readable by Administrators and SYSTEM only' (Test-Private $backups)
    Check 'backed-up private key not readable by Users' (Test-Private "$bkFiles\config\credentials\cakey.pem")
    Check 'existing credentials folder restricted by the update' ((Test-NoInherit "$install\config\credentials") -and (Test-Private "$install\config\credentials\cakey.pem"))

    Write-Host 'Rollback'
    Write-Text "$install\config\shell.conf" 'encoder = nvenc # changed after deploy'
    $out = Invoke-Installer @{ Rollback = $true; BackupPath = $bk[0].FullName; InstallDir = $install; NoService = $true } 2>&1
    Check 'rollback exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'old exe restored' ([IO.File]::ReadAllText("$install\shell.exe") -eq 'OLD-EXE')
    Check 'old assets restored including removed file' ([IO.File]::ReadAllText("$install\assets\web\index.html") -eq 'OLD-WEB' -and (Test-Path "$install\assets\old-only.txt"))
    Check 'config left as is without -RestoreConfig' ([IO.File]::ReadAllText("$install\config\shell.conf") -match 'changed after deploy')
    $out = Invoke-Installer @{ Rollback = $true; BackupPath = $bk[0].FullName; InstallDir = $install; NoService = $true; RestoreConfig = $true } 2>&1
    Check 'config restored with -RestoreConfig' ([IO.File]::ReadAllText("$install\config\shell.conf") -eq 'encoder = nvenc')
    $out = Invoke-Installer @{ Rollback = $true; BackupPath = $root; InstallDir = $install; NoService = $true } 2>&1
    Check 'rollback from a non-backup folder rejected' ($LASTEXITCODE -ne 0)

    Write-Host 'Backup pruning'
    $foreign = Join-Path $backups 'not-a-backup'
    Write-Text "$foreign\keep.txt" 'user data'
    $original = $bk[0].Name
    foreach ($i in 1..4) {
        Start-Sleep -Seconds 1   # backup folder names carry a per-second timestamp
        $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true; KeepBackups = 2 } 2>&1
        if ($LASTEXITCODE -ne 0) { Check "deploy $i with pruning" $false ($out | Out-String) }
    }
    $left = @(Get-ChildItem $backups -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'backup-manifest.json') } | Sort-Object Name)
    Check 'oldest plus newest 2 backups remain' ($left.Count -eq 3) "(got $($left.Count))"
    Check 'original backup is always kept' ($left[0].Name -eq $original)
    Check 'folders without a manifest are never removed' (Test-Path "$foreign\keep.txt")
    Start-Sleep -Seconds 1   # a backup made in the same second as the previous one would collide
    $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true } 2>&1
    $after = @(Get-ChildItem $backups -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'backup-manifest.json') })
    Check 'default keeps every backup' ($after.Count -eq 4) ($out | Out-String)

    Write-Host 'Ctrl+Alt+Del policy (a scratch registry key)'
    $sasKey = 'HKCU:\Software\ShellInstallTest-' + [Guid]::NewGuid().ToString('N')
    try {
        Start-Sleep -Seconds 1
        $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true; SasPolicyKey = $sasKey } 2>&1
        $value = (Get-Item -LiteralPath $sasKey -ErrorAction SilentlyContinue)
        $value = if ($value) { $value.GetValue('SoftwareSASGeneration', $null) } else { $null }
        Check 'policy created with the services bit' ($LASTEXITCODE -eq 0 -and $value -eq 1) ("value=$value " + ($out | Out-String))
        Set-ItemProperty -LiteralPath $sasKey -Name 'SoftwareSASGeneration' -Value 2 -Type DWord
        Start-Sleep -Seconds 1
        $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $install; BackupRoot = $backups; OldInstallDir = $none; NoService = $true; SasPolicyKey = $sasKey } 2>&1
        $value = (Get-Item -LiteralPath $sasKey).GetValue('SoftwareSASGeneration', $null)
        Check 'services bit added to an existing value' ($value -eq 3) ("value=$value " + ($out | Out-String))
    } finally {
        Remove-Item -LiteralPath $sasKey -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host 'First installation'
    $froot = Join-Path $root 'fresh'
    $fresh = Join-Path $froot 'Shell'
    $fbk = Join-Path $froot 'backups'
    $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $fresh; BackupRoot = $fbk; OldInstallDir = $none; NoService = $true } 2>&1
    Check 'install exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'install reported' ($out -match 'Install')
    Check 'program files installed' ([IO.File]::ReadAllText("$fresh\shell.exe") -eq 'NEW-EXE' -and (Test-Path "$fresh\tools\shellsvc.exe"))
    Check 'drivers installed' ([IO.File]::ReadAllText("$fresh\drivers\sudovda\driver.inf") -eq 'NEW-DRIVER' -and -not (Test-Path "$fresh\drivers\drivers"))
    Check 'config and credentials folders created' (Test-Path "$fresh\config\credentials")
    Write-Text "$fresh\config\credentials\cakey.pem" 'KEY'
    Check 'new credentials folder restricted' ((Test-NoInherit "$fresh\config\credentials") -and (Test-Private "$fresh\config\credentials\cakey.pem"))
    Check 'no backup for a first installation' (-not (Test-Path $fbk))
    Start-Sleep -Seconds 1
    $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $fresh; BackupRoot = $fbk; OldInstallDir = $none; NoService = $true } 2>&1
    Check 'second run updates with a backup' ($LASTEXITCODE -eq 0 -and ($out -match 'Update') -and @(Get-ChildItem $fbk -Directory).Count -eq 1) ($out | Out-String)
    $out = Invoke-Installer @{ ZipPath = $bad2; InstallDir = (Join-Path $froot 'Other'); OldInstallDir = $none; NoService = $true } 2>&1
    Check 'first installation from a bad ZIP rejected' ($LASTEXITCODE -ne 0 -and -not (Test-Path (Join-Path $froot 'Other')))

    Write-Host 'Uninstall'
    Write-Text "$fresh\config\shell.conf" 'encoder = nvenc'
    $out = Invoke-Installer @{ Uninstall = $true; InstallDir = $fresh; BackupRoot = $fbk; NoService = $true } 2>&1
    Check 'uninstall exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    $kept = @(Get-ChildItem $fbk -Directory -Filter 'uninstalled-*')
    Check 'install folder moved into the backups with its config' ((-not (Test-Path $fresh)) -and $kept.Count -eq 1 -and (Test-Path (Join-Path $kept[0].FullName 'config\shell.conf')))
    Check 'moved folder readable by Administrators and SYSTEM only' ((Test-Private $kept[0].FullName) -and (Test-Private (Join-Path $kept[0].FullName 'config\shell.conf')))
    $out = Invoke-Installer @{ Uninstall = $true; InstallDir = $fresh; BackupRoot = $fbk; NoService = $true } 2>&1
    Check 'uninstall without an installation rejected' ($LASTEXITCODE -ne 0)

    Write-Host 'Migration from Apollo (opt-in)'
    $mroot = Join-Path $root 'mig'
    $old = Join-Path $mroot 'Apollo'
    $new = Join-Path $mroot 'Shell'
    $mbk = Join-Path $mroot 'backups'
    $mig = @{ InstallDir = $new; BackupRoot = $mbk; OldInstallDir = $old; NoService = $true }
    Write-Text "$old\sunshine.exe" 'OLD-EXE'
    Write-Text "$old\config\sunshine.conf" "encoder = nvenc`nsunshine_name = host`nglobal_prep_cmd = [{`"do`":`"echo %APOLLO_CLIENT_FPS%`"}]"
    Write-Text "$old\config\sunshine_state.json" '{"root":{"uniqueid":"U"}}'
    $coverPath = (Join-Path $old 'config\covers\a.png').Replace('\', '\\')
    $otherTool = ($old + ' Tools\x.exe').Replace('\', '\\')
    Write-Text "$old\config\apps.json" ('{"apps":[{"name":"APOLLO_PC","image-path":"' + $coverPath + '","cmd":"run.exe %APOLLO_APP_ID%","detached":["' + $otherTool + '"]}]}')
    Write-Text "$old\config\credentials\cakey.pem" 'KEY'
    Write-Text "$old\config\sunshine.log" 'LOG'
    Write-Text "$old\drivers\sudovda\driver.inf" 'DRIVER'

    $out = Invoke-Installer ($mig + @{ ZipPath = $good }) 2>&1
    Check 'Apollo found without -MigrateFromApollo: nothing installed' ($LASTEXITCODE -ne 0 -and -not (Test-Path $new)) ($out | Out-String)
    $out = Invoke-Installer @{ ZipPath = $good; InstallDir = $new; BackupRoot = $mbk; OldInstallDir = $none; NoService = $true; MigrateFromApollo = $true } 2>&1
    Check '-MigrateFromApollo without Apollo rejected' ($LASTEXITCODE -ne 0 -and -not (Test-Path $new))

    $out = Invoke-Installer ($mig + @{ ZipPath = $good; MigrateFromApollo = $true }) 2>&1
    Check 'migration exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'migration reported' ($out -match 'Migrate')
    Check 'program files installed' ([IO.File]::ReadAllText("$new\shell.exe") -eq 'NEW-EXE')
    Check 'settings renamed' ((Test-Path "$new\config\shell.conf") -and (Test-Path "$new\config\shell_state.json") -and -not (Test-Path "$new\config\sunshine.conf"))
    $conf = [IO.File]::ReadAllText("$new\config\shell.conf")
    Check 'host name key renamed' ($conf -match '(?m)^shell_name = host' -and $conf -notmatch 'sunshine_name')
    Check 'Apollo variable names updated in settings' ($conf -match 'SHELL_CLIENT_FPS' -and $conf -notmatch 'APOLLO_')
    $apps = [IO.File]::ReadAllText("$new\config\apps.json")
    Check 'cover path points into the new folder' ($apps.Contains($new.Replace('\', '\\') + '\\config') -and -not $apps.Contains($old.Replace('\', '\\') + '\\config'))
    Check 'Apollo variable names updated in apps' ($apps -match 'SHELL_APP_ID' -and $apps -notmatch 'APOLLO_APP_ID')
    Check 'unrelated names and sibling folders left alone' ($apps -match '"APOLLO_PC"' -and $apps.Contains($otherTool))
    Check 'apps.json still valid JSON' ($null -ne ($apps | ConvertFrom-Json))
    Check 'credentials copied' (Test-Path "$new\config\credentials\cakey.pem")
    Check 'migrated credentials restricted' ((Test-NoInherit "$new\config\credentials") -and (Test-Private "$new\config\credentials\cakey.pem"))
    Check 'Apollo log not copied' (-not (Test-Path "$new\config\sunshine.log"))
    Check 'drivers copied without nesting' ((Test-Path "$new\drivers\sudovda\driver.inf") -and -not (Test-Path "$new\drivers\drivers"))
    Check 'Apollo installation left in place' (Test-Path "$old\sunshine.exe")
    $out = Invoke-Installer ($mig + @{ ZipPath = $good; MigrateFromApollo = $true }) 2>&1
    Check '-MigrateFromApollo once Shell is installed rejected' ($LASTEXITCODE -ne 0)

    Write-Host 'Revert and migrate again'
    $out = Invoke-Installer ($mig + @{ RevertMigration = $true }) 2>&1
    Check 'revert exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'new folder put aside' ((-not (Test-Path $new)) -and @(Get-ChildItem $mroot -Directory -Filter 'Shell.reverted-*').Count -eq 1)
    $out = Invoke-Installer ($mig + @{ ZipPath = $good; MigrateFromApollo = $true }) 2>&1
    Check 'migrates again after a revert' ($LASTEXITCODE -eq 0 -and ($out -match 'Migrate')) ($out | Out-String)
    Check 'the reverted folder is kept' (@(Get-ChildItem $mroot -Directory -Filter 'Shell.reverted-*').Count -eq 1)

    Write-Host 'Updates after a migration'
    foreach ($i in 1..3) {
        Start-Sleep -Seconds 1
        $out = Invoke-Installer ($mig + @{ ZipPath = $good; KeepBackups = 1 }) 2>&1
        if ($LASTEXITCODE -ne 0) { Check "update $i after migration" $false ($out | Out-String) }
    }
    Check 'updates after a migration keep the oldest and newest backup' (@(Get-ChildItem $mbk -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'backup-manifest.json') }).Count -eq 2)

    Write-Host 'Remove the Apollo installation'
    $out = Invoke-Installer ($mig + @{ RemoveOldInstall = $true }) 2>&1
    Check 'remove exit code 0' ($LASTEXITCODE -eq 0) ($out | Out-String)
    Check 'Apollo folder moved into the backups' ((-not (Test-Path $old)) -and @(Get-ChildItem $mbk -Directory -Filter 'old-install-*').Count -eq 1)
    $oldMoved = @(Get-ChildItem $mbk -Directory -Filter 'old-install-*')[0].FullName
    Check 'moved Apollo credentials not readable by Users' (Test-Private (Join-Path $oldMoved 'config\credentials\cakey.pem'))
    Check 'Shell still installed' (Test-Path "$new\shell.exe")
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "Passed $script:Pass, failed $script:Fail"
if ($script:Fail -gt 0) { exit 1 }
