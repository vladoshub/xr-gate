#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Repository = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Launcher = Join-Path $Repository "xr_client\scripts\windows\run_xr_client.ps1"
$Scratch = Join-Path ([IO.Path]::GetTempPath()) ("xr-launcher-test-" + [guid]::NewGuid().ToString("N"))

function Assert-Equal($Actual, $Expected, [string]$Message) {
  if ($Actual -ne $Expected) { throw "$Message Expected '$Expected', got '$Actual'." }
}

function Write-TestFile([string]$Path, [string]$Content) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

function global:Invoke-XrLauncherTestPython {
  $global:XrLauncherTestCall = [pscustomobject]@{
    Arguments = @($args)
    Device = $env:XR_TARGET_DEVICE
    DeviceHome = $env:XR_DEVICE_HOME
    OutRoot = $env:XR_OUT_ROOT
    BinRoot = $env:XR_OUT_BIN_ROOT
    ScriptsRoot = $env:XR_DEVICE_SCRIPTS_ROOT
    CapturePort = $env:XR_CAPTURE_TCP_PORT
  }
  $global:LASTEXITCODE = 0
}

function Invoke-TestLauncher([hashtable]$Parameters, [hashtable]$Environment = @{}) {
  $Saved = @{}
  foreach ($Entry in Get-ChildItem Env:) {
    if ($Entry.Name -like "XR_*" -or $Entry.Name -in @("ROOT_PROJECT", "STARTUP_GATE")) {
      $Saved[$Entry.Name] = $Entry.Value
      Remove-Item "Env:$($Entry.Name)"
    }
  }
  try {
    foreach ($Key in $Environment.Keys) { [Environment]::SetEnvironmentVariable($Key, $Environment[$Key], "Process") }
    $env:XR_PYTHON_BIN = "Invoke-XrLauncherTestPython"
    $global:XrLauncherTestCall = $null
    & $Launcher @Parameters
    if ($LASTEXITCODE -ne 0 -or $null -eq $global:XrLauncherTestCall) { throw "Launcher did not invoke Python successfully" }
    return $global:XrLauncherTestCall
  } finally {
    foreach ($Entry in Get-ChildItem Env:) {
      if ($Entry.Name -like "XR_*" -or $Entry.Name -in @("ROOT_PROJECT", "STARTUP_GATE")) { Remove-Item "Env:$($Entry.Name)" }
    }
    foreach ($Key in $Saved.Keys) { [Environment]::SetEnvironmentVariable($Key, $Saved[$Key], "Process") }
  }
}

try {
  $Tokens = $null
  $ParseErrors = $null
  [void][System.Management.Automation.Language.Parser]::ParseFile($Launcher, [ref]$Tokens, [ref]$ParseErrors)
  if ($ParseErrors.Count) { throw ($ParseErrors | Out-String) }

  $Source = Join-Path $Scratch "Source with spaces"
  Write-TestFile (Join-Path $Source "xr_client\xr_backend_client.py") "# mocked Python client"
  $Config = Join-Path $Source "xr_client\configs\custom.json"
  Write-TestFile $Config '{"windows_launcher":{"device":"custom_headset","out_dir":"out/custom bundle","env_script":"bootstrap/custom.ps1"}}'
  Write-TestFile (Join-Path $Source "bootstrap\custom.ps1") '$env:XR_CAPTURE_TCP_PORT = "49900"'
  $Call = Invoke-TestLauncher @{Root=$Source; Config="custom"; DryRun=$true}
  Assert-Equal $Call.Device "custom_headset" "Device must come from JSON."
  Assert-Equal $Call.DeviceHome (Join-Path $Source "devices\custom_headset") "Device directory must be generic."
  Assert-Equal $Call.OutRoot (Join-Path $Source "out\custom bundle") "Source output must come from JSON."
  Assert-Equal $Call.Arguments[2] $Config "JSON path with spaces must be one argument."
  Assert-Equal $Call.Arguments[3] "--dry-run" "DryRun must reach Python."
  Assert-Equal $Call.CapturePort "49900" "Relative bootstrap paths must resolve against Root."

  $ExplicitOut = Join-Path $Scratch "explicit output"
  $ExplicitHome = Join-Path $Scratch "explicit device"
  $Call = Invoke-TestLauncher @{Root=$Source; Config=$Config; Device="another-device"; OutRoot=$ExplicitOut} @{XR_DEVICE_HOME=$ExplicitHome}
  Assert-Equal $Call.Device "another_device" "Explicit Device must override JSON."
  Assert-Equal $Call.OutRoot $ExplicitOut "Explicit OutRoot must override JSON."
  Assert-Equal $Call.DeviceHome $ExplicitHome "An explicit device path must be preserved."

  Write-TestFile (Join-Path $Source "xr_client\configs\neutral.json") '{}'
  $Call = Invoke-TestLauncher @{Root=$Source; Config="neutral"}
  Assert-Equal $Call.Device "generic" "A neutral profile must not select vendor hardware."
  $Call = Invoke-TestLauncher @{Root=$Source; Config="neutral"} @{XR_TARGET_DEVICE="env_device"; XR_OUT_ROOT=$ExplicitOut}
  Assert-Equal $Call.Device "env_device" "Environment device must be preserved."
  Assert-Equal $Call.OutRoot $ExplicitOut "Environment output path must be preserved."

  $Package = Join-Path $Scratch "Package with spaces"
  Write-TestFile (Join-Path $Package "bin\python\xr_client\xr_backend_client.py") "# mocked packaged client"
  Write-TestFile (Join-Path $Package "bin\python\xr_client\configs\neutral.json") '{}'
  $Call = Invoke-TestLauncher @{Root=$Package; Config="neutral"}
  Assert-Equal $Call.OutRoot $Package "A package must not append another out directory."
  Assert-Equal $Call.Arguments[0] (Join-Path $Package "bin\python\xr_client\xr_backend_client.py") "Packaged Python client must be found."

  # Preserve the shipped vendor profile, including its network bootstrap.
  $VendorScripts = "devices\xreal_ultra\windows\scripts"
  foreach ($Name in @("xreal_ultra_out_env.ps1", "load_windows_wsl2_network_env.ps1")) {
    Write-TestFile (Join-Path $Source "$VendorScripts\$Name") (Get-Content (Join-Path $Repository "$VendorScripts\$Name") -Raw)
  }
  Write-TestFile (Join-Path $Source "xr_client\configs\default_windows_tcp.json") (Get-Content (Join-Path $Repository "xr_client\configs\default_windows_tcp.json") -Raw)
  Write-TestFile (Join-Path $Source "devices\xreal_ultra\windows\configs\windows_wsl2_network.env") "XR_CAPTURE_TCP_PORT=49600"
  $Call = Invoke-TestLauncher @{Root=$Source}
  Assert-Equal $Call.Device "xreal_ultra" "The shipped Windows profile must preserve its selected device."
  Assert-Equal $Call.OutRoot (Join-Path $Source "out\xreal_ultra_windows") "The shipped source output must remain compatible."
  Assert-Equal $Call.CapturePort "49600" "Network env must load before starting Python."
  $Call = Invoke-TestLauncher @{Root=$Source} @{XR_CAPTURE_TCP_PORT="49700"}
  Assert-Equal $Call.CapturePort "49700" "Explicit network settings must win."

  Write-Host "OK Windows launcher: custom/neutral profiles, explicit overrides, package paths, default vendor/network compatibility."
} finally {
  Remove-Item Function:\Invoke-XrLauncherTestPython
  Remove-Variable XrLauncherTestCall -Scope Global -ErrorAction SilentlyContinue
  if (Test-Path $Scratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force }
}
