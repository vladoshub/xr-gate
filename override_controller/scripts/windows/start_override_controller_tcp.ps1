param(
  [string]$Root = "",
  [string]$Config = "",
  [int]$Port = 45672,
  [switch]$Train,
  [switch]$ListDevices
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Root)) {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $Root = Resolve-Path (Join-Path $scriptDir "..\..\..")
}
$Root = (Resolve-Path $Root).Path
$Exe = Join-Path $Root "bin\override_controller\override_controller.exe"
if (!(Test-Path $Exe)) { throw "override_controller.exe not found. Run build_override_controller.ps1 first." }
$args = @("--publish-transport", "tcp", "--publish-tcp-bind-host", "127.0.0.1", "--publish-tcp-port", "$Port")
if ($Train) { $args += "--train" }
if ($ListDevices) { $args += "--list-devices" }
if (-not [string]::IsNullOrWhiteSpace($Config)) { $args += @("--config", $Config) }
& $Exe @args
