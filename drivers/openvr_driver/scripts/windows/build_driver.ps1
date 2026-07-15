param(
  [string]$Root = "",
  [string]$OpenVrSdkRoot = $env:XR_OPENVR_SDK_ROOT,
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Ninja",
  [string]$Device = $(if ($env:XR_OPENVR_DEVICE) { $env:XR_OPENVR_DEVICE } elseif ($env:XR_TARGET_DEVICE) { $env:XR_TARGET_DEVICE } else { "generic" }),
  [string]$DeviceSettings = $env:XR_OPENVR_DEVICE_SETTINGS,
  [double]$DisplayFrequency = $(if ($env:XR_OPENVR_DISPLAY_FREQUENCY_HZ) { [double]$env:XR_OPENVR_DISPLAY_FREQUENCY_HZ } else { 60.0 }),
  [ValidateSet("direct", "extended_sbs")]
  [string]$DisplayMode = $(if ($env:XR_OPENVR_DISPLAY_MODE) { $env:XR_OPENVR_DISPLAY_MODE } else { "direct" })
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
if ([string]::IsNullOrWhiteSpace($OpenVrSdkRoot)) { throw "Set -OpenVrSdkRoot or XR_OPENVR_SDK_ROOT to Valve OpenVR SDK root" }
if ($DisplayFrequency -lt 1.0 -or $DisplayFrequency -gt 1000.0) { throw "DisplayFrequency must be in range 1..1000" }

$Device = $Device.ToLowerInvariant().Replace("-", "_")
if ($Device -eq "none") { $Device = "generic" }
if ($Device -eq "xreal_air2ultra") { $Device = "xreal_ultra" }
if ($Device -notmatch '^[a-z0-9][a-z0-9_.]*$') { throw "Invalid device profile: $Device" }

$DriverDir = Join-Path $Root "drivers\openvr_driver"
if ([string]::IsNullOrWhiteSpace($DeviceSettings) -and $Device -ne "generic") {
  $candidate = Join-Path $DriverDir "devices\$Device\settings\default.vrsettings"
  if (!(Test-Path $candidate)) { throw "Device settings overlay not found: $candidate" }
  $DeviceSettings = $candidate
}
if (![string]::IsNullOrWhiteSpace($DeviceSettings)) {
  $DeviceSettings = (Resolve-Path $DeviceSettings).Path
}

$BuildSuffix = if ($Device -eq "generic" -or $Device -eq "xreal_ultra") { "windows_$BuildType" } else { "windows_${Device}_$BuildType" }
$BuildDir = Join-Path $Root "build\drivers\openvr_driver\$BuildSuffix"
cmake -S $DriverDir -B $BuildDir -G $Generator -DCMAKE_BUILD_TYPE=$BuildType -DXR_OPENVR_SDK_ROOT=$OpenVrSdkRoot
cmake --build $BuildDir --config $BuildType

$SettingsPath = Join-Path $BuildDir "xr_tracking\resources\settings\default.vrsettings"
if (!(Test-Path $SettingsPath)) { throw "Built package settings not found: $SettingsPath" }
$Renderer = Join-Path $DriverDir "scripts\render_display_settings.py"
$RenderArgs = @(
  $Renderer,
  "--settings", $SettingsPath,
  "--device-profile", $Device,
  "--display-frequency", $DisplayFrequency.ToString([Globalization.CultureInfo]::InvariantCulture),
  "--display-mode", $DisplayMode
)
if (![string]::IsNullOrWhiteSpace($DeviceSettings)) {
  $RenderArgs += @("--device-settings", $DeviceSettings)
}
python @RenderArgs
if ($LASTEXITCODE -ne 0) { throw "render_display_settings.py failed with exit code $LASTEXITCODE" }

Write-Host "[build_openvr_driver] device: $Device"
Write-Host "[build_openvr_driver] frequency: $DisplayFrequency"
Write-Host "[build_openvr_driver] mode: $DisplayMode"
Write-Host "[build_openvr_driver] package: $(Join-Path $BuildDir 'xr_tracking')"
