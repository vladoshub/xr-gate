#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'windows/Driver.ps1')
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:SelectedDevice = 'alpha'
function Get-XrSetting([string]$Key, [string]$Default = '') {
    if ($Key -eq 'XR_TARGET_DEVICE') { return $script:SelectedDevice }
    return $Default
}
function Save-State { }
function Find-BundledDriver([string]$Root) { return Get-XrSelectedDriverPackage $Root }
$script:State = @{FrequencyHz=60; DisplayMode='extended_sbs'; DriverPath=''; ReleaseTag='vFixture'}
$folder = Join-Path ([IO.Path]::GetTempPath()) ('xr-driver-devices-' + [Guid]::NewGuid().ToString('N'))
$script:StateDir = Join-Path $folder 'state'
$script:FixtureRoot = Join-Path $folder 'bundle'
function New-FixtureDriver([string]$Device, [int]$Frequency, [string]$Relative = '') {
    if (!$Relative) { $Relative = "drivers/$Device/${Frequency}Hz-extended_sbs/xr_tracking" }
    $package = Join-Path $script:FixtureRoot $Relative
    foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $path = Join-Path $package $name
        New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, 'fixture')
    }
    @{xr_tracking=@{deviceProfile=$Device;displayFrequency=$Frequency;isDisplayOnDesktop=$true}} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'resources/settings/default.vrsettings') -Encoding UTF8
    return $package
}
try {
    $alpha = New-FixtureDriver alpha 60
    $beta = New-FixtureDriver beta 60
    $null = New-FixtureDriver beta 90
    $manifest = @{schema=2; release_tag='vFixture'; devices=@('alpha','beta'); variants=@(
        @{device='alpha';frequency_hz=60;display_mode='extended_sbs';path='drivers/alpha/60Hz-extended_sbs/xr_tracking'},
        @{device='beta';frequency_hz=60;display_mode='extended_sbs';path='drivers/beta/60Hz-extended_sbs/xr_tracking'},
        @{device='beta';frequency_hz=90;display_mode='extended_sbs';path='drivers/beta/90Hz-extended_sbs/xr_tracking'}
    )}
    $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $script:FixtureRoot 'bundle.json')
    Assert ((Get-XrSelectedDriverPackage $script:FixtureRoot) -eq $alpha) 'Same-Hz devices were confused.'
    $variants = @(Get-XrDeviceDriverVariants $script:FixtureRoot)
    Assert ($variants.Count -eq 1 -and $variants[0].device -eq 'alpha') 'Another device leaked into the frequency list.'
    $selection = Resolve-DriverFrequency -Available @($variants | ForEach-Object {[int]$_.frequency_hz}) -Detected 90
    Assert ($selection.Frequency -eq 60 -and $selection.Fallback) 'Another device supplied the 90-Hz variant.'
    $script:SelectedDevice = 'missing'
    $rejected = $false
    try { Get-XrSelectedDriverPackage $script:FixtureRoot } catch { $rejected = $_.Exception.Message.Contains("No SteamVR driver for device 'missing'") }
    Assert $rejected 'Missing hardware silently used another device driver.'
    $script:SelectedDevice = 'alpha'
    Install-SelectedWindowsDriver $alpha 'vFixture'
    Assert ($script:State.DriverDevice -eq 'alpha' -and $script:State.DriverPath.Replace('\','/').Contains('/drivers/alpha/')) 'Cache is not device-specific.'
    $script:SelectedDevice = 'beta'
    Assert (!(Test-XrDriverForSelectedDevice $script:State.DriverPath)) 'Hardware change kept an incompatible driver.'
    function Select-AutomaticDriverFrequency { $script:ResolvedDriverRoot = $script:FixtureRoot }
    function Assert-SteamVrStopped { }
    function Ensure-XrDriverRegistration { }
    Update-AutomaticWindowsDriver
    Assert ($script:State.DriverDevice -eq 'beta' -and $script:State.DriverPath.Replace('\','/').Contains('/drivers/beta/')) 'Same-Hz hardware change did not install its driver.'
    $script:SelectedDevice = 'alpha'
    @{xr_tracking=@{deviceProfile='beta';displayFrequency=60;isDisplayOnDesktop=$true}} | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $alpha 'resources/settings/default.vrsettings')
    $rejected = $false
    try { Get-XrDeviceDriverVariants $script:FixtureRoot } catch { $rejected = $_.Exception.Message.Contains('does not match folder') }
    Assert $rejected 'A mislabeled folder was accepted.'
    $legacy = New-FixtureDriver beta 60 'drivers/60Hz-extended_sbs/xr_tracking'
    @{schema=1; variants=@(@{frequency_hz=60;display_mode='extended_sbs';path='drivers/60Hz-extended_sbs/xr_tracking'})} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $script:FixtureRoot 'bundle.json')
    $rejected = $false
    try { Get-XrSelectedDriverPackage $script:FixtureRoot } catch { $rejected = $true }
    Assert $rejected 'Legacy packages bypassed the selected device.'
    $script:SelectedDevice = 'beta'
    Assert ((Get-XrSelectedDriverPackage $script:FixtureRoot) -eq $legacy) 'Matching legacy package was rejected.'
} finally {
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
}
Write-Host 'OK: device-specific driver selection, refresh, metadata, cache and hardware changes.'
