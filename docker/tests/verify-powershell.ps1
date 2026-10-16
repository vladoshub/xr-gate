#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$bundle = Split-Path $PSScriptRoot -Parent
foreach ($file in Get-ChildItem "$bundle/windows/*.ps1") {
    $tokens = $null
    $errorsFound = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errorsFound)
    if ($errorsFound.Count -gt 0) { throw ($errorsFound | Out-String) }
}
$global:XrNativeCalls = New-Object 'System.Collections.Generic.List[object]'
function global:wsl.exe {
    $global:XrNativeCalls.Add([string[]]$args)
    $global:LASTEXITCODE = 0
    if ($args -contains 'printenv') { return '/home/test user' }
    if ($args -contains 'wslpath') { return '/mnt/c/Bundle with spaces' }
}
function global:usbipd.exe {
    $global:XrNativeCalls.Add([string[]]$args)
    $global:LASTEXITCODE = 0
}
try {
    & "$bundle/windows/XrGate.ps1" -Command Run -LinuxPath '/home/test user/xr-gate-docker' -Bluetooth -Video -ClientArguments @('--no-imu')
    $call = $global:XrNativeCalls[-1]
    if ($call[$call.IndexOf('--cd') + 1] -ne '/home/test user/xr-gate-docker') { throw 'Linux path argument was split.' }
    if ($call -notcontains 'XR_BLUETOOTH=1' -or $call -notcontains 'XR_VIDEO_TCP=1' -or $call -notcontains '--no-imu') { throw 'Missing runtime arguments.' }
    & "$bundle/windows/Install-XrGate.ps1" -LinuxPath '/home/test user/xr-gate-docker' -SkipBuild
    $copyCalls = @($global:XrNativeCalls | Where-Object { $_ -contains '/mnt/c/Bundle with spaces/scripts/install-copy.sh' })
    if ($copyCalls.Count -ne 1) { throw 'Install must pass a script filename as one argument.' }
    & "$bundle/windows/Usb-XrGate.ps1" -Action Attach -BusId @('1-2', '1-3')
    $attachCalls = @($global:XrNativeCalls | Where-Object { $_ -contains 'attach' })
    if ($attachCalls.Count -ne 2) { throw 'Both explicit USB devices must be attached.' }
    if ($attachCalls[0] -notcontains '--wsl' -or $attachCalls[1] -notcontains '1-3') { throw 'USB attach arguments are incorrect.' }
    Write-Host 'OK PowerShell parsing and mocked WSL/USB argument forwarding (including paths with spaces).'
} finally {
    Remove-Item Function:\wsl.exe, Function:\usbipd.exe
}
