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
$script:Settings['XR_DISPLAY_MATCH'] = 'Fixture glasses'
$script:Settings['XR_TARGET_DEVICE'] = 'fixture'
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
Assert ($status.display_name -eq '\\.\DISPLAY3' -and $status.display_device_id -eq 'glasses') 'Display protection did not receive the exact selected output and monitor identity.'
$script:State.DriverPath = 'XR_GATE_MISSING_SETUP_DRIVE:\driver'
Assert ((Get-XrSetupObservation $false).display_connected) 'A stale driver drive blocked pre-install display selection.'
$script:State.DriverPath = ''
$script:Displays = @($script:Displays[0])
Assert (!(Get-XrSetupObservation $false).display_connected) 'An unplugged saved display was treated as present.'
$disconnected = Get-XrSetupObservation $false
Assert (!$disconnected.display_name -and !$disconnected.display_device_id) 'An unplugged display retained a protection target.'
$script:Displays += [pscustomobject]@{DeviceName='\\.\DISPLAY3'; DeviceId='glasses'; Name='Fixture glasses'; Width=3840; Height=1080; Frequency=90; X=1920; Y=0}
$script:State.DisplayDeviceId = ''; $script:State.DisplayDeviceName = ''
$before = $script:Saved
Assert (!(Get-XrSetupObservation $false $false).display_selected) 'An unconnected device automatically selected a monitor.'
$automatic = Get-XrSetupObservation $false $true
Assert ($automatic.display_selected -and $automatic.display_connected -and $automatic.display_number -eq '3') 'A unique configured display was not selected automatically for the connected device.'
Assert (!$script:State.DisplayDeviceId -and $script:Saved -eq $before) 'Read-only status persisted a display selection while another action could own the state.'
$duplicate = [pscustomobject]@{DeviceName='\\.\DISPLAY4'; DeviceId='second-glasses'; Name='Fixture glasses'; Frequency=90}
$script:Displays += $duplicate
Assert (!(Get-XrSetupObservation $false $true).display_selected) 'Ambiguous displays were guessed instead of requiring manual selection.'
$script:Displays = @($script:Displays | Where-Object { $_.DeviceId -ne 'second-glasses' })
$null = Get-XrSetupObservation $false $true -RememberDisplay
Assert ($script:State.DisplayDeviceId -eq 'glasses' -and $script:Saved -eq $before + 1) 'Start VR did not persist its automatic display choice.'
$script:State.DisplayDeviceId = 'main'
Assert ((Get-XrSetupObservation $false $true).display_number -eq '1') 'Automatic selection replaced an explicit saved display.'
$script:State.DisplayDeviceId = 'glasses'
$script:State.DisplayTargetDevice = 'other-device'
Assert (!(Get-XrSetupObservation $false $false).display_connected) 'A saved display for another hardware device was reused.'
$script:State.DisplayTargetDevice = 'fixture'
$folder = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-setup-' + [Guid]::NewGuid().ToString('N'))
try {
    $script:State.DriverPath = $folder
    foreach ($relative in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $path = Join-Path $folder $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, 'fixture')
    }
    @{xr_tracking=@{deviceProfile=(Get-XrDriverDeviceId);displayFrequency=60;isDisplayOnDesktop=$true}} | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $folder 'resources/settings/default.vrsettings')
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
Write-Host 'OK: automatic/manual displays, connected-device gating, ambiguity, read-only polling and complete installation contract.'
