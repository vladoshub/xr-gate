param(
  [string]$InstallBinDir = "",
  [string]$BuildDir = "",
  [string]$OpenCvDir = $env:OpenCV_DIR,
  [string]$HidApiRoot = $env:HIDAPI_ROOT
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BackendDir = Resolve-Path (Join-Path $ScriptDir "..\..")
$RootProject = Resolve-Path (Join-Path $BackendDir "..")
if ([string]::IsNullOrWhiteSpace($InstallBinDir)) {
  $InstallBinDir = Join-Path $RootProject "bin\capture_service_cpp"
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $RootProject "build\capture_service_cpp_windows"
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $InstallBinDir | Out-Null

$cmakeArgs = @(
  "-S", $BackendDir,
  "-B", $BuildDir,
  "-DCMAKE_BUILD_TYPE=Release"
)
if (-not [string]::IsNullOrWhiteSpace($OpenCvDir)) {
  $cmakeArgs += "-DOpenCV_DIR=$OpenCvDir"
}
if (-not [string]::IsNullOrWhiteSpace($HidApiRoot)) {
  $cmakeArgs += "-DHIDAPI_ROOT=$HidApiRoot"
}

Write-Host "[build_capture_service_cpp_windows] configuring"
cmake @cmakeArgs
Write-Host "[build_capture_service_cpp_windows] building"
cmake --build $BuildDir --config Release

$exe = Get-ChildItem -Path $BuildDir -Filter capture_service_cpp.exe -Recurse | Select-Object -First 1
if (-not $exe) {
  throw "capture_service_cpp.exe was not produced"
}
Copy-Item $exe.FullName (Join-Path $InstallBinDir "capture_service_cpp.exe") -Force
Write-Host "[build_capture_service_cpp_windows] installed: $InstallBinDir\capture_service_cpp.exe"
