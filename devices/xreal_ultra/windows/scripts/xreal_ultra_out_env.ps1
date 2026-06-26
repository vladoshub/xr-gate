# Shared Windows build/package output environment for XREAL Ultra.
# Dot-source from device-level PowerShell scripts.

param()

$Script:XrEnvScriptDir = Split-Path -Parent $PSCommandPath
if (-not $env:ROOT_PROJECT -or [string]::IsNullOrWhiteSpace($env:ROOT_PROJECT)) {
  $env:ROOT_PROJECT = (Resolve-Path (Join-Path $Script:XrEnvScriptDir "..\..\..")).Path
}
if (-not $env:XR_ROOT_PROJECT -or [string]::IsNullOrWhiteSpace($env:XR_ROOT_PROJECT)) {
  $env:XR_ROOT_PROJECT = $env:ROOT_PROJECT
}
if (-not $env:XR_OUT_ROOT -or [string]::IsNullOrWhiteSpace($env:XR_OUT_ROOT)) {
  # Source checkout mode uses <root>\out\xreal_ultra_windows.
  # Deployed package mode already runs from <root> == out\xreal_ultra_windows.
  $CandidatePackageBin = Join-Path $env:XR_ROOT_PROJECT "bin"
  $CandidatePackageDevice = Join-Path $env:XR_ROOT_PROJECT "devices\xreal_ultra"
  if ((Test-Path $CandidatePackageBin) -and (Test-Path $CandidatePackageDevice)) {
    $env:XR_OUT_ROOT = $env:XR_ROOT_PROJECT
  } else {
    $env:XR_OUT_ROOT = Join-Path $env:XR_ROOT_PROJECT "out\xreal_ultra_windows"
  }
}
if (-not $env:XR_OUT_BIN_ROOT -or [string]::IsNullOrWhiteSpace($env:XR_OUT_BIN_ROOT)) {
  $env:XR_OUT_BIN_ROOT = Join-Path $env:XR_OUT_ROOT "bin"
}
if (-not $env:XR_OUT_DEVICE_HOME -or [string]::IsNullOrWhiteSpace($env:XR_OUT_DEVICE_HOME)) {
  $env:XR_OUT_DEVICE_HOME = Join-Path $env:XR_OUT_ROOT "devices\xreal_ultra"
}
if (-not $env:XR_OUT_SCRIPTS_ROOT -or [string]::IsNullOrWhiteSpace($env:XR_OUT_SCRIPTS_ROOT)) {
  $env:XR_OUT_SCRIPTS_ROOT = Join-Path $env:XR_OUT_DEVICE_HOME "windows\scripts"
}
if (-not $env:XR_OUT_CONFIGS_ROOT -or [string]::IsNullOrWhiteSpace($env:XR_OUT_CONFIGS_ROOT)) {
  $env:XR_OUT_CONFIGS_ROOT = Join-Path $env:XR_OUT_DEVICE_HOME "configs"
}
if (-not $env:XR_BIN_ROOT -or [string]::IsNullOrWhiteSpace($env:XR_BIN_ROOT)) {
  $env:XR_BIN_ROOT = $env:XR_OUT_BIN_ROOT
}
