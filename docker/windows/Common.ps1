Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-XrWsl {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        throw 'WSL is missing. Run windows\Setup-Windows.ps1 from an elevated PowerShell.'
    }
}

function Get-XrLinuxPath {
    param([string]$Distribution, [string]$LinuxPath)
    Assert-XrWsl
    if ($LinuxPath) {
        if (-not $LinuxPath.StartsWith('/') -or $LinuxPath -eq '/') {
            throw 'LinuxPath must be an absolute Linux directory, e.g. /home/me/xr-gate-docker.'
        }
        return $LinuxPath.TrimEnd('/')
    }
    $result = & wsl.exe --distribution $Distribution --exec printenv HOME
    if ($LASTEXITCODE -ne 0) { throw "Cannot read HOME in $Distribution. Initialize the distro first." }
    $linuxHome = (($result -join "`n") -replace "`0", '').Trim()
    if (-not $linuxHome.StartsWith('/')) { throw 'Unexpected Linux HOME.' }
    return "$linuxHome/xr-gate-docker"
}

function Invoke-XrWsl {
    param([string]$Distribution, [string]$WorkingDirectory, [string[]]$LinuxArguments)
    $nativeArguments = @('--distribution', $Distribution, '--user', 'root')
    if ($WorkingDirectory) { $nativeArguments += @('--cd', $WorkingDirectory) }
    $nativeArguments += @('--exec') + $LinuxArguments
    & wsl.exe @nativeArguments
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed (exit $LASTEXITCODE): $($LinuxArguments -join ' ')"
    }
}
