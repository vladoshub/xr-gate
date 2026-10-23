#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:State = [ordered]@{DriverPath=''; RegisteredDriver=''; InstallPending=$false; InstallNextStep='';
    DisplayDeviceId=''; DisplayDeviceName=''; FrequencyHz=60; DisplayMode='extended_sbs'}
$script:DisplayName = ''
$script:Saved = 0
$script:Registered = $false
$script:Displays = @(
    [pscustomobject]@{DeviceName='\\.\DISPLAY1'; DeviceId='main'; Name='Main'; Width=1920; Height=1080; Frequency=60; X=0; Y=0},
    [pscustomobject]@{DeviceName='\\.\DISPLAY3'; DeviceId='glasses'; Name='Fixture glasses'; Width=1920; Height=1080; Frequency=90; X=1920; Y=0}
)
function Get-DesktopDisplays { return $script:Displays }
function Assert-SteamVrStopped { }
function Read-Host($Prompt) { return '2' }
function Save-State { $script:Saved++ }
function Invoke-Linux { throw 'Pre-install display selection attempted to start Linux.' }
function Set-XrDisplaySettings { throw 'Pre-install display selection attempted to configure an uninstalled driver.' }
function Test-XrDriverRegistration($Path) { return $script:Registered }
$status = Get-XrSetupObservation $false
Assert (!$status.display_selected -and !$status.setup_complete) 'Fresh setup guessed a selected display.'
Choose-SbsDisplay -SelectOnly
Assert ($script:Saved -eq 1 -and $script:State.DisplayDeviceId -eq 'glasses' -and $script:State.FrequencyHz -eq 90) 'Pre-install selection did not save the actual display identity and Hz.'
$status = Get-XrSetupObservation $false
Assert ($status.display_selected -and $status.display_connected -and $status.display_number -eq '3' -and !$status.setup_complete) 'Selecting a display required Linux or falsely marked setup complete.'
$script:State.DriverPath = 'XR_GATE_MISSING_SETUP_DRIVE:\driver'
Assert ((Get-XrSetupObservation $false).display_connected) 'A stale driver drive blocked pre-install display selection.'
$script:State.DriverPath = ''
$script:Displays = @($script:Displays[0])
Assert (!(Get-XrSetupObservation $false).display_connected) 'An unplugged saved display was treated as present.'
$script:Displays += [pscustomobject]@{DeviceName='\\.\DISPLAY3'; DeviceId='glasses'; Name='Fixture glasses'; Width=3840; Height=1080; Frequency=90; X=1920; Y=0}
$folder = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-setup-' + [Guid]::NewGuid().ToString('N'))
try {
    $script:State.DriverPath = $folder
    foreach ($relative in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $path = Join-Path $folder $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, 'fixture')
    }
    Assert (!(Get-XrSetupObservation $true).setup_complete) 'Unregistered driver was treated as fully installed.'
    $script:Registered = $true
    Assert (!(Get-XrSetupObservation $false).setup_complete) 'A missing Linux runtime was treated as fully installed.'
    Assert ((Get-XrSetupObservation $true).setup_complete) 'Complete Linux/driver installation was rejected.'
    function Test-XrDriverRegistration($Path) { throw 'fixture unreadable OpenVR registration' }
    $status = Get-XrSetupObservation $true
    Assert (!$status.setup_complete -and $status.display_connected) 'Unreadable registration blocked display selection or enabled startup.'
    function Test-XrDriverRegistration($Path) { return $script:Registered }
    $script:State.InstallPending = $true; $script:State.InstallNextStep = 'restart_windows'
    $status = Get-XrSetupObservation $true
    Assert (!$status.setup_complete -and $status.install_pending -and $status.install_next_step -eq 'restart_windows') 'Paused setup lost its required restart.'
    $script:State.InstallPending = $false
    Remove-Item -LiteralPath (Join-Path $folder 'resources/settings/default.vrsettings')
    Assert (!(Get-XrSetupObservation $true).setup_complete) 'An incomplete driver package was treated as fully installed.'
} finally { Remove-Item -LiteralPath $folder -Recurse -Force }
Write-Host 'OK: display selection before WSL, current refresh rate, disconnected display and complete installation contract.'
