# A hidden probe must never wait for an interactive username/password prompt.
function New-XrLoginProbe {
    if (!('XrGateWsl.NativeCommand' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'NativeCommand.cs') }
    $arguments = [string[]]@('--distribution', [string]$script:State.Distribution, '--exec', 'id', '-un')
    return New-Object XrGateWsl.NativeCommand((Get-Tool 'wsl.exe'), $arguments, $true)
}
function Get-XrLoginUser([int]$TimeoutMilliseconds = 8000) {
    $probe = New-XrLoginProbe
    $text = New-Object Text.StringBuilder
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($clock.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
            $count = 0
            while ($count -lt 128 -and $null -ne ($line = $probe.ReadOutput())) {
                $count++
                if (!$line.IsError) { [void]$text.AppendLine($line.Text) }
            }
            if ($probe.Finished -and $count -lt 128) {
                $name = ($text.ToString() -replace '\x00', '').Trim()
                if ($probe.ExitCode -eq 0 -and $name -ne 'root' -and $name -match '^[a-z_][a-z0-9_-]*[$]?$') { return $name }
                return ''
            }
            Start-Sleep -Milliseconds 25
        }
        return ''
    } finally { $probe.Dispose() }
}
function Open-XrFirstLogin([string]$Wsl) {
    $script:NextStep = 'ubuntu_first_login'
    if ($Launcher) { Write-Step 'CREATE YOUR UBUNTU USERNAME AND PASSWORD IN THE UBUNTU WINDOW. THEN TYPE exit. INSTALLATION WILL CONTINUE AUTOMATICALLY.' }
    else { Write-Step 'CREATE YOUR UBUNTU USERNAME AND PASSWORD. TYPE exit, THEN RUN -Action Install AGAIN.' }
    $process = Start-Process -FilePath $Wsl -PassThru -ArgumentList @('--distribution', [string]$script:State.Distribution)
    try {
        $script:ActionData = @{distribution=[string]$script:State.Distribution;
            first_login=@{process_id=$process.Id; start_ticks=$process.StartTime.ToUniversalTime().Ticks.ToString()}}
    } finally { $process.Dispose() }
}
