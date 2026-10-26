function Get-XrSteamVrProcesses {
    $processes = @(Get-Process -Name vrmonitor,vrserver,vrcompositor -ErrorAction SilentlyContinue)
    if (!$processes.Count) { return }
    $session = (Get-Process -Id $PID).SessionId
    foreach ($process in $processes) {
        if ($process.SessionId -eq $session) { $process } else { $process.Dispose() }
    }
}
function Request-XrSteamVrQuit([object[]]$Processes) {
    # SteamVR's own Exit VR action uses this protocol. Only call it when
    # SteamVR is already running, so Stop cannot launch a new VR session.
    if (Test-Path -LiteralPath 'Registry::HKEY_CLASSES_ROOT\vrmonitor\shell\open\command') {
        Start-Process 'vrmonitor://quit'
        return
    }
    foreach ($process in $Processes) {
        if ($process.ProcessName -eq 'vrmonitor') { $null = $process.CloseMainWindow() }
    }
}
function Stop-XrSteamVr([int]$TimeoutMilliseconds = 0) {
    if ((Get-XrSetting 'XR_STOP_STEAMVR' '1') -eq '0') { return }
    $processes = @()
    try {
        $processes = @(Get-XrSteamVrProcesses)
        if (!$processes.Count) { return }
        if (!$TimeoutMilliseconds) { $TimeoutMilliseconds = [int](Get-XrSetting 'XR_STEAMVR_STOP_TIMEOUT' '10') * 1000 }
        Write-Step 'Requesting SteamVR exit...'
        Request-XrSteamVrQuit -Processes $processes
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($clock.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
            $remaining = @($processes | Where-Object { !$_.HasExited })
            if (!$remaining.Count) { Write-Step 'SteamVR stopped.'; return }
            Start-Sleep -Milliseconds 100
        }
        Write-XrWarning 'SteamVR has not exited. Finish any confirmation in SteamVR or close it manually. XR Gate tracking and USB cleanup will continue.'
    } catch {
        Write-XrWarning ('Could not request SteamVR exit: ' + $_.Exception.Message + ' Close SteamVR manually; XR Gate cleanup will continue.')
    } finally { foreach ($process in $processes) { $process.Dispose() } }
}
