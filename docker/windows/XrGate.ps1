#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Run', 'Up', 'Console', 'Stop', 'Logs', 'Doctor', 'Config', 'ExportConfigs', 'CheckImage', 'Shell')]
    [string]$Command = 'Run',
    [string]$Distribution = 'Ubuntu-24.04',
    [string]$LinuxPath,
    [switch]$Bluetooth,
    [switch]$Video,
    [string[]]$ClientArguments = @()
)
. "$PSScriptRoot\Common.ps1"
$destination = Get-XrLinuxPath -Distribution $Distribution -LinuxPath $LinuxPath
$mapping = @{ ExportConfigs = 'export-configs'; CheckImage = 'check-image' }
$linuxCommand = $Command.ToLowerInvariant()
if ($mapping.ContainsKey($Command)) { $linuxCommand = $mapping[$Command] }
$linuxArguments = @('env')
if ($Bluetooth) { $linuxArguments += 'XR_BLUETOOTH=1' }
if ($Video) { $linuxArguments += 'XR_VIDEO_TCP=1' }
$linuxArguments += @('bash', './xr-gate', $linuxCommand)
if ($ClientArguments.Count -gt 0) { $linuxArguments += @('--') + $ClientArguments }
Invoke-XrWsl -Distribution $Distribution -WorkingDirectory $destination -LinuxArguments $linuxArguments
