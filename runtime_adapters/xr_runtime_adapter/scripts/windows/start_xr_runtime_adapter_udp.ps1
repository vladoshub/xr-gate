param(
  [string]$Root = "",
  [string]$InputHost = "127.0.0.1",
  [int]$InputPort = 45670,
  [int]$TickRateHz = 90,
  [double]$PredictionMs = 15.0,
  [switch]$WithRuntimeHandUdp,
  [switch]$WithRuntimeControllerStateUdp,
  [switch]$WithControllerInputTcp,
  [int]$ControllerInputTcpPort = 45672
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
$Exe = Join-Path $Root "bin\runtime_adapters\xr_runtime_adapter\xr_runtime_adapter.exe"
if (!(Test-Path $Exe)) { throw "xr_runtime_adapter.exe not found: $Exe. Run build_xr_runtime_adapter.ps1 first." }

$args = @(
  "--mode", "tick",
  "--tick-rate-hz", "$TickRateHz",
  "--prediction-ms", "$PredictionMs",
  "--input", "udp",
  "--udp-bind-host", $InputHost,
  "--udp-bind-port", "$InputPort",
  "--publish-runtime-pose-udp",
  "--runtime-pose-udp-host", "127.0.0.1",
  "--runtime-pose-udp-port", "45800"
)
if ($WithRuntimeHandUdp) {
  $args += @("--publish-runtime-hand-udp", "--runtime-hand-udp-host", "127.0.0.1", "--runtime-hand-udp-port", "45801")
}
if ($WithRuntimeControllerStateUdp) {
  $args += @("--publish-runtime-controller-state-udp", "--runtime-controller-state-udp-host", "127.0.0.1", "--runtime-controller-state-udp-port", "45802")
}
if ($WithControllerInputTcp) {
  $args += @("--controller-input-transport", "tcp", "--controller-input-host", "127.0.0.1", "--controller-input-port", "$ControllerInputTcpPort", "--publish-runtime-controller-state-udp", "--runtime-controller-state-udp-host", "127.0.0.1", "--runtime-controller-state-udp-port", "45802")
}

& $Exe @args
