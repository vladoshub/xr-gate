# Driver selection shared by the GUI and command-line installer.
$script:ResolvedDriverRoot = ''
$script:WindowsBundleNames = @('xr-gate-windows-wsl2-usbip.zip', 'xr-gate-windows-wsl-usbip.zip', 'xr-gate-windows.zip')
. (Join-Path $PSScriptRoot 'DriverDevices.ps1')
function Get-XrWindowsBundleName($Release) {
    foreach ($name in $script:WindowsBundleNames) {
        if (@($Release.assets | Where-Object { $_.name -ceq $name }).Count) { return $name }
    }
    return $null
}
function Get-XrDriverConfiguration([string]$Path) {
    # Driver JSON deliberately contains debugMode and DebugMode. PS 5.1's
    # PSCustomObject conversion cannot represent both names; use a dictionary.
    $text = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $data = ConvertFrom-Json -InputObject $text -AsHashtable
    } else {
        Add-Type -AssemblyName System.Web.Extensions
        $parser = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $data = $parser.DeserializeObject($text)
    }
    if ($data -isnot [Collections.IDictionary] -or !$data.ContainsKey('xr_tracking')) {
        throw 'The installed driver settings have no xr_tracking section. Run Install.'
    }
    $current = $data['xr_tracking']
    if ($current -isnot [Collections.IDictionary] -or !$current.ContainsKey('displayFrequency') -or
        !$current.ContainsKey('isDisplayOnDesktop') -or $current['isDisplayOnDesktop'] -isnot [bool]) {
        throw 'The installed driver frequency/display settings are invalid. Run Install.'
    }
    $device = if ($current.ContainsKey('deviceProfile')) { [string]$current['deviceProfile'] } else { '' }
    return @{FrequencyHz=[int]$current['displayFrequency']; OnDesktop=[bool]$current['isDisplayOnDesktop']; Device=$device}
}
function Test-XrDriverRegistration([string]$DriverPath) {
    if (!$DriverPath) { return $false }
    $file = [IO.Path]::Combine($env:LOCALAPPDATA, 'openvr/openvrpaths.vrpath')
    if (!(Test-Path -LiteralPath $file -PathType Leaf -ErrorAction SilentlyContinue)) { return $false }
    $paths = Get-Content -Encoding UTF8 -Raw -LiteralPath $file | ConvertFrom-Json
    if (!$paths.PSObject.Properties['external_drivers']) { return $false }
    $selected = [IO.Path]::GetFullPath($DriverPath).TrimEnd([char[]]@('\','/'))
    foreach ($path in @($paths.external_drivers)) {
        if (!$path) { continue }
        try { $candidate = [IO.Path]::GetFullPath([string]$path).TrimEnd([char[]]@('\','/')) }
        catch { continue }
        if ([string]::Equals($selected, $candidate, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}
function Ensure-XrDriverRegistration {
    if (!(Test-XrDriverRegistration ([string]$script:State.DriverPath))) { Register-XrDriver }
    else {
        $script:State.RegisteredDriver = $script:State.DriverPath
        Save-State
    }
}
function Find-AutomaticXrDisplay([object[]]$Displays) {
    if ($DisplayName) { return Select-DesktopDisplay -Displays $Displays -Name $DisplayName }
    if ($script:State.DisplayDeviceId) {
        $saved = @($Displays | Where-Object { $_.DeviceId -eq $script:State.DisplayDeviceId })
        if ($saved.Count -eq 1) {
            if (@($Displays | Where-Object { $_.DeviceName -eq $saved[0].DeviceName }).Count -eq 1) { return $saved[0] }
            return $null
        }
    }
    $label = Get-XrSetting 'XR_DISPLAY_MATCH'
    $candidates = @()
    if ($label) {
        $candidates = @($Displays | Where-Object {
            $_.Name.IndexOf($label, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $_.DeviceId.IndexOf($label, [StringComparison]::OrdinalIgnoreCase) -ge 0
        })
    }
    # Resolution alone does not identify hardware: another monitor may match.
    if ($candidates.Count -eq 1 -and
        @($Displays | Where-Object { $_.DeviceName -eq $candidates[0].DeviceName }).Count -eq 1) {
        return $candidates[0]
    }
    return $null
}
function Get-XrDisplayObservation {
    $observation = @{frequency_hz=$null; display_name=''; message=''}
    try {
        $display = Find-AutomaticXrDisplay @(Get-DesktopDisplays)
        if ($display -and [int]$display.Frequency -gt 1) {
            $observation.frequency_hz = [int]$display.Frequency
            $observation.display_name = [string]$display.DeviceName
        }
    } catch { $observation.message = $_.Exception.Message }
    return $observation
}
function Resolve-DriverFrequency([int[]]$Available, [int]$Detected = 0) {
    # Windows commonly reports 59 instead of 60, or 89 instead of 90.
    $matches = @($Available | Where-Object { $Detected -gt 0 -and [Math]::Abs($_ - $Detected) -le 1 } |
        Sort-Object { [Math]::Abs($_ - $Detected) })
    if ($matches.Count) { return @{Frequency=[int]$matches[0]; Fallback=$false} }
    if ($Available -notcontains 60) { throw 'This package has no matching driver and no 60 Hz fallback. Download a Windows package that includes 60 Hz.' }
    return @{Frequency=60; Fallback=$true}
}
function Get-AutomaticDriverBundle($Release, [string]$Cache) {
    $roots = @($PSScriptRoot | Split-Path -Parent)
    if (Test-XrCustomRuntimeSource $Release) { return Get-XrCustomSourceDriverRoot $roots[0] }
    # A complete local Windows package is authoritative even when its pinned
    # Ubuntu runtime tag has no Windows archive (or still uses the old name).
    $localManifest = Join-Path $roots[0] 'bundle.json'
    if (Test-Path -LiteralPath $localManifest -PathType Leaf) {
        $localBundle = Get-Content -Encoding UTF8 -Raw -LiteralPath $localManifest | ConvertFrom-Json
        if ($localBundle.schema -in @(1, 2) -and ($localBundle.release_tag -eq 'latest' -or $localBundle.release_tag -eq $Release.tag_name)) { return $roots[0] }
    }
    if (Test-Path -LiteralPath $Cache) {
        if ($Release.assets.Count) {
            # A reinstall must honor the current asset digest even when an asset
            # was replaced under the same release tag.
            $bundleName = Get-XrWindowsBundleName $Release
            if (!$bundleName) { throw ('Release ' + $Release.tag_name + ' contains no Windows driver bundle. Download the complete Windows package.') }
            $currentAsset = Select-ReleaseAsset $Release $bundleName
            $directoryName = [IO.Path]::GetFileNameWithoutExtension($bundleName)
            $roots += Join-Path $Cache ('windows-' + ([string]$currentAsset.digest).Substring(7,12) + '/' + $directoryName)
        } else {
            # Start VR can use the last installed bundle without a network call.
            $cached = @(Get-ChildItem -LiteralPath $Cache -Directory -Filter 'windows-*' | Sort-Object LastWriteTime -Descending)
            foreach ($bundleName in $script:WindowsBundleNames) {
                $directoryName = [IO.Path]::GetFileNameWithoutExtension($bundleName)
                $roots += @($cached | ForEach-Object { Join-Path $_.FullName $directoryName })
            }
        }
    }
    foreach ($root in $roots) {
        $manifest = Join-Path $root 'bundle.json'
        if (!(Test-Path -LiteralPath $manifest)) { continue }
        $bundle = Get-Content -Encoding UTF8 -Raw -LiteralPath $manifest | ConvertFrom-Json
        if ($bundle.schema -in @(1, 2) -and ($bundle.release_tag -eq 'latest' -or $bundle.release_tag -eq $Release.tag_name)) {
            return $root
        }
    }
    if (!$Release.assets.Count) { $Release = Get-ReleaseInfo ([string]$Release.tag_name) }
    $bundleName = Get-XrWindowsBundleName $Release
    if (!$bundleName) { throw ('Release ' + $Release.tag_name + ' contains no Windows driver bundle. Download the complete Windows package.') }
    $asset = Select-ReleaseAsset $Release $bundleName
    $archive = Save-ReleaseAsset $asset $Cache
    $directory = Join-Path $Cache ('windows-' + ([string]$asset.digest).Substring(7,12))
    $root = Join-Path $directory ([IO.Path]::GetFileNameWithoutExtension($bundleName))
    if (!(Test-Path -LiteralPath (Join-Path $root 'bundle.json'))) {
        Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
    }
    return $root
}
function Select-AutomaticDriverFrequency($Release, [string]$Cache) {
    $script:ResolvedDriverRoot = Get-AutomaticDriverBundle $Release $Cache
    $variants = @(Get-XrDeviceDriverVariants $script:ResolvedDriverRoot)
    $available = @($variants | Where-Object { $_.display_mode -eq $script:State.DisplayMode } |
        ForEach-Object { [int]$_.frequency_hz })
    $display = Find-AutomaticXrDisplay @(Get-DesktopDisplays)
    $detected = if ($display) { [int]$display.Frequency } else { 0 }
    $selection = Resolve-DriverFrequency -Available $available -Detected $detected
    if ($selection.Fallback) {
        $reason = if ($display) { 'The glasses report ' + $detected + ' Hz; no matching driver is included.' }
                  else { 'The glasses refresh rate could not be identified safely.' }
        Write-XrWarning ($reason + ' Using the 60 Hz driver.')
        if ($display -and [Math]::Abs($detected - 60) -gt 1) {
            Assert-SteamVrStopped
            if (!('XrGateWsl.DisplayNative' -as [type])) { $null = Get-DesktopDisplays }
            [XrGateWsl.DisplayNative]::SetFrequency([string]$display.DeviceName, 60)
            Write-XrWarning ('Changed only the selected glasses output ' + $display.DeviceName + ' to 60 Hz.')
        }
    }
    $script:State.FrequencyHz = [int]$selection.Frequency
    if ($display) {
        $script:State.DisplayDeviceId = [string]$display.DeviceId
        $script:State.DisplayDeviceName = [string]$display.DeviceName
    }
    Write-Step ('Automatic Windows driver: ' + (Get-XrDriverDeviceId) + ' / ' + $script:State.FrequencyHz + ' Hz / ' + $script:State.DisplayMode)
}
function Install-SelectedWindowsDriver([string]$Package, [string]$Tag) {
    $device = Get-XrDriverDeviceId
    if (!(Test-XrDriverForSelectedDevice $Package)) { throw "The Windows driver does not match selected device '$device'." }
    $dllHash = (Get-FileHash -LiteralPath (Join-Path $Package 'bin/win64/driver_xr_tracking.dll') -Algorithm SHA256).Hash.Substring(0,12)
    $settingsHash = (Get-FileHash -LiteralPath (Join-Path $Package 'resources/settings/default.vrsettings') -Algorithm SHA256).Hash.Substring(0,12)
    $directory = Join-Path $script:StateDir ('drivers\' + $device + '\' + $Tag + '-' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '-' + $dllHash + $settingsHash)
    $target = Join-Path $directory 'xr_tracking'
    if (!(Test-Path -LiteralPath (Join-Path $target 'driver.vrdrivermanifest'))) {
        New-Item -ItemType Directory -Force $directory | Out-Null
        $staging = Join-Path $directory ('.staging-' + [Guid]::NewGuid().ToString('N'))
        try {
            Copy-Item -LiteralPath $Package -Destination $staging -Recurse
            Move-Item -LiteralPath $staging -Destination $target
        } finally {
            if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
        }
    }
    $script:State.DriverPath = $target
    $script:State.DriverDevice = $device
    $script:State.ReleaseTag = $Tag
    Save-State
}
function Update-AutomaticWindowsDriver {
    if (!$script:State.ReleaseTag) { throw 'Run Install before starting VR.' }
    $tag = [string]$script:State.ReleaseTag
    $release = [pscustomobject]@{tag_name=$tag; assets=@(); xr_runtime_source=[string]($script:State['RuntimeSourceKind'])}
    $cache = Join-Path $script:StateDir ('cache\' + $tag)
    Select-AutomaticDriverFrequency $release $cache
    $settings = Join-Path $script:State.DriverPath 'resources/settings/default.vrsettings'
    $matches = $false
    if (Test-Path -LiteralPath $settings) {
        $current = Get-XrDriverConfiguration $settings
        $matches = ($current.FrequencyHz -eq [int]$script:State.FrequencyHz -and
                    $current.OnDesktop -eq ($script:State.DisplayMode -eq 'extended_sbs') -and
                    $current.Device -ceq (Get-XrDriverDeviceId))
    }
    if (!$matches) {
        Assert-SteamVrStopped
        $package = Find-BundledDriver $script:ResolvedDriverRoot
        Install-SelectedWindowsDriver $package $tag
    }
    Save-State
    Ensure-XrDriverRegistration
}
