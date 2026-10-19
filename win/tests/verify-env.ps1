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

    $bundleRoot = Join-Path $temporary 'archive/xr-gate-windows'
    $package = Join-Path $bundleRoot 'drivers/72Hz-extended_sbs/xr_tracking'
    foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $target = Join-Path $package $name
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        'fixture' | Set-Content -LiteralPath $target
    }
    '{"schema":1,"release_tag":"v9.9.9","variants":[{"frequency_hz":72,"display_mode":"extended_sbs"}]}' |
        Set-Content -LiteralPath (Join-Path $bundleRoot 'bundle.json')
    Assert ((Find-BundledDriver $bundleRoot) -eq $package) 'Bundled driver selection failed.'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $script:TestArchive = Join-Path $temporary 'xr-gate-windows.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory((Split-Path -Parent $bundleRoot), $script:TestArchive)
    function Save-ReleaseAsset($Asset, [string]$Directory) {
        Assert ($Asset.name -eq 'xr-gate-windows.zip') 'Installer requested an old standalone variant.'
        return $script:TestArchive
    }
    $release = [pscustomobject]@{tag_name='v9.9.9'; assets=@([pscustomobject]@{
        name='xr-gate-windows.zip'; digest=('sha256:' + ('f' * 64));
        browser_download_url='https://github.com/vladoshub/xr-gate/releases/download/v9.9.9/xr-gate-windows.zip'
    })}
    $cache = Join-Path $temporary 'cache'
    New-Item -ItemType Directory $cache | Out-Null
    $downloaded = Get-DriverPackage $release $cache
    Assert (Test-Path -LiteralPath (Join-Path $downloaded 'bin/win64/driver_xr_tracking.dll')) 'Combined release bundle was not installed.'
    'OK Windows env precedence, generic USB, display geometry, state migration and combined archive installation'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
