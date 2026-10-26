#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
Add-Type @'
using System;
public sealed class SteamVrProcessFixture : IDisposable {
    public bool HasExited;
    public bool Disposed;
    public string ProcessName="vrmonitor";
    public void Dispose() { Disposed=true; }
}
'@
$script:Processes = @(); $script:Requests = 0; $script:QuitFails = $false; $script:ExitOnRequest = $true
function Get-XrSteamVrProcesses { return $script:Processes }
function Request-XrSteamVrQuit($Processes) {
    $script:Requests++
    if ($script:QuitFails) { throw 'fixture quit handler unavailable' }
    if ($script:ExitOnRequest) { foreach ($process in $Processes) { $process.HasExited = $true } }
}
$script:Settings['XR_STOP_STEAMVR'] = '1'
Stop-XrSteamVr -TimeoutMilliseconds 100
Assert ($script:Requests -eq 0) 'Stop launched a SteamVR quit handler when VR was not running.'
$script:Processes = @(New-Object SteamVrProcessFixture)
Stop-XrSteamVr -TimeoutMilliseconds 100
Assert ($script:Requests -eq 1 -and $script:Processes[0].Disposed) 'SteamVR did not receive a graceful exit request.'
$script:Settings['XR_STOP_STEAMVR'] = '0'
Stop-XrSteamVr -TimeoutMilliseconds 100
Assert ($script:Requests -eq 1) 'Disabled SteamVR shutdown still sent a quit request.'
$script:Settings['XR_STOP_STEAMVR'] = '1'
$script:Processes = @(New-Object SteamVrProcessFixture); $script:ExitOnRequest = $false
$before = $script:ActionWarnings.Count; $clock = [Diagnostics.Stopwatch]::StartNew()
Stop-XrSteamVr -TimeoutMilliseconds 100
Assert ($clock.Elapsed.TotalSeconds -lt 2 -and $script:ActionWarnings.Count -eq $before + 1 -and $script:Processes[0].Disposed) 'Unresponsive SteamVR blocked cleanup or had no warning.'
$script:Processes = @(New-Object SteamVrProcessFixture); $script:QuitFails = $true
Stop-XrSteamVr -TimeoutMilliseconds 100
Assert ($script:Processes[0].Disposed -and $script:ActionWarnings.Count -eq $before + 2) 'A failed quit request prevented the rest of cleanup.'
Write-Host 'OK: running-only SteamVR exit, disabled shutdown, bounded waits and failure warnings without force-killing Steam.'
