#requires -Version 7.4
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-PlainPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Reparse points are not supported for backup or recovery.'
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Get-ContentHash([byte[]]$Bytes) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
}

function Get-PasswordKey([securestring]$Password, [byte[]]$Salt) {
    if ($Password.Length -lt 12) { throw 'Use a backup password of at least 12 characters.' }
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
    try {
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
        [Security.Cryptography.Rfc2898DeriveBytes]::Pbkdf2(
            $plain, $Salt, 600000, [Security.Cryptography.HashAlgorithmName]::SHA256, 32)
    }
    finally {
        $plain = $null
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
    }
}

function Write-NewFile([string]$Path, [byte[]]$Bytes) {
    Assert-PlainPath $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($Bytes); $stream.Flush($true) }
    finally { $stream.Dispose() }
}

function Get-ShellDiagnostic {
    [CmdletBinding()]
    param([string]$InstallDirectory = "$env:ProgramFiles\Shell")
    $issues = [Collections.Generic.List[string]]::new()
    $service = Get-Service ShellService -ErrorAction SilentlyContinue
    if (-not $service) { $issues.Add('Shell service was not found.') }
    elseif ($service.Status -ne 'Running') { $issues.Add('Shell service is not running.') }
    $gpu = @(); $network = @(); $os = $null
    try {
        $gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
        $os = (Get-CimInstance Win32_OperatingSystem).Caption
        $network = @(Get-NetAdapter -Physical | Select-Object InterfaceDescription, Status, LinkSpeed)
    } catch { $issues.Add('Hardware inventory unavailable; try an elevated terminal.') }
    $exe = Join-Path $InstallDirectory 'shell.exe'
    $version = if (Test-Path -LiteralPath $exe) { (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion } else { $null }
    $config = Join-Path $InstallDirectory 'config'
    $files = @('shell.conf', 'apps.json', 'shell_state.json', 'credentials/cacert.pem', 'credentials/cakey.pem')
    $presence = foreach ($name in $files) {
        [pscustomobject]@{ File = $name; Present = (Test-Path -LiteralPath (Join-Path $config $name) -PathType Leaf) }
    }
    # Remote access over a Tailscale network is common for streaming away from home
    $tailscaleService = Get-Service Tailscale -ErrorAction SilentlyContinue
    [pscustomobject]@{
        SchemaVersion = 1; CreatedUtc = [DateTime]::UtcNow.ToString('o')
        OS = $os; GPU = $gpu; Network = $network
        ShellVersion = $version
        ShellService = $(if ($service) { [string]$service.Status } else { 'NotFound' })
        ConfigFiles = @($presence)
        TailscaleService = $(if ($tailscaleService) { [string]$tailscaleService.Status } else { 'NotFound' })
        Issues = @($issues.ToArray())
        Unmeasured = @('Internet upload capacity', 'External latency and packet loss', 'Wake-on-LAN from outside the local network', 'Streaming frame times')
    }
}

function Backup-ShellConfig {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigDirectory,
        [Parameter(Mandatory)][string]$OutputPath,
        [securestring]$Password,
        [switch]$SettingsOnly
    )
    $root = [IO.Path]::GetFullPath($ConfigDirectory)
    Assert-PlainPath $root
    Assert-PlainPath $OutputPath
    if (Test-Path -LiteralPath $OutputPath) { throw 'Backup destination already exists.' }
    $conf = Join-Path $root 'shell.conf'
    if (-not (Test-Path -LiteralPath $conf -PathType Leaf)) { throw 'shell.conf not found.' }
    # A partial backup with external state/key files would be misleading. Support
    # custom locations only when their recovery mapping has been implemented.
    if ([IO.File]::ReadAllText($conf) -match '(?m)^\s*(pkey|cert|file_state|credentials_file|file_apps)\s*=') {
        throw 'Custom configuration file locations are not supported yet; no backup was created.'
    }
    $paths = [Collections.Generic.List[string]]::new()
    $requiredFiles = if ($SettingsOnly) { @('shell.conf', 'apps.json') } else { @('shell.conf', 'apps.json', 'shell_state.json') }
    foreach ($name in $requiredFiles) {
        $path = Join-Path $root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required configuration file missing: $name" }
        $paths.Add($path)
    }
    $credentials = Join-Path $root 'credentials'
    if (-not $SettingsOnly) {
        foreach ($name in @('cacert.pem', 'cakey.pem')) {
            if (-not (Test-Path -LiteralPath (Join-Path $credentials $name) -PathType Leaf)) { throw "Required credential file missing: $name" }
        }
        Assert-PlainPath $credentials
        foreach ($item in Get-ChildItem -LiteralPath $credentials -Recurse -Force) {
            Assert-PlainPath $item.FullName
            if (-not $item.PSIsContainer) { $paths.Add($item.FullName) }
        }
    }
    $entries = [Collections.Generic.List[object]]::new()
    $total = 0L
    foreach ($path in $paths) {
        Assert-PlainPath $path
        if ((Get-Item -LiteralPath $path).Length -gt 16MB) { throw 'Configuration file is too large.' }
        $bytes = [IO.File]::ReadAllBytes($path)
        $total += $bytes.Length
        if ($total -gt 32MB -or $entries.Count -ge 1000) { throw 'Configuration backup exceeds supported size.' }
        $entries.Add([pscustomobject]@{
            Path = [IO.Path]::GetRelativePath($root, $path).Replace('\', '/')
            Sha256 = Get-ContentHash $bytes; Data = [Convert]::ToBase64String($bytes)
        })
    }
    foreach ($entry in $entries) {
        if ((Get-ContentHash ([IO.File]::ReadAllBytes((Join-Path $root $entry.Path)))) -ne $entry.Sha256) {
            throw 'Configuration changed during backup. Retry when settings are idle.'
        }
    }
    $scope = if ($SettingsOnly) { 'settings-only' } else { 'full' }
    $payload = [Text.Encoding]::UTF8.GetBytes((@{
        SchemaVersion = 1; Scope = $scope; CreatedUtc = [DateTime]::UtcNow.ToString('o'); Files = @($entries.ToArray())
    } | ConvertTo-Json -Depth 6 -Compress))
    $key = $null
    try {
        if ($Password) {
            $salt = [Security.Cryptography.RandomNumberGenerator]::GetBytes(16)
            $nonce = [Security.Cryptography.RandomNumberGenerator]::GetBytes(12)
            [byte[]]$key = Get-PasswordKey $Password $salt
            $cipher = [byte[]]::new($payload.Length); $tag = [byte[]]::new(16)
            $aes = [Security.Cryptography.AesGcm]::new($key, 16)
            try { $aes.Encrypt($nonce, $payload, $cipher, $tag) } finally { $aes.Dispose() }
            $envelope = @{
                Format = 'Shell-v1'; Mode = 'password-aes256gcm-pbkdf2-sha256-600000'
                Salt = [Convert]::ToBase64String($salt); Nonce = [Convert]::ToBase64String($nonce)
                Tag = [Convert]::ToBase64String($tag); Data = [Convert]::ToBase64String($cipher)
            }
        } else {
            $cipher = [Security.Cryptography.ProtectedData]::Protect($payload, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
            $envelope = @{ Format = 'Shell-v1'; Mode = 'windows-current-user'; Data = [Convert]::ToBase64String($cipher) }
        }
        Write-NewFile $OutputPath ([Text.Encoding]::UTF8.GetBytes(($envelope | ConvertTo-Json -Compress)))
        [pscustomobject]@{ Path = [IO.Path]::GetFullPath($OutputPath); Files = $entries.Count; Scope = $scope; Mode = $envelope.Mode }
    } finally {
        [Array]::Clear($payload, 0, $payload.Length)
        if ($key) { [Array]::Clear($key, 0, $key.Length) }
    }
}

function Expand-ShellBackup {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BackupPath,
        [Parameter(Mandatory)][string]$Destination,
        [securestring]$Password
    )
    $dest = [IO.Path]::GetFullPath($Destination)
    Assert-PlainPath $BackupPath
    Assert-PlainPath $dest
    if (Test-Path -LiteralPath $dest) { throw 'Recovery requires a new destination directory.' }
    if ((Get-Item -LiteralPath $BackupPath).Length -gt 64MB) { throw 'Backup exceeds supported size.' }
    $envelope = Get-Content -LiteralPath $BackupPath -Raw | ConvertFrom-Json
    if ($envelope.Format -ne 'Shell-v1') { throw 'Unsupported backup format.' }
    $payload = $null; $key = $null
    try {
        $cipher = [Convert]::FromBase64String($envelope.Data)
        switch ($envelope.Mode) {
            'windows-current-user' {
                $payload = [Security.Cryptography.ProtectedData]::Unprotect($cipher, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
            }
            'password-aes256gcm-pbkdf2-sha256-600000' {
                if (-not $Password) { throw 'This backup requires its password.' }
                [byte[]]$key = Get-PasswordKey $Password ([Convert]::FromBase64String($envelope.Salt))
                $payload = [byte[]]::new($cipher.Length)
                $aes = [Security.Cryptography.AesGcm]::new($key, 16)
                try { $aes.Decrypt([Convert]::FromBase64String($envelope.Nonce), $cipher, [Convert]::FromBase64String($envelope.Tag), $payload) }
                finally { $aes.Dispose() }
            }
            default { throw 'Unsupported encryption mode.' }
        }
        $archive = [Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json
        if ($archive.SchemaVersion -ne 1 -or @($archive.Files).Count -gt 1000) { throw 'Unsupported archive schema or file count.' }
        if ($archive.Scope -notin @('full', 'settings-only')) { throw 'Unsupported backup scope.' }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $validated = [Collections.Generic.List[object]]::new()
        $total = 0L
        foreach ($entry in $archive.Files) {
            $relative = [string]$entry.Path
            if ($relative -notmatch '^(shell\.conf|apps\.json|shell_state\.json|credentials/[a-zA-Z0-9_./-]+)$' -or
                $relative.Split('/') -contains '..' -or $relative.Split('/') -contains '.' -or $relative.Contains('//') -or
                $relative.EndsWith('/') -or $relative -match '(^|/)(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|/|$)' -or
                $relative -match '\.(/|$)' -or -not $seen.Add($relative)) {
                throw 'Unsafe or duplicate archive path.'
            }
            $bytes = [Convert]::FromBase64String($entry.Data)
            $total += $bytes.Length
            if ($bytes.Length -gt 16MB -or $total -gt 32MB -or (Get-ContentHash $bytes) -ne $entry.Sha256) { throw 'Invalid archive contents.' }
            $validated.Add([pscustomobject]@{ Path = Join-Path $dest $relative; Bytes = $bytes })
        }
        $requiredFiles = if ($archive.Scope -eq 'full') {
            @('shell.conf', 'apps.json', 'shell_state.json', 'credentials/cakey.pem', 'credentials/cacert.pem')
        } else { @('shell.conf', 'apps.json') }
        if ($archive.Scope -eq 'settings-only' -and $seen.Count -ne 2) { throw 'Unexpected files in settings-only backup.' }
        foreach ($required in $requiredFiles) {
            if (-not $seen.Contains($required)) { throw 'Incomplete configuration archive.' }
        }
        # Check path collisions before creating any recovery files.
        foreach ($name in $seen) {
            $parts = $name.Split('/')
            for ($i = 1; $i -lt $parts.Length; $i++) {
                if ($seen.Contains(($parts[0..($i - 1)] -join '/'))) { throw 'Archive file/directory collision.' }
            }
        }
        $null = New-Item -ItemType Directory -Path $dest -ErrorAction Stop
        # Recovered credentials should only be readable by the current account.
        $acl = [Security.AccessControl.DirectorySecurity]::new()
        $acl.SetAccessRuleProtection($true, $false)
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.WindowsIdentity]::GetCurrent().User, 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
        Set-Acl -LiteralPath $dest -AclObject $acl
        foreach ($item in $validated) {
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.Path))
            Write-NewFile $item.Path $item.Bytes
        }
        [pscustomobject]@{ Path = $dest; Files = $validated.Count; Scope = $archive.Scope; Activated = $false }
    } finally {
        if ($payload) { [Array]::Clear($payload, 0, $payload.Length) }
        if ($key) { [Array]::Clear($key, 0, $key.Length) }
    }
}

Export-ModuleMember -Function Get-ShellDiagnostic, Backup-ShellConfig, Expand-ShellBackup
