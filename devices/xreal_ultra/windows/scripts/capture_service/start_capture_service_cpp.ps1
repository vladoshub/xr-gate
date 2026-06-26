param(
  [string]$Root = "",
  [ValidateSet("tcp")]
  [string]$Publish = "tcp",
  [string]$RegistryPath = "capture_service_streams_windows.json",
  [string]$Namespace = "xreal_air2ultra_windows",
  [string]$TcpBindHost = "",
  [int]$TcpPort = 0,
  [string]$NetworkConfig = "",
  [int]$CameraIndex = 0,
  [string]$CameraApi = "msmf",
  [switch]$NoCamera,
  [switch]$NoImu,
  [int]$Duration = 0
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = (Resolve-Path (Join-Path $ScriptDir "..\..\..\..\..")).Path
} else {
  $Root = (Resolve-Path $Root).Path
}
$env:ROOT_PROJECT = $Root
$env:XR_ROOT_PROJECT = $Root
$ScriptsRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
. (Join-Path $ScriptsRoot "xreal_ultra_out_env.ps1")
. (Join-Path $ScriptsRoot "load_windows_wsl2_network_env.ps1")
$null = Import-XrWindowsNetworkEnv -Root $Root -NetworkConfig $NetworkConfig
if ([string]::IsNullOrWhiteSpace($TcpBindHost)) { $TcpBindHost = Get-XrEnvOrDefault "XR_CAPTURE_TCP_BIND_HOST" "127.0.0.1" }
if ($TcpPort -le 0) { $TcpPort = [int](Get-XrEnvOrDefault "XR_CAPTURE_TCP_PORT" "45660") }

$Candidates = @(
  (Join-Path $env:XR_OUT_BIN_ROOT "capture_service_cpp\capture_service_cpp.exe"),
  (Join-Path $Root "bin\capture_service_cpp\capture_service_cpp.exe")
)
$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
  throw "capture_service_cpp.exe not found. Run devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 first. Tried: $($Candidates -join ', ')"
}

$args = @(
  "--publish", $Publish,
  "--registry", $RegistryPath,
  "--namespace", $Namespace,
  "--tcp-bind", $TcpBindHost,
  "--tcp-port", "$TcpPort",
  "--camera-index", "$CameraIndex",
  "--camera-api", $CameraApi
)
if ($NoCamera) { $args += "--no-camera" }
if ($NoImu) { $args += "--no-imu" }
if ($Duration -gt 0) { $args += @("--duration", "$Duration") }

Write-Host "[start_capture_service_cpp_windows] $Exe $($args -join ' ')"
& $Exe @args
exit $LASTEXITCODE
