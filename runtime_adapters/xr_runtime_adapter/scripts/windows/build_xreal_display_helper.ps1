param(
  [string]$Root = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Visual Studio 17 2022",
  [string]$Arch = "x64",
  [string]$VcpkgToolchain = "",
  [string]$InstallPrefix = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = Resolve-Path (Join-Path $ScriptDir "..\..\..")
} else {
  $Root = Resolve-Path $Root
}

$HelperDir = Join-Path $Root "xreal_display_helper"
if (!(Test-Path $HelperDir)) {
  throw "xreal_display_helper not found: $HelperDir"
}

$BuildDir = Join-Path $Root "build\xreal_display_helper\windows_$BuildType"
if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
  $InstallPrefix = Join-Path $Root "out\xreal_ultra"
}

$Args = @(
  "-S", $HelperDir,
  "-B", $BuildDir,
  "-G", $Generator,
  "-A", $Arch,
  "-DCMAKE_BUILD_TYPE=$BuildType",
  "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
)

if (![string]::IsNullOrWhiteSpace($VcpkgToolchain)) {
  $Args += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
}

Write-Host "[build_xreal_display_helper] Root=$Root"
Write-Host "[build_xreal_display_helper] BuildDir=$BuildDir"
Write-Host "[build_xreal_display_helper] InstallPrefix=$InstallPrefix"

cmake @Args
cmake --build $BuildDir --config $BuildType --target xreal_display_helper
cmake --install $BuildDir --config $BuildType

$Exe = Join-Path $InstallPrefix "bin\xreal_display_helper\xreal_display_helper.exe"
Write-Host "[build_xreal_display_helper] built: $Exe"
