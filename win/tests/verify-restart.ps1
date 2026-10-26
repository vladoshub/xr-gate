#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:Calls = New-Object 'System.Collections.Generic.List[string]'
$script:Fail = ''
function Request-RuntimeWindowStop { $script:Calls.Add('cancel-startup') }
function New-LinuxRequest { return @{user='fixture_user'} }
function Invoke-Linux($Operation, $Request, [switch]$AsRoot) {
    $script:Calls.Add($Operation)
    if ($Operation -eq $script:Fail) { throw 'fixture runtime cleanup failure' }
    Assert ($Operation -ne 'prepare' -or $AsRoot) 'Runtime preparation must retain the root device-access step.'
}
function Stop-RuntimeWindow { $script:Calls.Add('close-window') }
function Start-RuntimeWindow { $script:Calls.Add('start-window') }
function Read-RuntimeWindow { return @{Token='fixture'} }
function Wait-XrStream($Seconds, $RuntimeWindow) { Assert ($RuntimeWindow.Token -eq 'fixture') 'Restart lost the new runtime owner.'; $script:Calls.Add('ready') }
function Stop-XrSteamVr { throw 'Restart must not stop SteamVR.' }
function Disconnect-SelectedUsb { throw 'Restart must not detach USB.' }
function Connect-SelectedUsb { throw 'Restart must preserve the current USB attachment.' }
function Update-AutomaticWindowsDriver { throw 'Restart must not replace a live SteamVR driver.' }
Restart-XrTracking
Assert (($script:Calls -join ',') -eq 'cancel-startup,stop,close-window,prepare,start-window,ready') 'Restart skipped ordered cleanup, access preparation or the HMD readiness check.'
foreach ($failure in @('stop','prepare')) {
    $script:Fail = $failure; $script:Calls.Clear(); $rejected = $false
    try { Restart-XrTracking } catch { $rejected = $true }
    Assert ($rejected -and !$script:Calls.Contains('start-window')) 'Failed cleanup/preparation launched a second runtime.'
}
Write-Host 'OK: ordered XR Gate restart, new runtime readiness and failure handling preserve SteamVR, driver/display state and USB.'
