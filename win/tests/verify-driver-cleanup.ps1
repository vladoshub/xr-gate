#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-driver-cleanup-' + [Guid]::NewGuid().ToString('N'))
$previousLocal = $env:LOCALAPPDATA
$script:StateDir = Join-Path $temporary 'XR-Gate-WSL'
$script:StateFile = Join-Path $script:StateDir 'state.json'
$script:Settings['XR_TARGET_DEVICE'] = 'fixture'
$script:State.FrequencyHz = 60; $script:State.DisplayMode = 'extended_sbs'
$script:Unregistered = New-Object 'System.Collections.Generic.List[string]'
$script:FailPath = ''
function Assert-SteamVrStopped { }
function Get-SteamVrDirectory { return (Join-Path $temporary 'SteamVR') }
function Write-Registry($Paths) {
    $file = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
    New-Item -ItemType Directory -Force (Split-Path -Parent $file) | Out-Null
    @{external_drivers=@($Paths)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $file -Encoding UTF8
}
function New-Package($Path, $Name='xr_tracking') {
    foreach ($relative in @('bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $file = Join-Path $Path $relative
        New-Item -ItemType Directory -Force (Split-Path -Parent $file) | Out-Null
        [IO.File]::WriteAllText($file, 'fixture')
    }
    @{name=$Name} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Path 'driver.vrdrivermanifest') -Encoding UTF8
    @{xr_tracking=@{deviceProfile='fixture';displayFrequency=60;isDisplayOnDesktop=$true}} | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $Path 'resources/settings/default.vrsettings') -Encoding UTF8
    return $Path
}
function Invoke-Native($File, $Arguments) {
    Assert ($File -like '*vrpathreg.exe' -and $Arguments[0] -eq 'removedriver') 'Cleanup used a global driver-name removal or an unrelated command.'
    $path = [string]$Arguments[1]
    Assert (Test-XrManagedDriverPath $path) 'Cleanup tried to unregister an unowned path.'
    if ($path -eq $script:FailPath) { throw 'fixture unregister failure' }
    $script:Unregistered.Add($path)
    Write-Registry @(Get-XrExternalDriverPaths | Where-Object { $_ -ne $path })
}
try {
    $env:LOCALAPPDATA = Join-Path $temporary 'local'
    New-Item -ItemType Directory -Force $script:StateDir | Out-Null
    $source = New-Package (Join-Path $temporary 'source/xr_tracking')
    Install-SelectedWindowsDriver $source 'vFixture'
    $owned = [string]$script:State.DriverPath
    $legacy = New-Package (Join-Path $script:StateDir 'drivers/vOld-90Hz-extended_sbs-AAAAAAAAAAAAAAAAAAAAAAAA/xr_tracking')
    $foreign = New-Package (Join-Path $temporary 'manual/xr_tracking')
    $inside = New-Package (Join-Path $script:StateDir 'drivers/other/vOther-60Hz-extended_sbs-BBBBBBBBBBBBBBBBBBBBBBBB/xr_tracking') 'unrelated_driver'
    $lookalike = New-Package (Join-Path $temporary 'XR-Gate-WSL/drivers-manual/vOther-60Hz-extended_sbs-CCCCCCCCCCCCCCCCCCCCCCCC/xr_tracking')
    $stale = Join-Path $script:StateDir 'drivers/vGone-60Hz-direct-DDDDDDDDDDDDDDDDDDDDDDDD/xr_tracking'
    Write-Registry @($owned,$legacy,$foreign,$inside,$lookalike,$stale)
    $paths = @(Get-XrManagedDriverPaths)
    Assert ($paths.Count -eq 3 -and $owned -in $paths -and $legacy -in $paths -and $stale -in $paths) 'Owned, legacy or already-missing launcher packages were not identified exactly.'
    Assert (!(Test-XrManagedDriverPath $foreign) -and !(Test-XrManagedDriverPath $inside) -and !(Test-XrManagedDriverPath $lookalike)) 'Shared driver name, an unrelated manifest or a path-prefix lookalike was treated as launcher ownership.'

    $link = Join-Path $owned 'external-resources'
    $kind = if ($env:OS -eq 'Windows_NT') { 'Junction' } else { 'SymbolicLink' }
    New-Item -ItemType $kind -Path $link -Target $foreign | Out-Null
    $rejected = $false
    try { Remove-XrManagedDrivers | Out-Null } catch { $rejected = $_.Exception.Message -like '*link or junction*' }
    Assert ($rejected -and !$script:Unregistered.Count -and (Test-Path -LiteralPath $foreign)) 'Cleanup followed a link or changed registration before validating its owned tree.'
    (Get-Item -LiteralPath $link -Force).Delete()

    $script:FailPath = $legacy
    $rejected = $false
    try { Remove-XrManagedDrivers | Out-Null } catch { $rejected = $_.Exception.Message -eq 'fixture unregister failure' }
    Assert ($rejected -and (Test-Path -LiteralPath $owned) -and (Test-Path -LiteralPath $legacy)) 'Failed unregistration deleted driver files instead of allowing a retry.'
    $script:FailPath = ''
    $removed = Remove-XrManagedDrivers
    Assert ($removed -eq 2 -and !(Test-Path -LiteralPath $owned) -and !(Test-Path -LiteralPath $legacy)) 'Retry did not remove the remaining owned driver copies.'
    $remaining = @(Get-XrExternalDriverPaths)
    Assert ($remaining.Count -eq 3 -and $foreign -in $remaining -and $inside -in $remaining -and $lookalike -in $remaining) 'Cleanup changed unrelated registrations.'
    foreach ($path in @($foreign,$inside,$lookalike)) { Assert (Test-Path -LiteralPath (Join-Path $path 'driver.vrdrivermanifest')) 'An unrelated driver file was removed.' }
    Write-Host 'OK: scoped driver removal, ownership markers, legacy/stale packages, unrelated registrations, links and retry after failure.'
} finally {
    $env:LOCALAPPDATA = $previousLocal
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
