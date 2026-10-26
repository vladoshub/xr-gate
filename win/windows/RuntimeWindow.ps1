# Own the tracking PowerShell window separately from transient action processes.
# PID plus creation time prevents Stop from terminating a reused Windows PID.
function Get-RuntimeWindowPath { return Join-Path $script:StateDir 'runtime-window.json' }
function Get-RuntimeWindowFiles($Record) {
    if (!$Record -or $Record.Token -notmatch '^[a-f0-9]{32}$') { return $null }
    $prefix = Join-Path $script:StateDir ('runtime-' + $Record.Token)
    return @{Output=($prefix + '.stdout.log'); Error=($prefix + '.stderr.log'); Result=($prefix + '.result.json')}
}
function Invoke-LoggedRuntime([string[]]$Arguments, $Files) {
    if (!$Files) { throw 'The launcher did not register the runtime log.' }
    if (!('XrGateWsl.NativeCommand' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'NativeCommand.cs') }
    $encoding = New-Object Text.UTF8Encoding($false)
    $stdout = New-Object IO.StreamWriter($Files.Output, $false, $encoding)
    $stderr = New-Object IO.StreamWriter($Files.Error, $false, $encoding)
    $stdout.AutoFlush = $stderr.AutoFlush = $true
    $command = $null
    try {
        $command = New-Object XrGateWsl.NativeCommand((Get-Tool 'wsl.exe'), $Arguments)
        while ($true) {
            $count = 0
            while ($count -lt 128 -and $null -ne ($line = $command.ReadOutput())) {
                $count++
                if ($line.IsError) { $stderr.WriteLine($line.Text) } else { $stdout.WriteLine($line.Text) }
            }
            if ($command.Finished -and $count -lt 128) { return $command.ExitCode }
            Start-Sleep -Milliseconds 25
        }
    } finally {
        if ($command) { $command.Dispose() }
        $stdout.Dispose(); $stderr.Dispose()
    }
}
function Get-RuntimeExitObservation($Record) {
    $message = 'XR Gate exited in Ubuntu.'
    $lines = @()
    $files = Get-RuntimeWindowFiles $Record
    if ($files) {
        try {
            if (Test-Path -LiteralPath $files.Result) {
                $result = Get-Content -Raw -Encoding UTF8 -LiteralPath $files.Result | ConvertFrom-Json
                if ($result.message) { $message = [string]$result.message }
            }
        } catch { }
        foreach ($file in @($files.Output,$files.Error)) {
            try {
                # Get-Content attaches provider metadata to strings. Strip it
                # before Windows PowerShell 5.1 serializes the status as JSON.
                if (Test-Path -LiteralPath $file) {
                    $lines += @(Get-Content -Tail 30 -Encoding UTF8 -LiteralPath $file | ForEach-Object { $_.ToString() })
                }
            } catch { }
        }
    }
    $failure = @($lines | Where-Object { $_ -match '(?i)\[ERROR\]|\bERROR:\s|failed to open|permission denied' } | Select-Object -First 1)
    if ($failure.Count) { $message = [string]$failure[0] }
    return @{message=$message; log_tail=@($lines | Where-Object { $_ })}
}
function Show-RuntimeExit($Record) {
    $exit = Get-RuntimeExitObservation $Record
    foreach ($line in $exit.log_tail) { Write-Host ('[Runtime] ' + $line) }
    throw ($exit.message + ' Tracking stopped before a valid HMD stream became ready.')
}
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
