# Every usbipd operation is bounded, including state queries and command-line use.
function Invoke-UsbipdCommand([string]$File, [string[]]$Arguments, [int]$TimeoutMilliseconds = 0, [switch]$StreamOutput) {
    if (!('XrGateWsl.NativeCommand' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'NativeCommand.cs')
    }
    if (!$TimeoutMilliseconds) { $TimeoutMilliseconds = (Get-XrInteger 'XR_USB_COMMAND_TIMEOUT' 5 120) * 1000 }
    $stdout = New-Object Text.StringBuilder
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = New-Object XrGateWsl.NativeCommand($File, $Arguments)
    try {
        while ($true) {
            $drained = 0
            while ($drained -lt 64 -and $null -ne ($line = $process.ReadOutput())) {
                $drained++
                if ($line.IsError) { Write-Host $line.Text }
                else {
                    [void]$stdout.AppendLine($line.Text)
                    if ($StreamOutput) { Write-Host $line.Text }
                }
            }
            if ($process.Finished -and $drained -lt 64) {
                return [pscustomobject]@{ExitCode=$process.ExitCode; Output=$stdout.ToString().TrimEnd()}
            }
            if ($clock.ElapsedMilliseconds -ge $TimeoutMilliseconds) {
                throw ('usbipd ' + ($Arguments -join ' ') + ' did not finish within ' +
                    ($TimeoutMilliseconds / 1000) + ' seconds. The command was stopped. Reconnect the selected USB device and retry; use List USB devices to check its current BUSID.')
            }
            Start-Sleep -Milliseconds 25
        }
    } finally { $process.Dispose() }
}
