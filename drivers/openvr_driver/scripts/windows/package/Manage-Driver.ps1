#Requires -Version 5.1
[CmdletBinding()]
param(
  [ValidateSet('Install', 'Uninstall', 'Guide', 'Test')][string]$Action = 'Install',
  [string]$SteamVR = '',
  [string]$DriverPackage = $PSScriptRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SteamVrDirectory {
  param([string]$Explicit)
  if ($Explicit) {
    $path = (Resolve-Path -LiteralPath $Explicit).Path
    if (!(Test-Path -LiteralPath (Join-Path $path 'bin\win64\vrpathreg.exe'))) { throw 'Not a SteamVR directory.' }
    return $path
  }
  $paths = New-Object 'System.Collections.Generic.List[string]'
  $openVrPaths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
  if (Test-Path -LiteralPath $openVrPaths) {
    $data = Get-Content -Raw -LiteralPath $openVrPaths | ConvertFrom-Json
    foreach ($entry in @($data.runtime)) { if ($entry) { $paths.Add([string]$entry) } }
  }
  $steam = ''
  try { $steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { }
  if ($steam) { $paths.Add((Join-Path $steam 'steamapps\common\SteamVR')) }
  if (${env:ProgramFiles(x86)}) { $paths.Add((Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR')) }
  foreach ($path in $paths) {
    if (Test-Path -LiteralPath (Join-Path $path 'bin\win64\vrpathreg.exe')) { return $path }
  }
  $manual = Read-Host 'SteamVR folder (for example D:\SteamLibrary\steamapps\common\SteamVR)'
  if (!$manual) { throw 'SteamVR directory is required.' }
  return Get-SteamVrDirectory -Explicit $manual.Trim().Trim('"')
}

try {
  if ($Action -in @('Install', 'Uninstall') -and !(Test-Path -LiteralPath (Join-Path $DriverPackage 'driver.vrdrivermanifest'))) {
    $DriverPackage = (Read-Host 'Full path to the extracted xr_tracking driver folder').Trim().Trim('"')
    if (!$DriverPackage) { throw 'Driver package directory is required.' }
  }
  $DriverPackage = (Resolve-Path -LiteralPath $DriverPackage).Path
  if ($Action -eq 'Guide') {
    $name = if ([Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'ru') { 'WINDOWS.ru.md' } else { 'WINDOWS.en.md' }
    Write-Host 'SteamVR: Settings > Developer > Enable Direct Mode. See the guide for SBS/USB ordering and hardware limits.'
    $html = Join-Path $DriverPackage 'START-HERE.html'
    if (Test-Path -LiteralPath $html) { Start-Process -FilePath $html }
    else { Start-Process notepad.exe -ArgumentList ('"' + (Join-Path $DriverPackage $name) + '"') }
    Start-Process 'steam://rungameid/250820'
    exit 0
  }
  if ($Action -eq 'Test') {
    $client = New-Object Net.Sockets.TcpClient
    try {
      $wait = $client.BeginConnect('127.0.0.1', 45810, $null, $null)
      if (!$wait.AsyncWaitHandle.WaitOne(5000)) { throw 'TCP connection timed out; start XR Gate first.' }
      $client.EndConnect($wait)
      $client.GetStream().ReadTimeout = 5000
      $reader = New-Object IO.StreamReader($client.GetStream())
      try {
        $hello = $reader.ReadLine() | ConvertFrom-Json
        $frame = $reader.ReadLine() | ConvertFrom-Json
        if ($hello.protocol -ne 'xr_gate_app_v1' -or $frame.type -ne 'frame') { throw 'Unexpected XR Gate protocol.' }
        Write-Host 'OK: 127.0.0.1:45810, xr_gate_app_v1. Check pose_valid separately.'
      } finally { $reader.Dispose() }
    } finally { $client.Close() }
    exit 0
  }
  if (Get-Process vrserver, vrcompositor -ErrorAction SilentlyContinue) { throw 'Close SteamVR before installing/removing the driver.' }
  if (!(Test-Path -LiteralPath (Join-Path $DriverPackage 'driver.vrdrivermanifest'))) { throw 'Driver manifest missing; extract the full package.' }
  if ($Action -eq 'Install' -and !(Test-Path -LiteralPath (Join-Path $DriverPackage 'bin\win64\driver_xr_tracking.dll'))) { throw 'win64 DLL missing.' }
  $SteamVR = Get-SteamVrDirectory -Explicit $SteamVR
  $reg = Join-Path $SteamVR 'bin\win64\vrpathreg.exe'
  $operation = if ($Action -eq 'Install') { 'adddriver' } else { 'removedriver' }
  & $reg $operation $DriverPackage
  if ($LASTEXITCODE -ne 0) { throw "vrpathreg failed: $LASTEXITCODE" }
  & $reg show
  if ($LASTEXITCODE -ne 0) { throw "vrpathreg show failed: $LASTEXITCODE" }
  Write-Host "OK: $Action $DriverPackage"
} catch { Write-Host ('Error: ' + $_.Exception.Message) -ForegroundColor Red; exit 1 }
