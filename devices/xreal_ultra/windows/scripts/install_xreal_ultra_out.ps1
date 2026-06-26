param(
  [string]$Root = "",
  [string]$OutRoot = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Ninja",
  [string]$Arch = "x64",
  [string]$VcpkgToolchain = $env:VCPKG_TOOLCHAIN_FILE,
  [string]$OpenCvDir = $env:OpenCV_DIR,
  [string]$HidApiRoot = $env:HIDAPI_ROOT,
  [string]$OpenVrSdkRoot = $env:XR_OPENVR_SDK_ROOT,
  [string]$NlohmannJsonInclude = $env:NLOHMANN_JSON_INCLUDE_DIR,
  [string]$Device = "",
  [int[]]$OpenVrFrequencies = @(60, 75, 90),
  [string[]]$BuildOnly = @()
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = (Resolve-Path (Join-Path $ScriptDir "..\..\..\..")).Path
} else {
  $Root = (Resolve-Path $Root).Path
}
if (-not [string]::IsNullOrWhiteSpace($OutRoot)) {
  $env:XR_OUT_ROOT = $OutRoot
}
if ([string]::IsNullOrWhiteSpace($Device)) {
  if (-not [string]::IsNullOrWhiteSpace($env:XR_TARGET_DEVICE)) { $Device = $env:XR_TARGET_DEVICE } else { $Device = "xreal_ultra" }
}
$env:ROOT_PROJECT = $Root
$env:XR_ROOT_PROJECT = $Root
$env:XR_TARGET_DEVICE = $Device
$env:XR_DEVICE_TARGET = $Device
. (Join-Path $ScriptDir "xreal_ultra_out_env.ps1")

$OutRoot = $env:XR_OUT_ROOT
$OutBin = $env:XR_OUT_BIN_ROOT
New-Item -ItemType Directory -Force -Path $OutBin | Out-Null

function Log([string]$Message) {
  Write-Host "[install_xreal_ultra_out_windows] $Message"
}

function Should-Build([string]$Name) {
  if ($BuildOnly.Count -eq 0) { return $true }
  foreach ($target in $BuildOnly) {
    if ($target -eq $Name -or $target -eq "all") { return $true }
    if (($target -eq "capture_service" -or $target -eq "capture_cpp") -and $Name -eq "capture_service_cpp") { return $true }
    if ($target -eq "drivers" -and ($Name -eq "openvr_driver")) { return $true }
    if ($target -eq "steamvr" -and $Name -eq "openvr_driver") { return $true }
  }
  return $false
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
  if (-not (Should-Build $Name)) {
    Log "skip $Name due BuildOnly=$($BuildOnly -join ',')"
    return
  }
  Log "== $Name =="
  & $Body
}

function Invoke-CMakeConfigureBuild([string]$SourceDir, [string]$BuildDir, [string]$InstallPrefix, [string[]]$ExtraArgs = @()) {
  New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
  $cmakeArgs = @("-S", $SourceDir, "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=$BuildType", "-DCMAKE_INSTALL_PREFIX=$InstallPrefix")
  if ($Generator -match "Visual Studio" -and -not [string]::IsNullOrWhiteSpace($Arch)) {
    $cmakeArgs += @("-A", $Arch)
  }
  if (-not [string]::IsNullOrWhiteSpace($VcpkgToolchain)) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
  }
  $cmakeArgs += $ExtraArgs
  cmake @cmakeArgs
  cmake --build $BuildDir --config $BuildType
}

function Copy-Tree([string]$Src, [string]$Dst) {
  if (-not (Test-Path $Src)) { return }
  if (Test-Path $Dst) { Remove-Item -Recurse -Force $Dst }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dst) | Out-Null
  Copy-Item -Recurse -Force $Src $Dst
}

Invoke-Step "capture_service_cpp" {
  $Source = Join-Path $Root "capture_service_cpp"
  $Build = Join-Path $Root "build\capture_service_cpp\windows_$BuildType"
  $Install = Join-Path $OutBin "capture_service_cpp"
  $extra = @("-DXR_CAPTURE_SERVICE_CPP_DEVICE=$Device")
  if (-not [string]::IsNullOrWhiteSpace($OpenCvDir)) { $extra += "-DOpenCV_DIR=$OpenCvDir" }
  if (-not [string]::IsNullOrWhiteSpace($HidApiRoot)) { $extra += "-DHIDAPI_ROOT=$HidApiRoot" }
  Invoke-CMakeConfigureBuild $Source $Build $Install $extra
  cmake --install $Build --config $BuildType --prefix $Install
  $Exe = Get-ChildItem -Path $Install -Filter "capture_service_cpp.exe" -Recurse | Select-Object -First 1
  if (-not $Exe) { throw "capture_service_cpp.exe was not installed under $Install" }
  Log "installed: $($Exe.FullName)"
}

Invoke-Step "xreal_display_helper" {
  $Source = Join-Path $Root "tools\xreal_ultra\xreal_display_helper"
  $Build = Join-Path $Root "build\tools\xreal_ultra\xreal_display_helper\windows_$BuildType"
  Invoke-CMakeConfigureBuild $Source $Build $OutRoot @()
  cmake --install $Build --config $BuildType --prefix $OutRoot
  $Exe = Join-Path $OutBin "xreal_display_helper\xreal_display_helper.exe"
  if (-not (Test-Path $Exe)) { throw "xreal_display_helper.exe was not installed: $Exe" }
  Log "installed: $Exe"
}


