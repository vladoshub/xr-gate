param(
  [string]$Root = "",
  [string]$OpenVrSdkRoot = $env:XR_OPENVR_SDK_ROOT,
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Ninja",
  [string]$Device = $(if ($env:XR_TARGET_DEVICE) { $env:XR_TARGET_DEVICE } else { "generic" })
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
if ([string]::IsNullOrWhiteSpace($OpenVrSdkRoot)) { throw "Set -OpenVrSdkRoot or XR_OPENVR_SDK_ROOT to Valve OpenVR SDK root" }

$DriverDir = Join-Path $Root "drivers\openvr_driver"
$BuildDir = Join-Path $Root "build\drivers\openvr_driver\windows_$BuildType"
cmake -S $DriverDir -B $BuildDir -G $Generator -DCMAKE_BUILD_TYPE=$BuildType -DXR_OPENVR_SDK_ROOT=$OpenVrSdkRoot
cmake --build $BuildDir --config $BuildType
Write-Host "[build_openvr_driver] device: $Device"
Write-Host "[build_openvr_driver] package: $(Join-Path $BuildDir 'xr_tracking')"
