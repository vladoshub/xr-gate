# Own the tracking PowerShell window separately from transient action processes.
# PID plus creation time prevents Stop from terminating a reused Windows PID.
function Get-RuntimeWindowPath { return Join-Path $script:StateDir 'runtime-window.json' }
function Read-RuntimeWindow {
    $path = Get-RuntimeWindowPath
    if (!(Test-Path -LiteralPath $path)) { return $null }
    $record = Get-Content -Raw -Encoding UTF8 -LiteralPath $path | ConvertFrom-Json
    if ($record.Token -notmatch '^[a-f0-9]{32}$' -or [int]$record.ProcessId -le 0 -or [string]$record.StartTicks -notmatch '^\d+$') {
        throw 'Invalid tracking console identity. Check runtime-window.json before retrying Stop.'
    }
    return $record
}
function Write-RuntimeWindow($Record) {
    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
    $path = Get-RuntimeWindowPath
    $temporary = $path + '.' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText($temporary, ($Record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
        Move-Item -Force -LiteralPath $temporary -Destination $path
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
function Get-RuntimeWindowProcess($Record) {
    if ($null -eq $Record) { return $null }
    $process = Get-Process -Id ([int]$Record.ProcessId) -ErrorAction SilentlyContinue
    if (!$process) { return $null }
    try {
        $null = $process.Handle # Pin the Windows process before checking identity.
        if ($process.StartTime.ToUniversalTime().Ticks.ToString() -eq [string]$Record.StartTicks) { return $process }
    } catch [System.InvalidOperationException] { }
    $process.Dispose()
    return $null
}
function Request-RuntimeWindowStop {
    $record = Read-RuntimeWindow
    if ($record) { $record.Stopping = $true; Write-RuntimeWindow $record }
}
function Stop-RuntimeWindow {
    $record = Read-RuntimeWindow
    $process = Get-RuntimeWindowProcess $record
    if (!$process) { return }
    try {
        # Linux Stop has already verified that every owned runtime service exited.
        if (!$process.WaitForExit(3000)) { $process.Kill() }
        if (!$process.WaitForExit(3000)) { throw 'The owned tracking console did not exit. Retry Stop.' }
    } finally { $process.Dispose() }
    # Retain the stopping token: a delayed WSL startup must still refuse to run.
}
function Wait-RuntimeWindowRegistration([string]$Token) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $record = Read-RuntimeWindow
        if ($record -and $record.Token -eq $Token) {
            if ($record.Stopping) { throw 'Runtime startup was cancelled.' }
            return
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The launcher did not register this runtime; startup was cancelled.'
}
