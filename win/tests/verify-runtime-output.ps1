#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:Shell = (Get-Process -Id $PID).Path
function Get-Tool($Name) { return $script:Shell }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-runtime-output-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
$script:StateDir = $temporary
try {
    $record = [pscustomobject]@{Token=('a' * 32)}
    $files = Get-RuntimeWindowFiles $record
    $command = '$OutputEncoding=[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false); [Console]::WriteLine("runtime started"); [Console]::Error.WriteLine("camera unavailable"); exit 7'
    $code = Invoke-LoggedRuntime -Arguments @('-NoLogo','-NoProfile','-Command',$command) -Files $files
    Assert ($code -eq 7) 'Runtime exit code was lost.'
    Assert ((Get-Content -Raw -LiteralPath $files.Output).Contains('runtime started')) 'Hidden runtime stdout was lost.'
    Assert ((Get-Content -Raw -LiteralPath $files.Error).Contains('camera unavailable')) 'Hidden runtime stderr was lost.'
    [IO.File]::WriteAllText($files.Result, '{"message":"fixture runtime failed"}')
    $exit = Get-RuntimeExitObservation $record
    Assert ($exit.message -eq 'fixture runtime failed' -and $exit.log_tail.Count -eq 2) 'Exited runtime observation lost its failure or current-token log.'
    Assert (!(Get-RuntimeWindowFiles ([pscustomobject]@{Token='../other'}))) 'Runtime log paths trusted an invalid token.'
    Write-Host 'OK: hidden runtime owns UTF-8 log pipes, preserves its exit code and reports a bounded current-session failure.'
} finally { Remove-Item -Recurse -Force -LiteralPath $temporary }
