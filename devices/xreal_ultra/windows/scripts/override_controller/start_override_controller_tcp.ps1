param(
  [string]$Root = "",
  [string]$Config = "",
  [string]$BindHost = "",
  [int]$Port = 0,
  [string]$NetworkConfig = "",
  [switch]$Train,
  [string]$Name = "default",
  [string]$ConfigDir = "",
  [int]$RateHz = 90
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
if ([string]::IsNullOrWhiteSpace($BindHost)) { $BindHost = Get-XrEnvOrDefault "XR_OVERRIDE_CONTROLLER_TCP_BIND_HOST" "127.0.0.1" }
if ($Port -le 0) { $Port = [int](Get-XrEnvOrDefault "XR_OVERRIDE_CONTROLLER_TCP_PORT" "45672") }

$Candidates = @(
  (Join-Path $env:XR_OUT_BIN_ROOT "override_controller\override_controller.exe"),
  (Join-Path $Root "bin\override_controller\override_controller.exe")
)
$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
  throw "override_controller.exe not found. Run devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 -BuildOnly override_controller first. Tried: $($Candidates -join ', ')"
}

$args = @(
  "--publish-transport", "tcp",
  "--publish-tcp-bind-host", $BindHost,
  "--publish-tcp-port", "$Port",
  "--publish-rate-hz", "$RateHz"
)

if (-not [string]::IsNullOrWhiteSpace($ConfigDir)) {
  $args += @("--config-dir", $ConfigDir)
}
if (-not [string]::IsNullOrWhiteSpace($Config)) {
  $args += @("--config", $Config)
}
if ($Train) {
  $args += @("--train", "--name", $Name)
}

Write-Host "[start_override_controller_tcp_windows] $Exe $($args -join ' ')"
& $Exe @args
exit $LASTEXITCODE
