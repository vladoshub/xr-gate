#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:Calls = New-Object 'System.Collections.Generic.List[object]'
$script:Camera = [pscustomobject]@{BusId='3-7';InstanceId='USB\VID_1234&PID_5678\CAM';Description='Camera';PersistedGuid='camera-bound';ClientIPAddress='host'}
$script:Bluetooth = [pscustomobject]@{BusId='2-7';InstanceId='USB\VID_ABCD&PID_0123\BT';Description='Selected dongle';PersistedGuid='bt-bound';ClientIPAddress='host'}
$script:Other = [pscustomobject]@{BusId='3-13';InstanceId='USB\VID_3333&PID_4444\OTHER';Description='Other dongle';PersistedGuid='other-bound';ClientIPAddress='host'}
function Start-Process { param($FilePath,$WindowStyle,$ArgumentList) }
function Save-State { }
function Get-Tool([string]$Name) { return $Name }
function Get-UsbDevices { return @($script:Camera,$script:Bluetooth,$script:Other) }
function Invoke-Native([string]$File,[string[]]$Arguments) {
    $script:Calls.Add([pscustomobject]@{Operation='native';Arguments=$Arguments})
}
function Invoke-Linux {
    param([string]$Operation,$Request,[switch]$AsRoot,[switch]$Capture)
    $script:Calls.Add([pscustomobject]@{Operation=$Operation;Arguments=@()})
    if ($Operation -eq 'firmware') {
        Assert ($AsRoot -and $Capture) 'Firmware setup must run as root and return structured status.'
        Assert ($Request.bluetooth_usb_id -eq 'abcd:0123') 'The selected USB identity was not forwarded to Linux.'
        $value = $script:Reports[$script:ReportIndex]
        $script:ReportIndex++
        return ($value | ConvertTo-Json -Depth 5 -Compress)
    }
    if ($Operation -eq 'prepare') {
        $script:PrepareCount++
        if ($script:PrepareCount -le $script:FailPrepares) { throw 'fixture prepare failure' }
    }
}
function Report([bool]$Ready,[bool]$Changed=$false,[string[]]$Files=@()) {
    return @{enabled=$true;ready=$Ready;changed=$Changed;repaired_files=$Files;messages=@()}
}
function Reset-Fixture($Reports,[int]$FailPrepares=0) {
    $script:Calls.Clear()
    $script:Reports = @($Reports)
    $script:ReportIndex = 0
    $script:PrepareCount = 0
    $script:FailPrepares = $FailPrepares
    $script:Settings['XR_USB_IDS'] = '1234:5678'
    $script:Settings['XR_USB_INSTANCE_IDS'] = ''
    $script:Settings['XR_BLUETOOTH'] = '1'
    $script:State.UsbInstanceIds = @($script:Camera.InstanceId)
    $script:State.BluetoothInstanceId = $script:Bluetooth.InstanceId
    $script:UsbBusId = @()
    $script:BluetoothBusId = $script:Bluetooth.BusId
    $script:WithoutBluetooth = $false
}

Reset-Fixture @((Report $true))
Connect-SelectedUsb
Assert ($script:PrepareCount -eq 1) 'Normal preparation did not run.'
Assert (@($script:Calls | Where-Object { $_.Operation -eq 'native' }).Count -eq 0) 'A healthy attached controller was reset.'

Reset-Fixture @((Report $false $true))
Connect-SelectedUsb
$native = @($script:Calls | Where-Object { $_.Operation -eq 'native' })
Assert ($native.Count -eq 2) 'A failed attached dongle was not reconnected once.'
Assert (($native[0].Arguments -join ' ') -eq 'detach --busid 2-7') 'The wrong device was detached.'
Assert (($native[1].Arguments -join ' ') -eq 'attach --wsl --busid 2-7') 'The selected dongle was not reattached.'
Assert ($script:Calls[0].Operation -eq 'firmware') 'USB attach ran before firmware was exposed to the kernel.'

Reset-Fixture @((Report $true),(Report $false $false @('vendor_x/controller.bin'))) 1
Connect-SelectedUsb
Assert ($script:PrepareCount -eq 2) 'A newly discovered compressed firmware did not get one retry.'
$native = @($script:Calls | Where-Object { $_.Operation -eq 'native' })
Assert ($native.Count -eq 2 -and $native[0].Arguments[-1] -eq '2-7' -and $native[1].Arguments[-1] -eq '2-7') 'Firmware recovery touched unrelated USB devices.'

Reset-Fixture @((Report $true),(Report $false)) 1
$rejected = $false
try { Connect-SelectedUsb } catch { $rejected = $_.Exception.Message.Contains('fixture prepare failure') }
Assert $rejected 'An unrelated preparation failure was masked as firmware recovery.'
Assert (@($script:Calls | Where-Object { $_.Operation -eq 'native' }).Count -eq 0) 'An unrelated failure reset USB.'

Reset-Fixture @((Report $true),(Report $false $false @('vendor_x/controller.bin'))) 2
$rejected = $false
try { Connect-SelectedUsb } catch { $rejected = $_.Exception.Message.Contains('fixture prepare failure') }
Assert ($rejected -and $script:PrepareCount -eq 2 -and $script:ReportIndex -eq 2) 'A persistent failure retried indefinitely.'

Reset-Fixture @()
$script:State.BluetoothInstanceId = ''
$script:BluetoothBusId = ''
$script:WithoutBluetooth = $true
$script:Settings['XR_BLUETOOTH'] = '0'
Connect-SelectedUsb
Assert ($script:ReportIndex -eq 0 -and $script:PrepareCount -eq 1) 'Disabled Bluetooth still requested firmware setup.'
'OK: firmware before attach, selected-dongle recovery, healthy/unrelated devices, one retry and disabled Bluetooth'
