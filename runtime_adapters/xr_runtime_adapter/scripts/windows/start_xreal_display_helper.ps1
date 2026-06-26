param(
  [string]$Root = "",
  [ValidateSet("90hz", "60hz", "3d", "2d", "half-sbs", "120hz")]
  [string]$Mode = "90hz",
  [switch]$KeepRunning,
  [switch]$ListDevices,
  [string]$Path = "",
  [int]$Interface = -1,
  [int]$PollSeconds = 2
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Root)) {
  $Root = Resolve-Path (Join-Path $ScriptDir "..\..\..")
} else {
  $Root = Resolve-Path $Root
}

$Candidates = @(
  (Join-Path $Root "out\xreal_ultra\bin\xreal_display_helper\xreal_display_helper.exe"),
  (Join-Path $Root "build\xreal_display_helper\windows_RelWithDebInfo\RelWithDebInfo\xreal_display_helper.exe"),
  (Join-Path $Root "build\xreal_display_helper\windows_Debug\Debug\xreal_display_helper.exe")
)

$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$Exe) {
  throw "xreal_display_helper.exe not found. Run scripts\windows\build_xreal_display_helper.ps1 first."
}

$Args = @()
if ($ListDevices) {
  $Args += "--list-devices"
} else {
  $Args += @("--mode", $Mode)
  if ($KeepRunning) { $Args += "--keep-running" }
  $Args += @("--poll-seconds", "$PollSeconds")
  if (![string]::IsNullOrWhiteSpace($Path)) { $Args += @("--path", $Path) }
  if ($Interface -ge 0) { $Args += @("--interface", "$Interface") }
}

Write-Host "[start_xreal_display_helper] $Exe $($Args -join ' ')"
& $Exe @Args
exit $LASTEXITCODE
