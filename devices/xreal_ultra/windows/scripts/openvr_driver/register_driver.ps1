param(
  [string]$Root = "",
  [ValidateRange(60,120)]
  [int]$FrequencyHz = 60,
  [string]$Device = "",
  [string]$VrPathReg = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = (Resolve-Path (Join-Path $ScriptDir "..\..\..\..\..")).Path
} else {
  $Root = (Resolve-Path $Root).Path
}
if ([string]::IsNullOrWhiteSpace($Device)) {
  if (-not [string]::IsNullOrWhiteSpace($env:XR_TARGET_DEVICE)) { $Device = $env:XR_TARGET_DEVICE } else { $Device = "xreal_ultra" }
}
$env:ROOT_PROJECT = $Root
$env:XR_ROOT_PROJECT = $Root
$env:XR_TARGET_DEVICE = $Device
$env:XR_DEVICE_TARGET = $Device
$ScriptsRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
. (Join-Path $ScriptsRoot "xreal_ultra_out_env.ps1")

if ([string]::IsNullOrWhiteSpace($VrPathReg)) {
  $candidates = @(
    "$env:ProgramFiles(x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe",
    "$env:ProgramFiles\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
  )
  foreach ($c in $candidates) {
    if (Test-Path $c) { $VrPathReg = $c; break }
  }
}
if (-not (Test-Path $VrPathReg)) { throw "vrpathreg.exe not found; pass -VrPathReg" }

$DriverPackage = Join-Path $env:XR_OUT_BIN_ROOT "drivers\openvr_driver_${FrequencyHz}HZ\xr_tracking"
if (-not (Test-Path (Join-Path $DriverPackage "driver.vrdrivermanifest"))) {
  throw "Driver package not found: $DriverPackage. Run install_xreal_ultra_out.ps1 first."
}

Write-Host "[register_openvr_driver_windows] registering ${FrequencyHz}Hz package: $DriverPackage device=$Device"
$show = & $VrPathReg show 2>$null
foreach ($line in $show) {
  if ($line -match '^\s*xr_tracking\s*:\s*(.+)$') {
    $old = $Matches[1].Trim()
    if (-not [string]::IsNullOrWhiteSpace($old)) {
      & $VrPathReg removedriver $old | Out-Null
    }
  }
}
& $VrPathReg adddriver $DriverPackage
& $VrPathReg show
exit $LASTEXITCODE
