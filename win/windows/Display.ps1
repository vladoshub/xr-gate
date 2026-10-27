function Get-XrNativeDesktopDisplays {
    if (!('XrGateWsl.DisplayNative' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'DisplayNative.cs')
    }
    return [XrGateWsl.DisplayNative]::Enumerate()
}
function Get-DesktopDisplays {
    $displays = @(Get-XrNativeDesktopDisplays)
    if (Get-XrSetting 'XR_DISPLAY_MODEL_NAME') {
        # DeviceString is frequently "Generic PnP Monitor"; use the same EDID
        # friendly name exposed by WmiMonitorID, keyed to the exact monitor instance.
        $names = @{}
        foreach ($monitor in @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop | Where-Object Active)) {
            $key = Get-XrMonitorIdentityKey ([string]$monitor.InstanceName) -Wmi
            if (!$key) { continue }
            $name = -join ($monitor.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
            if ($names.ContainsKey($key)) { $names[$key] = '' } else { $names[$key] = $name.Trim() }
        }
        foreach ($display in $displays) {
            $key = Get-XrMonitorIdentityKey ([string]$display.DeviceId)
            $name = if ($key -and $names.ContainsKey($key)) { [string]$names[$key] } else { '' }
            $display | Add-Member -NotePropertyName EdidName -NotePropertyValue $name -Force
            if ($name) { $display.Name = $name }
        }
    }
    return $displays
}
function Get-XrMonitorIdentityKey([string]$DeviceId, [switch]$Wmi) {
    $path = $DeviceId.Replace('#', '\') -replace '^\\\\\?\\', ''
    if ($path -notmatch '^DISPLAY\\([^\\]+)\\([^\\]+)') { return '' }
    $model = $Matches[1]; $instance = $Matches[2]
    if ($Wmi) { $instance = $instance -replace '_\d+$', '' }
    return ($model + '\' + $instance).ToUpperInvariant()
}
function Test-XrDisplayIdentity($Display) {
    $model = (Get-XrSetting 'XR_DISPLAY_MODEL_ID').Trim()
    $name = (Get-XrSetting 'XR_DISPLAY_MODEL_NAME').Trim()
    if ($model) {
        $key = Get-XrMonitorIdentityKey ([string]$Display.DeviceId)
        if (!$key -or ($key -split '\\')[0] -ine $model) { return $false }
    }
    if ($name) {
        if (!$Display.PSObject.Properties['EdidName'] -or ([string]$Display.EdidName).Trim() -ine $name) { return $false }
    }
    return $true
}
function Get-XrSbsObservation($Display) {
    $resolution = (Get-XrSetting 'SBS_RESOLUTION').Trim()
    $required = [bool]$resolution
    $enabled = if ($required) { $null } else { $true }
    $width = $null; $height = $null
    if ($resolution -and $resolution -notmatch '^([1-9][0-9]{0,4})[xX]([1-9][0-9]{0,4})$') {
        throw 'SBS_RESOLUTION must be WIDTHxHEIGHT, for example 3840x1080, or empty.'
    }
    if ($required) { $expectedWidth = [int]$Matches[1]; $expectedHeight = [int]$Matches[2] }
    if ($null -ne $Display -and $Display.PSObject.Properties['Width'] -and $Display.PSObject.Properties['Height'] -and
        [int]$Display.Width -gt 0 -and [int]$Display.Height -gt 0) {
        $width = [int]$Display.Width; $height = [int]$Display.Height
        if ($required) { $enabled = ($width -eq $expectedWidth -and $height -eq $expectedHeight) }
    }
    return @{sbs_required=$required; sbs_enabled=$enabled; sbs_resolution=$resolution; display_width=$width; display_height=$height}
}
function Assert-XrSbsEnabled {
    $resolution = (Get-XrSetting 'SBS_RESOLUTION').Trim()
    if (!$resolution) { return }
    $display = Find-AutomaticXrDisplay @(Get-DesktopDisplays)
    $mode = Get-XrSbsObservation $display
    if (!$display) { throw 'Select and connect the configured XR display before starting VR.' }
    if ($null -eq $mode.sbs_enabled) { throw 'The selected XR display resolution is unknown. Refresh its status before starting VR.' }
    if ($mode.sbs_enabled -ne $true) {
        throw ('SBS is off. Enable SBS on the device and select ' + $mode.sbs_resolution + ' in Windows Display settings. Current: ' + $mode.display_width + 'x' + $mode.display_height + '.')
    }
}
function Select-DesktopDisplay([object[]]$Displays, [string]$Name = '', [string]$SavedId = '') {
    if ($Name) {
        $matches = @($Displays | Where-Object { $_.DeviceName -eq $Name })
    } elseif ($SavedId) {
        $matches = @($Displays | Where-Object { $_.DeviceId -eq $SavedId })
    } else { throw 'Click Select glasses display first.' }
    if ($matches.Count -ne 1) {
        throw 'The selected display is missing or duplicated. Use Extend in Windows, enable SBS, then click Select glasses display.'
    }
    # Cloned monitors share the same GDI device; an independent output is needed.
    if (@($Displays | Where-Object { $_.DeviceName -eq $matches[0].DeviceName }).Count -ne 1) {
        throw 'Use Extend these displays in Windows instead of Duplicate, then click Select glasses display.'
    }
    if (!(Test-XrDisplayIdentity $matches[0])) { throw 'The selected display does not match this hardware profile: monitor model code and EDID name must match.' }
    return $matches[0]
}
function Assert-SbsDisplay($Display) {
    if ($Display.Width -ne (Get-XrInteger 'XR_WINDOW_WIDTH') -or $Display.Height -ne (Get-XrInteger 'XR_WINDOW_HEIGHT')) {
        throw ('Select the resolution from your hardware env in Windows Display settings, then click Select glasses display. Current: ' + $Display.Width + 'x' + $Display.Height)
    }
    if ([Math]::Abs($Display.Frequency - [int]$script:State.FrequencyHz) -gt 1) {
        throw ('Select ' + $script:State.FrequencyHz + ' Hz for the glasses in Windows, or install the matching -FrequencyHz driver.')
    }
}
function Get-SteamVrSettingsPath {
    $vpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (!(Test-Path -LiteralPath $vpath)) { throw 'Open SteamVR once, close it, then click Select glasses display.' }
    $paths = Get-Content -Encoding UTF8 -Raw -LiteralPath $vpath | ConvertFrom-Json
    if (!$paths.PSObject.Properties['config']) { throw 'OpenVR has no config path. Open SteamVR once, close it, then click Select glasses display.' }
    $folders = @($paths.config | Where-Object { $_ })
    if (!$folders.Count -or !(Test-Path -LiteralPath $folders[0] -PathType Container)) {
        throw 'The SteamVR configuration folder was not found. Open SteamVR once, close it, then click Select glasses display.'
    }
    return Join-Path $folders[0] 'steamvr.vrsettings'
}
function Assert-SteamVrStopped {
    if (Get-Process vrserver,vrcompositor -ErrorAction SilentlyContinue) {
        throw 'Close SteamVR before changing its display settings. Keep xr_client open if it is already running.'
    }
}
function Set-XrDisplaySettings([int]$X, [int]$Y, [string]$Mode) {
    Assert-SteamVrStopped
    $settings = Join-Path $script:State.DriverPath 'resources\settings\default.vrsettings'
    if (!(Test-Path -LiteralPath $settings -PathType Leaf)) { throw 'Click Install before starting VR.' }
    $steamSettings = Get-SteamVrSettingsPath
    $request = New-LinuxRequest
    $request.driver_settings = ConvertTo-WslPath $settings
    $request.steamvr_settings = (ConvertTo-WslPath (Split-Path -Parent $steamSettings)).TrimEnd('/') + '/steamvr.vrsettings'
    $request.window_x = $X
    $request.window_y = $Y
    $request.frequency_hz = [int]$script:State.FrequencyHz
    $request.display_mode = $Mode
    Invoke-Linux -Operation display -Request $request
}
function Set-SbsDisplay($Display) {
    Assert-SbsDisplay $Display
    Set-XrDisplaySettings -X ([int]$Display.X) -Y ([int]$Display.Y) -Mode extended_sbs
    $script:State.DisplayDeviceId = [string]$Display.DeviceId
    $script:State.DisplayDeviceName = [string]$Display.DeviceName
    $script:State.DisplayTargetDevice = Get-XrDriverDeviceId
    $script:State.DisplayMode = 'extended_sbs'
    Save-State
    Write-Step ('SBS output: ' + $Display.DeviceName + ', ' + $Display.Width + 'x' + $Display.Height + ' @ ' + $Display.Frequency + ' Hz, X=' + $Display.X + ', Y=' + $Display.Y)
}
function Choose-SbsDisplay([switch]$SelectOnly) {
    Assert-SteamVrStopped
    $displays = @(Get-DesktopDisplays)
    if (!$displays.Count) { throw 'No desktop monitor found. Disable Direct Mode in SteamVR, close SteamVR, enable SBS, and choose Extend in Windows.' }
    Write-Host 'Windows Settings > System > Display > Identify shows the display number in the glasses.'
    for ($i = 0; $i -lt $displays.Count; $i++) {
        $d = $displays[$i]
        Write-Host ('[{0}] {1} | {2} | {3}x{4} @ {5} Hz | X={6}, Y={7}' -f ($i + 1),$d.DeviceName,$d.Name,$d.Width,$d.Height,$d.Frequency,$d.X,$d.Y)
    }
    if ($DisplayName) {
        $selected = Select-DesktopDisplay -Displays $displays -Name $DisplayName
    } else {
        $answer = Read-Host 'Choose the list row for the glasses (not the main monitor)'
        $number = 0
        if (![int]::TryParse($answer, [ref]$number) -or $number -lt 1 -or $number -gt $displays.Count) { throw 'Invalid display selection.' }
        $selected = Select-DesktopDisplay -Displays $displays -Name $displays[$number - 1].DeviceName
    }
    if ($SelectOnly) {
        $script:State.DisplayDeviceId = [string]$selected.DeviceId
        $script:State.DisplayDeviceName = [string]$selected.DeviceName
        $script:State.DisplayTargetDevice = Get-XrDriverDeviceId
        if ([int]$selected.Frequency -gt 1) { $script:State.FrequencyHz = [int]$selected.Frequency }
        Save-State
        Write-Step ('Selected glasses display: ' + $selected.DeviceName + ' @ ' + $selected.Frequency + ' Hz.')
    } else { Set-SbsDisplay $selected }
}
function Update-SavedSbsDisplay {
    if ($DisplayName) {
        Set-SbsDisplay (Select-DesktopDisplay -Displays @(Get-DesktopDisplays) -Name $DisplayName)
        return
    }
    if (!$script:State.DisplayDeviceId) {
        if ($Launcher) { throw 'Select the glasses with Select glasses display in the launcher before starting VR.' }
        Choose-SbsDisplay; return
    }
    $display = Select-DesktopDisplay -Displays @(Get-DesktopDisplays) -SavedId ([string]$script:State.DisplayDeviceId)
    Set-SbsDisplay $display
}
