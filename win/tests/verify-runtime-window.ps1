#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -and
    (Test-Path -LiteralPath '/proc/self/stat') -and
    (Get-Content -Raw -LiteralPath '/proc/self/stat').Split(' ')[0] -ne [string]$PID) {
    Write-Host 'SKIP real runtime-window process test: /proc is outside this PID namespace; runs on Windows CI.'
    return
}
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-window-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$script:StateDir = $temporary
$shell = (Get-Process -Id $PID).Path
$fixture = Join-Path $temporary 'sleep.ps1'
[IO.File]::WriteAllText($fixture, 'Start-Sleep -Seconds 60')
$process = $null
try {
    $process = Start-Process $shell -PassThru -ArgumentList @('-NoLogo','-NoProfile','-File',('"' + $fixture + '"'))
    $token = [Guid]::NewGuid().ToString('N')
    Write-RuntimeWindow @{Bundle='previous-extraction-folder'; ProcessId=$process.Id;
        StartTicks=$process.StartTime.ToUniversalTime().Ticks.ToString(); Token=$token; Stopping=$false}
    Wait-RuntimeWindowRegistration $token
    $owned = Get-RuntimeWindowProcess (Read-RuntimeWindow)
    Assert ($owned -and $owned.Id -eq $process.Id) 'The owned runtime console identity was lost.'
    $owned.Dispose()
    # A PID alone is insufficient. A stale record must leave this process alive.
    $record = Read-RuntimeWindow
    $record.StartTicks = '1'
    Write-RuntimeWindow $record
    Stop-RuntimeWindow
    Assert (!$process.HasExited) 'A reused/mismatched Windows PID was terminated.'
    $record.StartTicks = $process.StartTime.ToUniversalTime().Ticks.ToString()
    Write-RuntimeWindow $record
    Request-RuntimeWindowStop
    $cancelled = $false
    try { Wait-RuntimeWindowRegistration $token } catch { $cancelled = $_.Exception.Message -like '*cancelled*' }
    Assert $cancelled 'A cancelled pending runtime was allowed to start.'
    Stop-RuntimeWindow
    Assert ($process.WaitForExit(5000)) 'The owned tracking console survived Stop.'
    Assert ((Read-RuntimeWindow).Stopping) 'Cleanup forgot the cancellation token for delayed WSL startup.'
    Write-Host 'OK: hidden-runtime ownership, PID reuse protection and startup cancellation.'
} finally {
    if ($process) { if (!$process.HasExited) { $process.Kill(); $null = $process.WaitForExit(5000) }; $process.Dispose() }
    Remove-Item -Recurse -Force -LiteralPath $temporary
}
