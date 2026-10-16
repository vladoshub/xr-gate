#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('List', 'Bind', 'Attach', 'Detach')][string]$Action = 'List',
    [string[]]$BusId = @(),
    [string]$Distribution = 'Ubuntu-24.04'
)
. "$PSScriptRoot\Common.ps1"
if (-not (Get-Command usbipd.exe -ErrorAction SilentlyContinue)) {
    throw 'usbipd-win is missing. Run Setup-Windows.ps1, then reopen PowerShell.'
}
if ($Action -eq 'List') {
    & usbipd.exe list
    if ($LASTEXITCODE -ne 0) { throw 'usbipd list failed.' }
    return
}
if ($BusId.Count -eq 0) { throw 'Specify the XREAL and/or USB Bluetooth BUSIDs shown by -Action List.' }
foreach ($id in $BusId) {
    if ($id -notmatch '^\d+-\d+$') { throw "Invalid USB BUSID: $id" }
}
if ($Action -eq 'Bind') {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Bind requires an elevated PowerShell. Attach/Detach can run without elevation.'
    }
}
if ($Action -eq 'Attach') {
    Assert-XrWsl
    & wsl.exe --distribution $Distribution --exec true
    if ($LASTEXITCODE -ne 0) { throw "Cannot start $Distribution." }
}
foreach ($id in $BusId) {
    switch ($Action) {
        'Bind'   { & usbipd.exe bind --busid $id }
        'Attach' { & usbipd.exe attach --wsl --busid $id }
        'Detach' { & usbipd.exe detach --busid $id }
    }
    if ($LASTEXITCODE -ne 0) { throw "usbipd $Action failed for $id." }
}
if ($Action -eq 'Attach') {
    Invoke-XrWsl -Distribution $Distribution -LinuxArguments @('bash', '-c', 'modprobe uvcvideo; modprobe btusb; lsusb; bluetoothctl list')
    Write-Host 'Run XrGate.ps1 -Command Doctor. Restart the container after USB reattachment.'
}
