param(
  [string]$Root = "",
  [string]$Distro = "",
  [string]$WslProjectRoot = "",
  [string]$WslOutName = "",
  [string]$WslRuntimeRoot = "",
  [string]$NetworkConfig = ""
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

if ([string]::IsNullOrWhiteSpace($Distro)) {
  $Distro = Get-XrEnvOrDefault "XR_WSL2_DISTRO" "Ubuntu-24.04"
}
if ([string]::IsNullOrWhiteSpace($WslProjectRoot)) {
  $WslProjectRoot = Get-XrEnvOrDefault "XR_WSL_PROJECT_ROOT" "~/src/xr_tracking"
}
if ([string]::IsNullOrWhiteSpace($WslOutName)) {
  $WslOutName = Get-XrEnvOrDefault "XR_WSL_OUT_NAME" "xreal_ultra"
}
if ([string]::IsNullOrWhiteSpace($WslRuntimeRoot)) {
  $WslRuntimeRoot = "$WslProjectRoot/out/$WslOutName"
}

function Quote-Bash([string]$Value) {
  return "'" + $Value.Replace("'", "'\''") + "'"
}

$EscapedRuntimeRoot = $WslRuntimeRoot.Replace("'", "'\''")
$RuntimeRootResolved = (& wsl.exe -d $Distro -- bash -lc "cd '$EscapedRuntimeRoot' && pwd").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($RuntimeRootResolved)) {
  throw "WSL runtime root not found. Run install_wsl2_ubuntu24_runtime.ps1 first. Root: $WslRuntimeRoot"
}

$StartScript = "$RuntimeRootResolved/scripts/start_windows_wsl2_runtime.sh"
$ConfigPath = "$RuntimeRootResolved/configs/windows_wsl2_network.env"
$Command = "test -x $(Quote-Bash $StartScript) && XR_WINDOWS_NETWORK_CONFIG=$(Quote-Bash $ConfigPath) exec $(Quote-Bash $StartScript)"

Write-Host "[start_wsl2_runtime] distro=$Distro"
Write-Host "[start_wsl2_runtime] runtime=$RuntimeRootResolved"
Write-Host "[start_wsl2_runtime] config=$ConfigPath"
wsl.exe -d $Distro -- bash -lc $Command
exit $LASTEXITCODE
