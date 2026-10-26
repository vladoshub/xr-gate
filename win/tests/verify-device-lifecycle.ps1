#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Test-MissingToolLookup {
    function Get-Command { return $null }
    function Test-Path { return $false }
    Assert ($null -eq (Get-Tool 'usbipd.exe' -Optional)) 'An optional missing usbipd lookup threw or returned a path.'
    $failed = $false
    try { Get-Tool 'usbipd.exe' | Out-Null } catch { $failed = $_.Exception.Message -like '*usbipd.exe was not found*' }
    Assert $failed 'A missing usbipd stopped being required for installation or attachment.'
}
Test-MissingToolLookup
$script:Settings = @{XR_DEVICE_NAME='Fixture glasses'; XR_USB_IDS='1234:abcd'; XR_USB_BUS_IDS=''; XR_USB_INSTANCE_IDS=''}
$script:State = [ordered]@{UsbInstanceIds=@(); BluetoothInstanceId=''; AttachedUsbInstanceIds=@()}
function Device($Id, $Bus, $Client='') { return [pscustomobject]@{InstanceId=$Id; BusId=$Bus; ClientIPAddress=$Client} }
$glasses = Device 'USB\VID_1234&PID_ABCD\HEADSET' '1-2'
$other = Device 'USB\VID_7777&PID_8888\OTHER' '1-3' '172.0.0.1'
Assert ((Get-XrDeviceObservation @($other)).device_status -eq 'NOT CONNECTED') 'An unrelated device reported a connected headset.'
Assert ((Get-XrDeviceObservation @($glasses,$other)).device_status -eq 'CONNECTED') 'A Windows-connected headset was not identified.'
$glasses.ClientIPAddress = '172.0.0.1'
$observation = Get-XrDeviceObservation @($glasses,$other)
Assert ($observation.device_status -eq 'SHARED' -and $observation.device_name -eq 'Fixture glasses') 'WSL attachment or the configured readable name was lost.'
$script:Settings.XR_USB_IDS = '1234:abcd,1111:2222'
Assert ((Get-XrDeviceObservation @($glasses)).device_status -eq 'NOT CONNECTED') 'Only one of two required USB devices enabled startup.'
$script:Settings.XR_USB_IDS = '1234:abcd'
$script:Settings.XR_USB_BUS_IDS = '1-2,9-9'
Assert ((Get-XrDeviceObservation @($glasses)).device_status -eq 'NOT CONNECTED') 'A missing explicitly selected BUSID enabled startup.'
$script:Settings.XR_USB_BUS_IDS = ''

$dongle = Device 'USB\VID_1111&PID_2222\DONGLE' '4-7' '172.0.0.1'
# Desired Bluetooth is now disabled, but Stop still owns the earlier attachment.
$script:State.AttachedUsbInstanceIds = @($glasses.InstanceId,$dongle.InstanceId)
$script:Devices = @($glasses,$dongle,$other)
$script:Attempted = New-Object 'System.Collections.Generic.List[string]'
$script:ToolAvailable = $false; $script:InventoryFails = $false; $script:InventoryCalls = 0; $script:StateSaves = 0
function Get-UsbDevices {
    $script:InventoryCalls++
    if (!$script:ToolAvailable) { throw 'USB inventory was queried without usbipd.' }
    if ($script:InventoryFails) { throw 'fixture USB inventory failed' }
    return $script:Devices
}
function Get-Tool($Name, [switch]$Optional) {
    if ($script:ToolAvailable) { return $Name }
    Assert $Optional 'Cleanup required usbipd before installation.'
    return $null
}
function Save-State { $script:StateSaves++ }
function Invoke-Native($File, $Arguments) {
    $bus = $Arguments[-1]
    $script:Attempted.Add($bus)
    if ($bus -eq '1-2') { throw 'fixture first-device failure' }
    foreach ($device in $script:Devices) { if ($device.BusId -eq $bus) { $device.ClientIPAddress = '' } }
}
$before = $script:ActionWarnings.Count
Disconnect-SelectedUsb
Assert ($script:InventoryCalls -eq 0 -and $script:Attempted.Count -eq 0 -and $script:StateSaves -eq 0) 'Missing usbipd queried devices, ran a detach or changed ownership.'
Assert ($script:ActionWarnings.Count -eq $before + 1 -and $script:ActionWarnings[-1] -like '*usbipd-win is not installed*') 'Skipped USB cleanup was not explained as a warning.'
Assert ($script:State.AttachedUsbInstanceIds.Count -eq 2) 'Missing usbipd discarded attachment ownership needed for a later retry.'

$script:ToolAvailable = $true; $script:InventoryFails = $true
$failed = $false
try { Disconnect-SelectedUsb } catch { $failed = $_.Exception.Message -eq 'fixture USB inventory failed' }
Assert ($failed -and $script:State.AttachedUsbInstanceIds.Count -eq 2) 'An installed but failing usbipd was silently treated as absent.'
$script:InventoryFails = $false
$failed = $false
try { Disconnect-SelectedUsb } catch { $failed = $_.Exception.Message.Contains('still attached') }
Assert $failed 'Incomplete detach was reported as success.'
Assert ($script:Attempted.Count -eq 2 -and '4-7' -in $script:Attempted) 'A detach failure skipped the other owned device.'
Assert ('1-3' -notin $script:Attempted -and $other.ClientIPAddress) 'An unrelated USB device was detached.'
Assert ($script:State.AttachedUsbInstanceIds.Count -eq 1 -and $script:State.AttachedUsbInstanceIds[0] -eq $glasses.InstanceId) 'Remaining attachment ownership was not retained for retry.'
Write-Host 'OK: device name/status, shutdown without usbipd, scoped USB ownership and partial-detach recovery.'
