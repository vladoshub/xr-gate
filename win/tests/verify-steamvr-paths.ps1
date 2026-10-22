#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-steamvr-' + [Guid]::NewGuid())
$previousLocalAppData = $env:LOCALAPPDATA
New-Item -ItemType Directory $temporary | Out-Null
try {
    $env:LOCALAPPDATA = $temporary
    $script:StateDir = $temporary
    $script:StateFile = Join-Path $temporary 'state.json'
    $missing = 'XR_GATE_MISSING_TEST_DRIVE:\SteamLibrary'
    $script:TestSteamRoot = Join-Path $temporary 'Steam with spaces'
    $available = Join-Path $script:TestSteamRoot 'steamapps/common/SteamVR'
    $reg = Join-Path $available 'bin/win64/vrpathreg.exe'
    New-Item -ItemType Directory -Force (Split-Path -Parent $reg) | Out-Null
    '' | Set-Content -LiteralPath $reg
    function Get-ItemProperty {
        [CmdletBinding()]param([string]$Path)
        return [pscustomobject]@{SteamPath=$script:TestSteamRoot}
    }
    $vpath = Join-Path $temporary 'openvr/openvrpaths.vrpath'
    New-Item -ItemType Directory (Split-Path -Parent $vpath) | Out-Null
    @{runtime=@($missing); external_drivers=@($missing)} | ConvertTo-Json |
        Set-Content -LiteralPath $vpath -Encoding UTF8
    ('"libraryfolders" { "1" { "path" "' + $missing.Replace('\','\\') + '" } }') |
        Set-Content -LiteralPath (Join-Path $script:TestSteamRoot 'steamapps/libraryfolders.vdf')
    $SteamVR = ''
    Assert ((Resolve-Path (Get-SteamVrDirectory)).Path -eq (Resolve-Path $available).Path) 'Stale OpenVR/library paths prevented discovery of the available SteamVR installation.'
    $script:TestSteamRoot = $missing
    @{runtime=@($available)} | ConvertTo-Json | Set-Content -LiteralPath $vpath -Encoding UTF8
    Assert ((Resolve-Path (Get-SteamVrDirectory)).Path -eq (Resolve-Path $available).Path) 'An unavailable Steam registry root hid the valid OpenVR runtime.'
    @{runtime=@($missing)} | ConvertTo-Json | Set-Content -LiteralPath $vpath -Encoding UTF8
    Assert (!(Get-SteamVrDirectory)) 'Missing SteamVR locations were not treated as unavailable.'
    $SteamVR = $missing
    $rejected = $false
    try { Get-SteamVrDirectory } catch { $rejected = $_.Exception.Message -like '*XR_STEAMVR_PATH*' }
    Assert $rejected 'An invalid explicit SteamVR override did not explain which setting to correct.'

    $driver = Join-Path $temporary 'managed-driver'
    New-Item -ItemType Directory $driver | Out-Null
    '{"name":"xr-test"}' | Set-Content -LiteralPath (Join-Path $driver 'driver.vrdrivermanifest')
    $script:State.DriverPath = $driver
    $beforeWarnings = $script:ActionWarnings.Count
    Register-XrDriver -AllowDeferred
    Assert ($script:ActionWarnings.Count -gt $beforeWarnings) 'Installation failed to defer registration with a warning for an invalid SteamVR override.'
    Assert (!$script:State.RegisteredDriver) 'Deferred registration was incorrectly recorded as complete.'

    function Get-Process {
        [CmdletBinding()]param([string[]]$Name)
        return @()
    }
    $script:NativeCalls = New-Object 'System.Collections.Generic.List[object]'
    function Invoke-Native([string]$File, [string[]]$Arguments) {
        $script:NativeCalls.Add(@{File=$File; Arguments=$Arguments})
    }
    $oldDriver = Join-Path $temporary 'old-driver'
    New-Item -ItemType Directory $oldDriver | Out-Null
    '{"name":"xr-test"}' | Set-Content -LiteralPath (Join-Path $oldDriver 'driver.vrdrivermanifest')
    @{runtime=@($available); external_drivers=@($missing,$oldDriver)} | ConvertTo-Json |
        Set-Content -LiteralPath $vpath -Encoding UTF8
    $SteamVR = $available
    Register-XrDriver
    Assert ($script:State.RegisteredDriver -eq $driver) 'A missing external-driver disk prevented registration.'
    Assert ($script:NativeCalls.Count -eq 3) 'Registration did not remove the verified old package and add/show the new package.'
    Assert ($script:NativeCalls[0].Arguments[0] -eq 'removedriver' -and $script:NativeCalls[0].Arguments[1] -eq $oldDriver) 'Registration removed an unverified stale external driver.'
    @{runtime=@($available)} | ConvertTo-Json | Set-Content -LiteralPath $vpath -Encoding UTF8
    Register-XrDriver
    'OK SteamVR discovery, unavailable drives, explicit overrides, deferred installation and stale external drivers'
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
