#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-test-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    @'
XR_HARDWARE_ENV=hardware.env
XR_DISTRIBUTION=Ubuntu-24.04
XR_DISPLAY_MODE=extended_sbs
XR_DISPLAY_OPTION=current
'@ | Set-Content -LiteralPath (Join-Path $temporary '.env.example') -Encoding UTF8
    @'
XR_TARGET_DEVICE=fixture_camera
XR_USB_IDS=1234:abcd
XR_DISPLAY_FREQUENCY_HZ=72
XR_WINDOW_WIDTH=2520
XR_WINDOW_HEIGHT=1440
XR_RENDER_WIDTH=1260
XR_RENDER_HEIGHT=1440
XR_REQUIRE_CAMERA=1
'@ | Set-Content -LiteralPath (Join-Path $temporary 'hardware.env') -Encoding UTF8
    @'
XR_USB_BUS_IDS=2-3
XR_STEAMVR_PATH="D:\A folder\SteamVR"
'@ | Set-Content -LiteralPath (Join-Path $temporary '.env') -Encoding UTF8
    Initialize-XrSettings $temporary
    Assert ((Get-XrSetting 'XR_USB_IDS') -eq '1234:abcd') 'Hardware preset was ignored.'
    Assert ((Get-XrSetting 'XR_STEAMVR_PATH') -eq 'D:\A folder\SteamVR') 'Quoted Windows path was altered.'
    Assert ((Get-XrInteger 'XR_WINDOW_WIDTH') -eq 2520) 'Custom geometry was ignored.'
    $env:XR_USB_IDS = '5678:abcd'
    Initialize-XrSettings $temporary
    Assert ((Get-XrSetting 'XR_USB_IDS') -eq '5678:abcd') 'Process environment must override .env.'
    Remove-Item Env:XR_USB_IDS
    Initialize-XrSettings $temporary
    $devices = @(
        [pscustomobject]@{BusId='2-3'; InstanceId='USB\VID_1234&PID_ABCD\camera'; Description='Custom camera'},
        [pscustomobject]@{BusId='2-4'; InstanceId='USB\VID_3318&PID_0426\other'; Description='Other camera'}
    )
    $selected = @(Select-RuntimeDevices -Devices $devices)
    Assert ($selected.Count -eq 1 -and $selected[0].BusId -eq '2-3') 'A non-XREAL VID/PID was not selected.'
    $selected = @(Select-RuntimeDevices -Devices $devices -ExplicitBusIds @('2-4'))
    Assert ($selected.Count -eq 1 -and $selected[0].BusId -eq '2-4') 'Explicit USB BUSID was ignored.'
    $devices += [pscustomobject]@{BusId='2-5'; InstanceId='USB\VID_5678&PID_DEF0\imu'; Description='Serial IMU'}
    $script:Settings['XR_USB_IDS'] = '1234:abcd,5678:def0'
    $selected = @(Select-RuntimeDevices -Devices $devices)
    Assert ($selected.Count -eq 2 -and !($selected | Where-Object { $_.BusId -eq '2-4' })) 'Multiple device identities were not respected.'
    $script:State.FrequencyHz = 72
    $script:Settings['XR_BLUETOOTH'] = '0'
    $script:State.BluetoothInstanceId = 'saved-dongle'
    Assert-SbsDisplay ([pscustomobject]@{Width=2520; Height=1440; Frequency=72})
    $bad = $false
    try { Assert-SbsDisplay ([pscustomobject]@{Width=3840; Height=1080; Frequency=72}) } catch { $bad = $true }
    Assert $bad 'Display check accepted a different hardware resolution.'

    $script:StateFile = Join-Path $temporary 'state.json'
    '{"FrequencyHz":90,"XrealInstanceId":"legacy-instance"}' | Set-Content -LiteralPath $script:StateFile
    $script:State.UsbInstanceIds = @()
    $FrequencyHz = 0; $Distribution = ''; $DisplayMode = ''; $DisplayOption = ''
    Load-State
    Assert (!$script:State.BluetoothInstanceId) 'XR_BLUETOOTH=0 did not disable the saved dongle.'
    Assert ($script:State.FrequencyHz -eq 90) 'Saved interactive selection was lost.'
    Assert ($script:State.UsbInstanceIds[0] -eq 'legacy-instance') 'Old USB selection was not migrated.'
    $script:StateDir = $temporary
    $UsbBusId = @('2-3'); $BluetoothBusId = '2-4'
    Assert (@(Get-SelectedUsb $devices).Count -eq 1) 'XR_BLUETOOTH=0 still attached the configured dongle.'
    $script:SettingOverrides['XR_DISPLAY_FREQUENCY_HZ'] = '72'
    Load-State
    Assert ([int]$script:State.FrequencyHz -eq 72) '.env did not override the saved selection.'
    $script:Settings['XR_AUTO_DRIVER_FREQUENCY'] = '1'
    Load-State
    Assert ([int]$script:State.FrequencyHz -eq 90) 'Manual .env frequency replaced the automatically selected/saved driver frequency.'
    $script:Settings['XR_AUTO_DRIVER_FREQUENCY'] = '0'
    Load-State

    foreach ($bundleName in @('xr-gate-windows-wsl2-usbip.zip', 'xr-gate-windows-wsl-usbip.zip', 'xr-gate-windows.zip')) {
        $bundleRootName = [IO.Path]::GetFileNameWithoutExtension($bundleName)
        $bundleRoot = Join-Path $temporary ('archive-' + $bundleRootName + '/' + $bundleRootName)
        $package = Join-Path $bundleRoot 'drivers/fixture_camera/72Hz-extended_sbs/xr_tracking'
        foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
            $target = Join-Path $package $name
            New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
            'fixture' | Set-Content -LiteralPath $target
        }
        '{"xr_tracking":{"deviceProfile":"fixture_camera","displayFrequency":72,"isDisplayOnDesktop":true}}' |
            Set-Content -LiteralPath (Join-Path $package 'resources/settings/default.vrsettings')
        '{"schema":2,"release_tag":"v9.9.9","devices":["fixture_camera"],"variants":[{"device":"fixture_camera","frequency_hz":72,"display_mode":"extended_sbs","path":"drivers/fixture_camera/72Hz-extended_sbs/xr_tracking"}]}' |
            Set-Content -LiteralPath (Join-Path $bundleRoot 'bundle.json')
        Assert ((Find-BundledDriver $bundleRoot) -eq $package) 'Bundled driver selection failed.'
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $script:TestBundleName = $bundleName
        $script:TestArchive = Join-Path $temporary $bundleName
        [IO.Compression.ZipFile]::CreateFromDirectory((Split-Path -Parent $bundleRoot), $script:TestArchive)
        function Save-ReleaseAsset($Asset, [string]$Directory) {
            Assert ($Asset.name -eq $script:TestBundleName) 'Installer requested an old standalone variant.'
            return $script:TestArchive
        }
        $release = [pscustomobject]@{tag_name='v9.9.9'; assets=@([pscustomobject]@{
            name=$bundleName; digest=('sha256:' + ('f' * 64));
            browser_download_url=('https://github.com/vladoshub/xr-gate/releases/download/v9.9.9/' + $bundleName)
        })}
        $cache = Join-Path $temporary ('cache-' + $bundleRootName)
        New-Item -ItemType Directory $cache | Out-Null
        $downloaded = Get-DriverPackage $release $cache
        Assert (Test-Path -LiteralPath (Join-Path $downloaded 'bin/win64/driver_xr_tracking.dll')) 'Combined release bundle was not installed.'
        $automatic = Get-AutomaticDriverBundle $release $cache
        Assert ((Find-BundledDriver $automatic) -eq $downloaded) ('Automatic driver lookup failed for ' + $bundleName)
        $offline = Get-AutomaticDriverBundle ([pscustomobject]@{tag_name='v9.9.9'; assets=@()}) $cache
        Assert ((Find-BundledDriver $offline) -eq $downloaded) ('Offline driver cache failed for ' + $bundleName)
    }
    $selection = Resolve-DriverFrequency -Available @(60,90) -Detected 90
    Assert ($selection.Frequency -eq 90 -and !$selection.Fallback) 'Current glasses frequency did not select its matching driver.'
    $selection = Resolve-DriverFrequency -Available @(60,90) -Detected 59
    Assert ($selection.Frequency -eq 60 -and !$selection.Fallback) '59/60 Hz Windows rounding was not handled.'
    $selection = Resolve-DriverFrequency -Available @(60,90) -Detected 120
    Assert ($selection.Frequency -eq 60 -and $selection.Fallback) 'Unsupported glasses frequency did not fall back to 60 Hz.'
    $selection = Resolve-DriverFrequency -Available @(60,90) -Detected 0
    Assert ($selection.Frequency -eq 60 -and $selection.Fallback) 'Unknown glasses frequency silently selected another monitor frequency.'
    $bad = $false
    try { Resolve-DriverFrequency -Available @(90) -Detected 120 } catch { $bad = $true }
    Assert $bad 'A package without the required fallback silently selected 90 Hz.'
    $DisplayName = ''; $script:State.DisplayDeviceId = ''
    $script:Settings['XR_DISPLAY_MATCH'] = 'Custom headset'
    $glasses = [pscustomobject]@{DeviceName='DISPLAY7'; DeviceId='custom-1'; Name='Custom headset'; Width=2520; Height=1440; Frequency=72; Primary=$false}
    $main = [pscustomobject]@{DeviceName='DISPLAY1'; DeviceId='monitor-1'; Name='Main monitor'; Width=1920; Height=1080; Frequency=144; Primary=$true}
    Assert ((Find-AutomaticXrDisplay @($main,$glasses)).DeviceId -eq 'custom-1') 'A non-XREAL hardware display match was ignored.'
    $clone = [pscustomobject]@{DeviceName='DISPLAY7'; DeviceId='clone'; Name='Another monitor'; Width=2520; Height=1440; Frequency=72; Primary=$false}
    Assert ($null -eq (Find-AutomaticXrDisplay @($main,$glasses,$clone))) 'An ambiguous cloned output was selected automatically.'
    $script:State.DisplayDeviceId = 'custom-1'
    Assert ($null -eq (Find-AutomaticXrDisplay @($main,$glasses,$clone))) 'A saved display silently changed a cloned main-monitor output.'
    $script:State.DisplayDeviceId = ''; $script:Settings['XR_DISPLAY_MATCH'] = ''
    $main.Width = 2520; $main.Height = 1440
    Assert ($null -eq (Find-AutomaticXrDisplay @($main))) 'The main monitor was guessed from geometry alone.'
    $unrelated = [pscustomobject]@{DeviceName='DISPLAY8'; DeviceId='monitor-2'; Name='Office monitor'; Width=2520; Height=1440; Frequency=144; Primary=$false}
    Assert ($null -eq (Find-AutomaticXrDisplay @($main,$unrelated))) 'An unrelated secondary monitor was guessed from resolution.'
    function Get-DesktopDisplays { return @($main, $unrelated) }
    $observation = Get-XrDisplayObservation
    Assert ($null -eq $observation.frequency_hz) 'Missing glasses were assigned a monitor or configured driver frequency.'
    $script:Settings['XR_DISPLAY_MATCH'] = 'Custom headset'
    function Get-DesktopDisplays { return @($main, $glasses) }
    $configuredBeforeObservation = [int]$script:State.FrequencyHz
    $observation = Get-XrDisplayObservation
    Assert ($observation.frequency_hz -eq 72) 'Status did not read the current glasses output frequency.'
    Assert ([int]$script:State.FrequencyHz -eq $configuredBeforeObservation) 'Read-only status changed the selected driver.'
    Select-AutomaticDriverFrequency $release $cache
    Assert ([int]$script:State.FrequencyHz -eq 72) 'Automatic selection did not use the current custom headset frequency.'
    Assert ($script:State.DisplayDeviceId -eq 'custom-1') 'Automatic selection did not save the selected glasses identity.'
    Assert ($script:ResolvedDriverRoot -eq (Split-Path -Parent $downloaded | Split-Path -Parent | Split-Path -Parent | Split-Path -Parent)) 'Automatic selection ignored the verified cached Windows bundle.'
    Add-Type -Path (Join-Path $root 'windows/DisplayNative.cs')
    Assert ([XrGateWsl.DisplayNative]::ModeStructSize -eq 220) 'DEVMODEW layout changed.'
    Assert ([XrGateWsl.DisplayNative]::DeviceStructSize -eq 840) 'DISPLAY_DEVICEW layout changed.'
    'OK Windows env, generic USB/display matching, frequency fallback, state migration and archive installation'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
