#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Expected) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert ($message.Contains($Expected)) ('Expected rejection containing: ' + $Expected + '; got: ' + $message)
}
function Device([string]$BusId, [string]$Identity, [string]$Name) {
    return [pscustomobject]@{BusId=$BusId; InstanceId=$Identity; Description=$Name}
}
$camera = Device '3-7' 'USB\VID_1234&PID_5678\CAM' 'Camera'
$asus = Device '2-7' 'USB\VID_0B05&PID_190E\ASUS' 'ASUS USB-BT500'
$system = Device '3-13' 'USB\VID_0489&PID_E0E2\SYSTEM' 'MediaTek system adapter'
$devices = @($camera, $asus, $system)

$bt = Select-BluetoothDevice $devices -UsbId '0b05:190e' -SavedInstanceId $system.InstanceId
Assert ($bt.InstanceId -eq $asus.InstanceId) 'The saved system adapter overrode the configured VID:PID.'
$asus.BusId = '8-9'
$bt = Select-BluetoothDevice $devices -UsbId '0B05:190E'
Assert ($bt.BusId -eq '8-9') 'VID:PID selection did not follow the device to a new USB port.'
$bt = Select-BluetoothDevice $devices -ExplicitBusId '3-13' -UsbId '0b05:190e'
Assert ($bt.InstanceId -eq $system.InstanceId) 'An explicit BUSID did not override VID:PID.'
$bt = Select-BluetoothDevice $devices -ExplicitInstanceId $system.InstanceId -UsbId '0b05:190e'
Assert ($bt.InstanceId -eq $system.InstanceId) 'An explicit advanced USB instance was ignored.'
$bt = Select-BluetoothDevice $devices -SavedInstanceId $system.InstanceId
Assert ($bt.InstanceId -eq $system.InstanceId) 'Empty selectors did not preserve the saved device.'
Reject { Select-BluetoothDevice $devices -ExplicitBusId '9-9' -UsbId '0b05:190e' } 'BUSID 9-9 was not found'
Reject { Select-BluetoothDevice $devices -ExplicitBusId 'invalid' -UsbId '0b05:190e' } 'BUSID must look like'
Reject { Select-BluetoothDevice $devices -ExplicitInstanceId 'gone' -UsbId '0b05:190e' } 'instance was not found'
Reject { Select-BluetoothDevice @($camera,$system) -UsbId '0b05:190e' -SavedInstanceId $system.InstanceId } '0b05:190e was not found'
foreach ($id in @('0b05:190','0b05:190e*','*:*','0b05:190e,0489:e0e2','190e')) {
    Reject { Select-BluetoothDevice $devices -UsbId $id } 'one VID:PID pair'
}
$partial = Device '2-8' 'USB\VID_0B05&PID_190EA\OTHER' 'Unrelated PID prefix'
Reject { Select-BluetoothDevice @($partial) -UsbId '0b05:190e' } 'was not found'
$composite = Device '2-8' 'USB\VID_0B05&PID_190E&MI_00\COMPOSITE' 'Composite USB identity'
Assert ((Select-BluetoothDevice @($composite) -UsbId '0b05:190e').BusId -eq '2-8') 'A composite USB identity was rejected.'
$second = Device '2-8' 'USB\VID_0B05&PID_190E\SECOND' 'Second matching adapter'
Reject { Select-BluetoothDevice @($asus,$second,$system) -UsbId '0b05:190e' -SavedInstanceId $system.InstanceId } 'Multiple Bluetooth adapters'
$bt = Select-BluetoothDevice @($asus,$second) -UsbId '0b05:190e' -SavedInstanceId $second.InstanceId
Assert ($bt.InstanceId -eq $second.InstanceId) 'An existing selection did not disambiguate identical adapters.'

$script:SaveCount = 0
function Save-State { $script:SaveCount++ }
$script:Settings['XR_USB_IDS'] = '1234:5678'
$script:Settings['XR_USB_INSTANCE_IDS'] = ''
$script:Settings['XR_BLUETOOTH'] = '1'
$script:Settings['XR_BLUETOOTH_INSTANCE_ID'] = ''
$script:BluetoothBusId = ''
$script:BluetoothUsbId = '0b05:190e'
$script:BluetoothUsbIdExplicit = $false
$script:UsbBusId = @()
$script:WithoutBluetooth = $false
$script:State.BluetoothInstanceId = $system.InstanceId
$selected = @(Get-SelectedUsb $devices)
Assert ($selected.Count -eq 2 -and $script:SaveCount -eq 1) 'USB selection did not save exactly one camera and one adapter.'
Assert ($script:State.BluetoothInstanceId -eq $asus.InstanceId) 'The selected adapter was not persisted for detach and HCI checks.'
Assert ((New-LinuxRequest).bluetooth_usb_id -eq '0b05:190e') 'Firmware readiness did not receive the resolved adapter identity.'
Assert (!($selected | Where-Object { $_.InstanceId -eq $system.InstanceId })) 'The system adapter was selected alongside the dedicated one.'
$script:BluetoothBusId = '3-7'
Reject { Set-SelectedBluetooth $devices -RuntimeInstanceIds @($camera.InstanceId) } 'already selected for camera'
$script:BluetoothBusId = ''
$script:Settings['XR_BLUETOOTH'] = '0'
Assert ($null -eq (Set-SelectedBluetooth $devices) -and !$script:State.BluetoothInstanceId) 'Disabled Bluetooth still selected an adapter.'
$script:Settings['XR_BLUETOOTH'] = '1'
$script:WithoutBluetooth = $true
Assert (!(Test-BluetoothSelectionRequested) -and $null -eq (Set-SelectedBluetooth $devices)) '-WithoutBluetooth did not disable automatic selection.'
$script:WithoutBluetooth = $false
$script:Settings['XR_BLUETOOTH'] = ''
$script:State.BluetoothInstanceId = ''
Assert (!(Test-BluetoothSelectionRequested) -and $null -eq (Set-SelectedBluetooth $devices)) 'A default VID:PID enabled Bluetooth without user selection.'
$script:BluetoothUsbIdExplicit = $true
Assert ((Set-SelectedBluetooth $devices).InstanceId -eq $asus.InstanceId) 'Explicit CLI VID:PID did not request adapter selection.'
'OK: Bluetooth BUSID/instance/VID:PID priority, USB port changes, missing/ambiguous adapters, exact identity and disabled Bluetooth'
