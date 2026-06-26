param(
  [string]$Root = "",
  [switch]$Once,
  [int]$PollSeconds = 2
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

$Candidates = @(
  (Join-Path $env:XR_OUT_BIN_ROOT "xreal_display_helper\xreal_display_helper.exe"),
  (Join-Path $Root "bin\xreal_display_helper\xreal_display_helper.exe")
)
$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
  throw "xreal_display_helper.exe not found. Run devices\xreal_ultra\windows\scripts\install_xreal_ultra_out.ps1 first."
}

$args = @("--mode", "90hz", "--poll-seconds", "$PollSeconds")
if (-not $Once) { $args += "--keep-running" }
Write-Host "[start_xreal_helper_90hz_windows] $Exe $($args -join ' ')"
& $Exe @args
exit $LASTEXITCODE
