function Get-XrSetupObservation([bool]$LinuxInstalled) {
    $driverReady = $false
    if ($script:State.DriverPath) {
        try {
            $driverReady = $true
            foreach ($relative in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
                if (!(Test-Path -LiteralPath ([IO.Path]::Combine($script:State.DriverPath, $relative)) -PathType Leaf -ErrorAction SilentlyContinue)) { $driverReady = $false }
            }
            if ($driverReady) { $driverReady = Test-XrDriverForSelectedDevice ([string]$script:State.DriverPath) }
        } catch { $driverReady = $false }
    }
    $registered = $false
    if ($driverReady) {
        try { $registered = Test-XrDriverRegistration ([string]$script:State.DriverPath) }
        catch { $registered = $false }
    }
    $selected = [bool]($DisplayName -or $script:State.DisplayDeviceId)
    $connected = $false
    $displayNumber = ''
    if ($selected) {
        try {
            $display = Select-DesktopDisplay -Displays @(Get-DesktopDisplays) -Name $DisplayName -SavedId ([string]$script:State.DisplayDeviceId)
            $connected = $null -ne $display
            if ($display.DeviceName -match 'DISPLAY(\d+)$') { $displayNumber = $Matches[1] }
        } catch { $connected = $false }
    }
    return @{setup_complete=($LinuxInstalled -and $driverReady -and $registered -and !$script:State.InstallPending);
        driver_ready=$driverReady; driver_registered=$registered; display_selected=$selected; display_connected=$connected;
        display_number=$displayNumber; install_pending=([bool]$script:State.InstallPending -or ($LinuxInstalled -and (!$driverReady -or !$registered)));
        install_next_step=[string]$script:State.InstallNextStep}
}
