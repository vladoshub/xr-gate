#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
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

$dongle = Device 'USB\VID_1111&PID_2222\DONGLE' '4-7' '172.0.0.1'
# Desired Bluetooth is now disabled, but Stop still owns the earlier attachment.
$script:State.AttachedUsbInstanceIds = @($glasses.InstanceId,$dongle.InstanceId)
$script:Devices = @($glasses,$dongle,$other)
$script:Attempted = New-Object 'System.Collections.Generic.List[string]'
function Get-UsbDevices { return $script:Devices }
function Get-Tool($Name) { return $Name }
function Save-State { }
function Invoke-Native($File, $Arguments) {
    $bus = $Arguments[-1]
    $script:Attempted.Add($bus)
    if ($bus -eq '1-2') { throw 'fixture first-device failure' }
    foreach ($device in $script:Devices) { if ($device.BusId -eq $bus) { $device.ClientIPAddress = '' } }
}
$failed = $false
try { Disconnect-SelectedUsb } catch { $failed = $_.Exception.Message.Contains('still attached') }
Assert $failed 'Incomplete detach was reported as success.'
Assert ($script:Attempted.Count -eq 2 -and '4-7' -in $script:Attempted) 'A detach failure skipped the other owned device.'
Assert ('1-3' -notin $script:Attempted -and $other.ClientIPAddress) 'An unrelated USB device was detached.'
Assert ($script:State.AttachedUsbInstanceIds.Count -eq 1 -and $script:State.AttachedUsbInstanceIds[0] -eq $glasses.InstanceId) 'Remaining attachment ownership was not retained for retry.'
Write-Host 'OK: device name/status, scoped USB ownership and partial-detach recovery.'
