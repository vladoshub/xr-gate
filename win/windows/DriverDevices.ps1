function Get-XrDriverDeviceId {
    $device = (Get-XrSetting 'XR_TARGET_DEVICE').Trim().ToLowerInvariant().Replace('-', '_')
    if (!$device -or $device -cnotmatch '^[a-z0-9][a-z0-9_.]*$' -or $device.EndsWith('.')) {
        throw 'Set XR_TARGET_DEVICE to the driver device identifier in the selected Hardware preset.'
    }
    return $device
}
function Test-XrDriverForSelectedDevice([string]$Package) {
    if (!$Package) { return $false }
    $settings = Join-Path $Package 'resources/settings/default.vrsettings'
    if (!(Test-Path -LiteralPath $settings -PathType Leaf -ErrorAction SilentlyContinue)) { return $false }
    $configuration = Get-XrDriverConfiguration $settings
    return $configuration.Device -ceq (Get-XrDriverDeviceId)
}
function Get-XrDeviceDriverVariants([string]$Root) {
    $manifestPath = Join-Path $Root 'bundle.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Windows driver bundle.json is missing.' }
    $manifest = Get-Content -Encoding UTF8 -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ([int]$manifest.schema -notin @(1, 2)) { throw 'Unsupported Windows bundle format.' }
    $device = Get-XrDriverDeviceId
    $found = @()
    $availableDevices = @()
    foreach ($entry in @($manifest.variants)) {
        $hz = [int]$entry.frequency_hz
        $mode = [string]$entry.display_mode
        if ($hz -lt 1 -or $hz -gt 1000 -or $mode -cnotin @('direct', 'extended_sbs')) {
            throw 'Invalid driver frequency/display mode in bundle.json.'
        }
        if ([int]$manifest.schema -eq 2) {
            $entryDevice = [string]$entry.device
            $availableDevices += $entryDevice
            if ($entryDevice -cne $device) { continue }
            $relative = 'drivers/' + $device + '/' + $hz + 'Hz-' + $mode + '/xr_tracking'
        } else {
            $relative = 'drivers/' + $hz + 'Hz-' + $mode + '/xr_tracking'
        }
        if ([string]$entry.path -cne $relative) { throw "Invalid driver path in bundle.json: $($entry.path)" }
        $package = Join-Path $Root $relative
        $configuration = Get-XrDriverConfiguration (Join-Path $package 'resources/settings/default.vrsettings')
        $availableDevices += $configuration.Device
        if ($configuration.Device -cne $device) {
            if ([int]$manifest.schema -eq 2) { throw "Driver device profile does not match folder '$device': $relative" }
            continue
        }
        if ($configuration.FrequencyHz -ne $hz -or $configuration.OnDesktop -ne ($mode -eq 'extended_sbs')) {
            throw "Driver settings do not match bundle.json: $relative"
        }
        $found += [pscustomobject]@{device=$device; frequency_hz=$hz; display_mode=$mode; path=$relative}
    }
    if (!$found.Count) {
        $available = @($availableDevices | Where-Object { $_ } | Sort-Object -Unique) -join ', '
        throw "No SteamVR driver for device '$device'. Available driver devices: $available. Build/download a Windows release with this device profile."
    }
    return $found
}
function Get-XrSelectedDriverPackage([string]$Root) {
    $variants = @(Get-XrDeviceDriverVariants $Root)
    $variant = @($variants | Where-Object {
        [int]$_.frequency_hz -eq [int]$script:State.FrequencyHz -and $_.display_mode -eq $script:State.DisplayMode
    })
    if ($variant.Count -ne 1) {
        $choices = @($variants | ForEach-Object { "$($_.frequency_hz) Hz / $($_.display_mode)" }) -join ', '
        throw "Device '$(Get-XrDriverDeviceId)' contains: $choices. Select an included display mode and refresh rate."
    }
    $package = Join-Path $Root ([string]$variant[0].path)
    foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        if (!(Test-Path -LiteralPath (Join-Path $package $name) -PathType Leaf)) { throw "Windows bundle file missing: $($variant[0].path)/$name" }
    }
    return $package
}
