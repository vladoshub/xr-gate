param(
  [string]$Root = "",
  [string]$BuildType = "RelWithDebInfo",
  [string]$Generator = "Ninja",
  [string]$VcpkgToolchain = "",
  [string]$NlohmannJsonInclude = ""
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..")
}
$Root = (Resolve-Path $Root).Path
$Src = Join-Path $Root "override_controller"
$BuildDir = Join-Path $Root "build\override_controller\windows_$BuildType"
$args = @("-S", $Src, "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=$BuildType", "-DCMAKE_INSTALL_PREFIX=$Root")
if (-not [string]::IsNullOrWhiteSpace($VcpkgToolchain)) { $args += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain" }
if (-not [string]::IsNullOrWhiteSpace($NlohmannJsonInclude)) { $args += "-DNLOHMANN_JSON_INCLUDE_DIR=$NlohmannJsonInclude" }
cmake @args
cmake --build $BuildDir --config $BuildType
cmake --install $BuildDir --config $BuildType
