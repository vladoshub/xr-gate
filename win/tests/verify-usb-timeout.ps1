#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-usb-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
$childPid = 0
try {
    $fixture = Join-Path $temporary "O'Brien USB fixture.ps1"
    $pidFile = Join-Path $temporary 'child.pid'
    $body = @'
param([string]$Mode, [string]$PidFile)
$OutputEncoding = [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
if ($Mode -eq 'ready') { Write-Output '{"Devices":[{"BusId":"2-7","Description":"fixture USB"}]}'; exit 0 }
if ($Mode -eq 'failure') { Write-Output 'fixture failure'; exit 7 }
[IO.File]::WriteAllText($PidFile, [string]$PID)
if ($Mode -eq 'tree') {
    $shell = (Get-Process -Id $PID).Path
    $child = Start-Process -FilePath $shell -NoNewWindow -PassThru -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive','-Command','Start-Sleep -Seconds 30')
    [IO.File]::WriteAllText(($PidFile + '.leaf'), [string]$child.Id)
}
Write-Output 'fixture attach started'
[Console]::Error.WriteLine('fixture attach waiting')
Start-Sleep -Seconds 30
'@
    [IO.File]::WriteAllText($fixture, $body, (New-Object Text.UTF8Encoding($true)))
    $shell = (Get-Process -Id $PID).Path
    $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-File',$fixture,'ready')
    $result = Invoke-UsbipdCommand -File $shell -Arguments $arguments -TimeoutMilliseconds 10000
    Assert ($result.ExitCode -eq 0 -and ($result.Output | ConvertFrom-Json).Devices[0].BusId -eq '2-7') 'Native stdout was lost or mixed with stderr.'
    $arguments[-1] = 'failure'
    $result = Invoke-UsbipdCommand -File $shell -Arguments $arguments -TimeoutMilliseconds 10000
    Assert ($result.ExitCode -eq 7 -and $result.Output -eq 'fixture failure') 'Native failure code/output was lost.'
    $arguments[-1] = 'hang'
    $arguments += $pidFile
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $rejected = $false
    try { Invoke-UsbipdCommand -File $shell -Arguments $arguments -TimeoutMilliseconds 3000 | Out-Null }
    catch { $rejected = $_.Exception.Message.Contains('did not finish within') }
    Assert ($rejected -and $watch.Elapsed.TotalSeconds -lt 8) 'A hung USB command exceeded its deadline.'
    Assert (Test-Path -LiteralPath $pidFile) 'The hanging fixture never started.'
    $childPid = [int][IO.File]::ReadAllText($pidFile)
    Assert ($null -eq (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) 'A timed-out native USB process survived.'
    $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-File',$fixture,'ready')
    Assert ((Invoke-UsbipdCommand -File $shell -Arguments $arguments -TimeoutMilliseconds 10000).ExitCode -eq 0) 'A timeout blocked the next USB command.'
    if ($env:OS -eq 'Windows_NT') {
        $arguments[-1] = 'tree'
        $arguments += $pidFile
        $rejected = $false
        try { Invoke-UsbipdCommand -File $shell -Arguments $arguments -TimeoutMilliseconds 4000 | Out-Null }
        catch { $rejected = $_.Exception.Message.Contains('did not finish within') }
        Assert $rejected 'The native process-tree fixture did not time out.'
        $leaf = [int][IO.File]::ReadAllText(($pidFile + '.leaf'))
        Assert ($null -eq (Get-Process -Id $leaf -ErrorAction SilentlyContinue)) 'A timed-out USB command left its child running.'
    }

    # A reboot/re-enumeration changes BUSID while the selected instance remains
    # the same. Resolve it again instead of attaching another device at an old ID.
    $script:BtInstance = 'USB\VID_ABCD&PID_0123\SELECTED'
    function Get-UsbDevices { return @([pscustomobject]@{InstanceId=$script:BtInstance;BusId='9-3'}) }
    Assert ((Get-CurrentUsbDevice $script:BtInstance).BusId -eq '9-3') 'USB BUSID was read from stale saved state.'
    $rejected = $false
    try { Get-CurrentUsbDevice 'USB\VID_ABCD&PID_0123\DISCONNECTED' | Out-Null }
    catch { $rejected = $_.Exception.Message.Contains('disconnected') }
    Assert $rejected 'A missing selected USB instance silently fell back to another device.'
    'OK: bounded native USB command, output/exit codes, timeout cleanup, retry and changed BUSID'
} finally {
    if ($childPid -and (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) { Stop-Process -Id $childPid -Force }
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
