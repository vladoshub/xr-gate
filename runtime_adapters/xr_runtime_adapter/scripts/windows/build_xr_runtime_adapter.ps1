param(
  [string]$Root = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Ninja",
  [string]$VcpkgToolchain = "",
  [string]$Cli11Include = "",
  [string]$NlohmannJsonInclude = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
}
$Root = (Resolve-Path $Root).Path
$AdapterDir = Join-Path $Root "runtime_adapters\xr_runtime_adapter"
$BuildDir = Join-Path $Root "build\runtime_adapters\xr_runtime_adapter\windows_$BuildType"
$InstallDir = Join-Path $Root "bin\runtime_adapters\xr_runtime_adapter"

$args = @("-S", $AdapterDir, "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=$BuildType", "-DCMAKE_INSTALL_PREFIX=$Root")
if (-not [string]::IsNullOrWhiteSpace($VcpkgToolchain)) { $args += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain" }
if (-not [string]::IsNullOrWhiteSpace($Cli11Include)) { $args += "-DCLI11_INCLUDE_DIR=$Cli11Include" }
if (-not [string]::IsNullOrWhiteSpace($NlohmannJsonInclude)) { $args += "-DNLOHMANN_JSON_INCLUDE_DIR=$NlohmannJsonInclude" }

cmake @args
cmake --build $BuildDir --config $BuildType
cmake --install $BuildDir --config $BuildType

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Write-Host "[build_xr_runtime_adapter] installed to $InstallDir"