Invoke-Step "override_controller" {
  $Source = Join-Path $Root "override_controller"
  $Build = Join-Path $Root "build\override_controller\windows_$BuildType"
  $extra = @(
    "-DXR_TRACKING_ROOT=$Root",
    "-DXR_SHARED_INCLUDE_DIR=$(Join-Path $Root 'shared\include')"
  )
  if (-not [string]::IsNullOrWhiteSpace($NlohmannJsonInclude)) {
    $extra += "-DNLOHMANN_JSON_INCLUDE_DIR=$NlohmannJsonInclude"
  }
  Invoke-CMakeConfigureBuild $Source $Build $OutRoot $extra
  cmake --install $Build --config $BuildType --prefix $OutRoot
  $Exe = Join-Path $OutBin "override_controller\override_controller.exe"
  if (-not (Test-Path $Exe)) { throw "override_controller.exe was not installed: $Exe" }
  Log "installed: $Exe"
}

Invoke-Step "openvr_driver" {
  if ([string]::IsNullOrWhiteSpace($OpenVrSdkRoot)) {
    throw "Set -OpenVrSdkRoot or XR_OPENVR_SDK_ROOT to Valve OpenVR SDK root"
  }
  $Source = Join-Path $Root "drivers\openvr_driver"
  foreach ($freq in $OpenVrFrequencies) {
    $Build = Join-Path $Root "build\drivers\openvr_driver_${freq}HZ\windows_$BuildType"
    $extra = @("-DXR_OPENVR_SDK_ROOT=$OpenVrSdkRoot")
    Invoke-CMakeConfigureBuild $Source $Build $OutRoot $extra
    $Pkg = Join-Path $Build "xr_tracking"
    $Settings = Join-Path $Pkg "resources\settings\default.vrsettings"
    if (-not (Test-Path $Settings)) { throw "OpenVR package settings not found: $Settings" }
    $json = Get-Content $Settings -Raw | ConvertFrom-Json
    $DeviceProfile = Join-Path $Source "devices\$Device\settings\default.vrsettings"
    if (Test-Path $DeviceProfile) {
      $overlay = Get-Content $DeviceProfile -Raw | ConvertFrom-Json
      foreach ($section in @("xr_tracking", "steamvr")) {
        if ($overlay.PSObject.Properties.Name -contains $section) {
          if (-not ($json.PSObject.Properties.Name -contains $section)) {
            $json | Add-Member -NotePropertyName $section -NotePropertyValue ([pscustomobject]@{})
          }
          foreach ($prop in $overlay.$section.PSObject.Properties) {
            $json.$section | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
          }
        }
      }
    }
    if (-not $json.xr_tracking) { $json | Add-Member -NotePropertyName xr_tracking -NotePropertyValue ([pscustomobject]@{}) }
    if (-not $json.steamvr) { $json | Add-Member -NotePropertyName steamvr -NotePropertyValue ([pscustomobject]@{}) }
    $json.xr_tracking | Add-Member -NotePropertyName deviceProfile -NotePropertyValue $Device -Force
    $json.xr_tracking | Add-Member -NotePropertyName displayFrequency -NotePropertyValue $freq -Force
    $json.steamvr | Add-Member -NotePropertyName displayFrequency -NotePropertyValue $freq -Force
    $json | ConvertTo-Json -Depth 32 | Set-Content -Encoding UTF8 $Settings

    $Dst = Join-Path $OutBin "drivers\openvr_driver_${freq}HZ\xr_tracking"
    Copy-Tree $Pkg $Dst
    Log "installed OpenVR ${freq}Hz package: $Dst"
  }
}

# Copy Windows launcher/config scripts into the deploy tree.
Copy-Tree (Join-Path $Root "devices\xreal_ultra\windows") (Join-Path $OutRoot "devices\xreal_ultra\windows")
if (Test-Path (Join-Path $Root "devices\xreal_ultra\configs")) {
  Copy-Tree (Join-Path $Root "devices\xreal_ultra\configs") (Join-Path $OutRoot "devices\xreal_ultra\configs")
}

# Copy xr_client into the Windows deploy package so it can be launched directly
# from out/xreal_ultra_windows without relying on the source-tree path.
if (Test-Path (Join-Path $Root "xr_client")) {
  Copy-Tree (Join-Path $Root "xr_client") (Join-Path $OutRoot "xr_client")
}

# Keep Python runtime modules next to the deploy package.  The C++ capture
# service is native, but xr_client/startup_gate still imports capture_client.
if (Test-Path (Join-Path $Root "capture_client")) {
  Copy-Tree (Join-Path $Root "capture_client") (Join-Path $OutBin "python\capture_client")
} elseif (Test-Path (Join-Path $Root "capture_service\capture_client")) {
  # Compatibility with older source-tree layout during migration.
  Copy-Tree (Join-Path $Root "capture_service\capture_client") (Join-Path $OutBin "python\capture_client")
}

# Convenience package-root launcher: out/xreal_ultra_windows/run_xr_client.ps1
$PackageRunClient = Join-Path $OutRoot "run_xr_client.ps1"
@'
param(
  [string]$Config = "",
  [switch]$DryRun,
  [switch]$NoStartupGate
)

$ErrorActionPreference = "Stop"
$PackageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Launcher = Join-Path $PackageRoot "xr_client\scripts\windows\run_xr_client.ps1"
if (-not (Test-Path $Launcher)) {
  throw "xr_client package launcher not found: $Launcher"
}

$argsList = @(
  "-Root", $PackageRoot
)
if (-not [string]::IsNullOrWhiteSpace($Config)) {
  $argsList += @("-Config", $Config)
}
if ($DryRun) { $argsList += "-DryRun" }
if ($NoStartupGate) { $argsList += "-NoStartupGate" }

& $Launcher @argsList
exit $LASTEXITCODE
'@ | Set-Content -Encoding UTF8 $PackageRunClient
Log "installed xr_client package: $(Join-Path $OutRoot 'xr_client')"
Log "installed package launcher: $PackageRunClient"

Log "done: $OutRoot"
