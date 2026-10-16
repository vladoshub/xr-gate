#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param([string]$Distribution = 'Ubuntu-24.04')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    throw 'The pinned XR Gate archive requires an x64 CPU with AVX2; ARM64 is unsupported.'
}
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'Install WSL first: wsl --install -d Ubuntu-24.04 ; reboot if requested, then repeat.'
}
& wsl.exe --update
if ($LASTEXITCODE -ne 0) {
    throw 'WSL update failed. Run wsl --install if Windows features are missing; reboot if requested.'
}
$installed = ((& wsl.exe --list --quiet) -join "`n") -replace "`0", ''
if ($LASTEXITCODE -ne 0) { throw 'Cannot list WSL distributions.' }
if ($Distribution -notin ($installed -split "`n" | ForEach-Object { $_.Trim() })) {
    & wsl.exe --install --distribution $Distribution --no-launch
    if ($LASTEXITCODE -ne 0) { throw 'Distro install failed; reboot if Windows requests it.' }
    Write-Host "Launch wsl -d $Distribution, create the Linux user, then run this script again."
    return
}
# WSL 3.x is the runtime version; the distro generation is still WSL 2.
& wsl.exe --set-version $Distribution 2
if ($LASTEXITCODE -ne 0) { throw 'Cannot configure the distro as WSL 2.' }
if (-not (Get-Command usbipd.exe -ErrorAction SilentlyContinue)) {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw 'Install usbipd-win: https://github.com/dorssel/usbipd-win/releases ; then reopen PowerShell.'
    }
    & winget.exe install --interactive --exact dorssel.usbipd-win --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw 'usbipd-win installation failed.' }
}
& wsl.exe --version
& wsl.exe --list --verbose
Write-Host 'Reopen a normal PowerShell. Next: .\windows\Install-XrGate.ps1'
Write-Host 'The bundle uses Docker Engine inside Ubuntu, rather than a Docker Desktop engine.'
