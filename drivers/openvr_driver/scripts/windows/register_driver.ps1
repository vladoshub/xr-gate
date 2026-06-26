param(
  [string]$Root = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$VrPathReg = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
if ([string]::IsNullOrWhiteSpace($VrPathReg)) {
  $candidates = @(
    "$env:ProgramFiles(x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe",
    "$env:ProgramFiles\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $VrPathReg = $c; break } }
}
if (!(Test-Path $VrPathReg)) { throw "vrpathreg.exe not found; pass -VrPathReg" }
$DriverPackage = Join-Path $Root "build\drivers\openvr_driver\windows_$BuildType\xr_tracking"
if (!(Test-Path (Join-Path $DriverPackage "driver.vrdrivermanifest"))) { throw "Driver package not found: $DriverPackage. Run build_driver.ps1 first." }
& $VrPathReg adddriver $DriverPackage
& $VrPathReg show
