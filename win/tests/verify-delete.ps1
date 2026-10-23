#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-delete-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
$script:StateDir = $temporary; $script:StateFile = Join-Path $temporary 'state.json'
$script:Calls = New-Object 'System.Collections.Generic.List[string]'
$script:Failure = ''; $script:Registered = @('Ubuntu-24.04','Ubuntu-other','docker-desktop')
function Reset-Fixture {
    $script:State = [ordered]@{Distribution='Ubuntu-24.04';LinuxUser='fixture';ReleaseTag='v1';InstallPending=$false;
        InstallNextStep='';AttachedUsbInstanceIds=@('selected');CleanupSettings=@{};DisplayDeviceId='saved-display'}
    Save-State; $script:Calls.Clear(); $script:Failure = ''
}
function Get-XrRegisteredDistributions { return $script:Registered }
function Request-RuntimeWindowStop { $script:Calls.Add('cancel-startup') }
function Invoke-Linux($Operation, $Request) { $script:Calls.Add('stop-linux'); if ($script:Failure -eq 'linux') { throw 'fixture Linux cleanup failed' } }
function New-LinuxRequest { return @{} }
function Stop-RuntimeWindow { $script:Calls.Add('stop-window') }
function Disconnect-SelectedUsb { $script:Calls.Add('return-usb'); if ($script:Failure -eq 'usb') { throw 'fixture USB cleanup failed' } }
function Get-Tool($Name) { return $Name }
function Invoke-Native($File, $Arguments) {
    Assert ($File -eq 'wsl.exe' -and $Arguments.Count -eq 2 -and $Arguments[0] -eq '--unregister' -and $Arguments[1] -ceq 'Ubuntu-24.04') 'Deletion targeted an unconfirmed distribution or all of WSL.'
    $script:Calls.Add('unregister'); if ($script:Failure -eq 'unregister') { throw 'fixture unregister failed' }
}
try {
    Reset-Fixture
    foreach ($target in @('', 'Ubuntu-other', 'docker-desktop', 'Ubuntu-24.04;bad')) {
        $rejected = $false
        try { Remove-XrDistribution $target } catch { $rejected = $true }
        Assert ($rejected -and $script:Calls.Count -eq 0) 'An absent, changed or invalid confirmation changed the installation.'
    }
    foreach ($failureCase in @('linux','usb','unregister')) {
        Reset-Fixture; $script:Failure = $failureCase
        $rejected = $false
        try { Remove-XrDistribution 'Ubuntu-24.04' } catch { $rejected = $true }
        Assert ($rejected -and $script:State.LinuxUser -eq 'fixture') 'A failed deletion cleared the installation state.'
        if ($failureCase -ne 'unregister') { Assert ('unregister' -notin $script:Calls) 'Deletion continued after cleanup failed.' }
    }
    Reset-Fixture
    [IO.File]::WriteAllText((Get-RuntimeWindowPath), 'fixture old runtime')
    Remove-XrDistribution 'Ubuntu-24.04'
    Assert (($script:Calls -join ',') -eq 'cancel-startup,stop-linux,stop-window,return-usb,unregister') 'Deletion did not clean tracking and USB before unregistering.'
    $saved = Get-Content -Raw -LiteralPath $script:StateFile | ConvertFrom-Json
    Assert (!$saved.LinuxUser -and !$saved.InstallPending -and !$saved.AttachedUsbInstanceIds.Count -and $saved.DisplayDeviceId -eq 'saved-display') 'Successful deletion left stale setup state or erased the display choice.'
    Assert (!(Test-Path -LiteralPath (Get-RuntimeWindowPath)) -and $script:ActionData.deleted) 'Deletion retained a runtime owner or did not report success.'
    Reset-Fixture; $script:State.LinuxUser = ''; $script:State.InstallPending = $true; Save-State
    Remove-XrDistribution 'Ubuntu-24.04'
    Assert ('stop-linux' -notin $script:Calls -and 'unregister' -in $script:Calls) 'An unfinished first-login installation could not be removed.'
    Reset-Fixture; $script:Registered = @('Ubuntu-other','docker-desktop')
    Remove-XrDistribution 'Ubuntu-24.04'
    Assert ('stop-linux' -notin $script:Calls -and 'unregister' -notin $script:Calls -and !$script:State.LinuxUser) 'Already absent Ubuntu prevented clearing its stale XR Gate state.'
    Write-Host 'OK: confirmed exact-distribution deletion, stop/USB ordering, failed cleanup retention and unfinished/absent installations.'
} finally { Remove-Item -Recurse -Force -LiteralPath $temporary }
