# Shared loader for devices/xreal_ultra/windows/configs/windows_wsl2_network.env.
# Dot-source this file from Windows device scripts.

function Import-XrWindowsNetworkEnv {
  param(
    [string]$Root = "",
    [string]$NetworkConfig = ""
  )

  $candidates = @()
  if (-not [string]::IsNullOrWhiteSpace($NetworkConfig)) {
    $candidates += $NetworkConfig
  }
  if (-not [string]::IsNullOrWhiteSpace($env:XR_WINDOWS_NETWORK_CONFIG)) {
    $candidates += $env:XR_WINDOWS_NETWORK_CONFIG
  }
  if (-not [string]::IsNullOrWhiteSpace($env:XR_OUT_ROOT)) {
    $candidates += (Join-Path $env:XR_OUT_ROOT "devices\xreal_ultra\windows\configs\windows_wsl2_network.env")
  }
  if (-not [string]::IsNullOrWhiteSpace($Root)) {
    $candidates += (Join-Path $Root "devices\xreal_ultra\windows\configs\windows_wsl2_network.env")
    $candidates += (Join-Path $Root "out\xreal_ultra_windows\devices\xreal_ultra\windows\configs\windows_wsl2_network.env")
  }

  $path = $null
  foreach ($candidate in $candidates) {
    if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
    try {
      $resolved = (Resolve-Path $candidate -ErrorAction Stop).Path
      $path = $resolved
      break
    } catch {
      continue
    }
  }
  if (-not $path) {
    return $null
  }

  Get-Content -Path $path | ForEach-Object {
    $line = $_.Trim()
    if ([string]::IsNullOrWhiteSpace($line)) { return }
    if ($line.StartsWith("#")) { return }
    $idx = $line.IndexOf("=")
    if ($idx -le 0) { return }
    $key = $line.Substring(0, $idx).Trim()
    $value = $line.Substring($idx + 1).Trim()
    if (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'"))) {
      $value = $value.Substring(1, $value.Length - 2)
    }
    if (-not [string]::IsNullOrWhiteSpace($key) -and -not [Environment]::GetEnvironmentVariable($key, "Process")) {
      [Environment]::SetEnvironmentVariable($key, $value, "Process")
    }
  }

  $env:XR_WINDOWS_NETWORK_CONFIG_RESOLVED = $path
  return $path
}

function Get-XrEnvOrDefault {
  param(
    [string]$Name,
    [string]$Default = ""
  )
  $value = [Environment]::GetEnvironmentVariable($Name, "Process")
  if ([string]::IsNullOrWhiteSpace($value)) { return $Default }
  return $value
}
