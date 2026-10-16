#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Distribution = 'Ubuntu-24.04',
    [string]$LinuxPath,
    [switch]$SkipBuild
)
. "$PSScriptRoot\Common.ps1"
$destination = Get-XrLinuxPath -Distribution $Distribution -LinuxPath $LinuxPath
if ($destination.StartsWith('/mnt/')) { throw 'Choose a directory in the Linux filesystem, e.g. /home/me/xr-gate-docker.' }
$sourceWindows = (Resolve-Path "$PSScriptRoot\..").Path
$result = & wsl.exe --distribution $Distribution --exec wslpath -a -u $sourceWindows
if ($LASTEXITCODE -ne 0) { throw 'Cannot convert the bundle path with wslpath.' }
$sourceLinux = (($result -join "`n") -replace "`0", '').Trim()

# All paths are positional arguments. A script file also avoids native-argument
# quoting differences between Windows PowerShell 5.1 and PowerShell 7.
Invoke-XrWsl -Distribution $Distribution -LinuxArguments @('bash', "$sourceLinux/scripts/install-copy.sh", $sourceLinux, $destination)
Invoke-XrWsl -Distribution $Distribution -WorkingDirectory $destination -LinuxArguments @('bash', 'scripts/install-engine-ubuntu.sh')
if (-not $SkipBuild) {
    Invoke-XrWsl -Distribution $Distribution -WorkingDirectory $destination -LinuxArguments @('bash', './xr-gate', 'build')
    Invoke-XrWsl -Distribution $Distribution -WorkingDirectory $destination -LinuxArguments @('bash', './xr-gate', 'check-image')
}
Write-Host "Installed into $Distribution : $destination"
Write-Host 'Attach USB with Usb-XrGate.ps1, then run XrGate.ps1 -Command Run.'
