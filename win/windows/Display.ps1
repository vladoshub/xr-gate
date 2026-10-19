function Get-DesktopDisplays {
    if (!('XrGateWsl.DisplayNative' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'DisplayNative.cs')
    }
    return @([XrGateWsl.DisplayNative]::Enumerate())
}
function Select-DesktopDisplay([object[]]$Displays, [string]$Name = '', [string]$SavedId = '') {
    if ($Name) {
        $matches = @($Displays | Where-Object { $_.DeviceName -eq $Name })
    } elseif ($SavedId) {
        $matches = @($Displays | Where-Object { $_.DeviceId -eq $SavedId })
    } else { throw 'Select the glasses with DISPLAY.cmd first.' }
    if ($matches.Count -ne 1) {
        throw 'The selected display is missing or duplicated. Use Extend in Windows, enable SBS, then run DISPLAY.cmd.'
    }
    # Cloned monitors share the same GDI device; an independent output is needed.
    if (@($Displays | Where-Object { $_.DeviceName -eq $matches[0].DeviceName }).Count -ne 1) {
        throw 'Use Extend these displays in Windows instead of Duplicate, then run DISPLAY.cmd.'
    }
    return $matches[0]
}
function Assert-SbsDisplay($Display) {
    if ($Display.Width -ne 3840 -or $Display.Height -ne 1080) {
        throw ('Select 3840x1080 for the glasses in Windows Display settings, then run DISPLAY.cmd. Current: ' + $Display.Width + 'x' + $Display.Height)
    }
    if ([Math]::Abs($Display.Frequency - [int]$script:State.FrequencyHz) -gt 1) {
        throw ('Select ' + $script:State.FrequencyHz + ' Hz for the glasses in Windows, or install the matching -FrequencyHz driver.')
    }
}
function Get-SteamVrSettingsPath {
    $vpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (!(Test-Path -LiteralPath $vpath)) { throw 'Open SteamVR once, close it, then run DISPLAY.cmd.' }
    $paths = Get-Content -Raw -LiteralPath $vpath | ConvertFrom-Json
    if (!$paths.PSObject.Properties['config']) { throw 'OpenVR has no config path. Open SteamVR once, close it, then run DISPLAY.cmd.' }
    $folders = @($paths.config | Where-Object { $_ })
    if (!$folders.Count -or !(Test-Path -LiteralPath $folders[0] -PathType Container)) {
        throw 'The SteamVR configuration folder was not found. Open SteamVR once, close it, then run DISPLAY.cmd.'
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
    if (!(Test-Path -LiteralPath $settings -PathType Leaf)) { throw 'Run INSTALL.cmd before DISPLAY.cmd.' }
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
    $script:State.DisplayMode = 'extended_sbs'
    Save-State
    Write-Step ('SBS output: ' + $Display.DeviceName + ', 3840x1080 @ ' + $Display.Frequency + ' Hz, X=' + $Display.X + ', Y=' + $Display.Y)
}
function Choose-SbsDisplay {
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
    Set-SbsDisplay $selected
}
function Update-SavedSbsDisplay {
    if (!$script:State.DisplayDeviceId) { Choose-SbsDisplay; return }
    $display = Select-DesktopDisplay -Displays @(Get-DesktopDisplays) -SavedId ([string]$script:State.DisplayDeviceId)
    Set-SbsDisplay $display
}
