param(
  [string]$Root = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$VrPathReg = "",
  [string]$Device = $(if ($env:XR_OPENVR_DEVICE) { $env:XR_OPENVR_DEVICE } elseif ($env:XR_TARGET_DEVICE) { $env:XR_TARGET_DEVICE } else { "generic" }),
  [string]$DriverPackage = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
$Device = $Device.ToLowerInvariant().Replace("-", "_")
if ($Device -eq "none") { $Device = "generic" }
if ($Device -eq "xreal_air2ultra") { $Device = "xreal_ultra" }
if ($Device -notmatch '^[a-z0-9][a-z0-9_.]*$') { throw "Invalid device profile: $Device" }

if ([string]::IsNullOrWhiteSpace($VrPathReg)) {
  $candidates = @(
    "$env:ProgramFiles(x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe",
    "$env:ProgramFiles\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $VrPathReg = $c; break } }
}
if (!(Test-Path $VrPathReg)) { throw "vrpathreg.exe not found; pass -VrPathReg" }

if ([string]::IsNullOrWhiteSpace($DriverPackage)) {
  $BuildSuffix = if ($Device -eq "generic" -or $Device -eq "xreal_ultra") { "windows_$BuildType" } else { "windows_${Device}_$BuildType" }
  $DriverPackage = Join-Path $Root "build\drivers\openvr_driver\$BuildSuffix\xr_tracking"
}
if (!(Test-Path (Join-Path $DriverPackage "driver.vrdrivermanifest"))) { throw "Driver package not found: $DriverPackage. Run build_driver.ps1 first." }
& $VrPathReg adddriver $DriverPackage
& $VrPathReg show
