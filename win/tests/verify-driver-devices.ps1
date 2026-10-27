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
function Write-Step($Message) { }
$script:Warnings = New-Object 'System.Collections.Generic.List[string]'
function Write-XrWarning($Message) { $script:Warnings.Add($Message) }
function Get-AutomaticDriverBundle { return $script:FixtureRoot }
function Get-DesktopDisplays { throw 'Installation must not read or change Windows displays.' }
function Find-BundledDriver([string]$Root) { return Get-XrSelectedDriverPackage $Root }
$script:State = @{FrequencyHz=60; DisplayMode='extended_sbs'; DriverPath=''; ReleaseTag='vFixture'}
$folder = Join-Path ([IO.Path]::GetTempPath()) ('xr-driver-devices-' + [Guid]::NewGuid().ToString('N'))
$script:StateDir = Join-Path $folder 'state'
$script:FixtureRoot = Join-Path $folder 'bundle'
function New-FixtureDriver([string]$Device, [int]$Frequency, [string]$Relative = '', [string]$Mode = 'extended_sbs') {
    if (!$Relative) { $Relative = "drivers/$Device/${Frequency}Hz-$Mode/xr_tracking" }
    $package = Join-Path $script:FixtureRoot $Relative
    foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        $path = Join-Path $package $name
        New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, 'fixture')
    }
    @{xr_tracking=@{deviceProfile=$Device;displayFrequency=$Frequency;isDisplayOnDesktop=($Mode -eq 'extended_sbs')}} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'resources/settings/default.vrsettings') -Encoding UTF8
    return $package
}
try {
    $alpha = New-FixtureDriver alpha 60
    $beta = New-FixtureDriver beta 60
    $null = New-FixtureDriver beta 90
    $null = New-FixtureDriver gamma 72
    $null = New-FixtureDriver gamma 90
    $null = New-FixtureDriver delta 60 '' direct
    $manifest = @{schema=2; release_tag='vFixture'; devices=@('alpha','beta'); variants=@(
        @{device='alpha';frequency_hz=60;display_mode='extended_sbs';path='drivers/alpha/60Hz-extended_sbs/xr_tracking'},
        @{device='beta';frequency_hz=60;display_mode='extended_sbs';path='drivers/beta/60Hz-extended_sbs/xr_tracking'},
        @{device='beta';frequency_hz=90;display_mode='extended_sbs';path='drivers/beta/90Hz-extended_sbs/xr_tracking'},
        @{device='gamma';frequency_hz=72;display_mode='extended_sbs';path='drivers/gamma/72Hz-extended_sbs/xr_tracking'},
        @{device='gamma';frequency_hz=90;display_mode='extended_sbs';path='drivers/gamma/90Hz-extended_sbs/xr_tracking'},
        @{device='delta';frequency_hz=60;display_mode='direct';path='drivers/delta/60Hz-direct/xr_tracking'}
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
    $script:State.DisplayMode = 'direct'; $script:State.FrequencyHz = 90
    Select-InstallationDriver $null ''
    Assert ($script:State.FrequencyHz -eq 60 -and $script:State.DisplayMode -eq 'extended_sbs') 'Install did not select this device 60 Hz SBS without a display.'
    $script:SelectedDevice = 'gamma'
    Select-InstallationDriver $null ''
    Assert ($script:State.FrequencyHz -eq 72 -and $script:Warnings.Count -eq 1) 'A device with no 60 Hz SBS driver did not use its own lowest SBS frequency.'
    $script:SelectedDevice = 'delta'
    $rejected = $false
    try { Select-InstallationDriver $null '' } catch { $rejected = $_.Exception.Message -like '*No extended SBS*' }
    Assert $rejected 'Install silently used a direct-mode or different-device driver when no SBS package existed.'
    $script:SelectedDevice = 'alpha'; Select-InstallationDriver $null ''
    Install-SelectedWindowsDriver $alpha 'vFixture'
    Assert ($script:State.DriverDevice -eq 'alpha' -and $script:State.DriverPath.Replace('\','/').Contains('/drivers/alpha/')) 'Cache is not device-specific.'
    $owner = Get-Content -Raw -LiteralPath (Join-Path $script:State.DriverPath 'xr-gate-installed.json') | ConvertFrom-Json
    Assert ($owner.owner -ceq 'XR Gate Windows launcher') 'An installed copy had no launcher ownership marker.'
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
