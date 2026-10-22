# Read-only USB observation: never select, bind or attach devices while polling.
function Get-XrDeviceObservation([object[]]$Devices) {
    $name = Get-XrSetting 'XR_DEVICE_NAME' (Get-XrSetting 'XR_TARGET_DEVICE' 'XR device')
    $busIds = @(Get-XrList 'XR_USB_BUS_IDS')
    $instances = @(Get-XrList 'XR_USB_INSTANCE_IDS')
    $selected = @()
    if ($busIds.Count) {
        $selected = @($Devices | Where-Object { $_.BusId -in $busIds })
    } elseif ($instances.Count) {
        $selected = @($Devices | Where-Object { $_.InstanceId -in $instances })
    } else {
        foreach ($id in @(Get-XrList 'XR_USB_IDS')) {
            if ($id -notmatch '^([a-fA-F0-9]{4}):([a-fA-F0-9]{4})$') { throw 'Invalid XR_USB_IDS in the hardware env.' }
            $pattern = '^USB\\VID_' + $Matches[1] + '&PID_' + $Matches[2] + '(?:&[^\\]+)?\\'
            $selected += @($Devices | Where-Object { $_.InstanceId -match $pattern })
        }
        $saved = @($selected | Where-Object { $_.InstanceId -in @($script:State.UsbInstanceIds) })
        if ($saved.Count) { $selected = $saved }
    }
    $state = 'NOT CONNECTED'
    if ($selected.Count) {
        $state = 'CONNECTED'
        if (@($selected | Where-Object { !$_.ClientIPAddress }).Count -eq 0) { $state = 'SHARED' }
    }
    return @{device_name=$name; device_status=$state;
        device_status_message='CONNECTED: available to Windows. SHARED: attached to WSL through USB/IP.'}
}
