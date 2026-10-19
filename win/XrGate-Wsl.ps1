#Requires -Version 5.1
<#
.SYNOPSIS
Install/run XR Gate in Ubuntu 24.04 WSL2, with native Windows SteamVR.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Install
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\XrGate-Wsl.ps1 -Action Run -StartSteamVR
#>
[CmdletBinding()]
param(
    [ValidateSet('Install','Run','Runtime','Usb','Detach','Doctor','Test','Driver','Display','Pair','Controllers','Help','Admin')]
    [string]$Action = 'Help',
    [string]$Distribution = '',
    [string]$ReleaseTag = '',
    [Alias('XrealBusId')][string[]]$UsbBusId = @(),
    [string]$BluetoothBusId = '',
    [switch]$WithoutBluetooth,
    [switch]$GearVR,
    [string]$ControllerConfig = '',
    [ValidateRange(0,1000)][int]$FrequencyHz = 0,
    [ValidateSet('','direct','extended_sbs')][string]$DisplayMode = '',
    [ValidateSet('','current','60hz','90hz','prompt')][string]$DisplayOption = '',
    [string]$DisplayName = '',
    [string]$SteamVR = '',
    [switch]$StartSteamVR,
    [ValidateRange(0,600)][int]$TimeoutSeconds = 0,
    [ValidateSet('Prerequisites','Bind')][string]$AdminTask = 'Prerequisites',
    [string[]]$BindBusId = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Repo = 'vladoshub/xr-gate'
$script:Self = $PSCommandPath
. (Join-Path $PSScriptRoot 'windows\Environment.ps1')
Initialize-XrSettings $PSScriptRoot
if (!$ReleaseTag) { $ReleaseTag = Get-XrSetting 'XR_RELEASE_TAG' 'latest' }
if (!$TimeoutSeconds) { $TimeoutSeconds = Get-XrInteger 'XR_START_TIMEOUT' 5 600 }
if (!$SteamVR) { $SteamVR = Get-XrSetting 'XR_STEAMVR_PATH' }
if (!$ControllerConfig) { $ControllerConfig = Get-XrSetting 'XR_CONTROLLER_CONFIG' }
if (!$DisplayName) { $DisplayName = Get-XrSetting 'XR_DISPLAY_NAME' }
if (!$UsbBusId.Count) { $UsbBusId = @(Get-XrList 'XR_USB_BUS_IDS') }
if (!$BluetoothBusId) { $BluetoothBusId = Get-XrSetting 'XR_BLUETOOTH_BUS_ID' }
$script:TcpHost = Get-XrSetting 'XR_TCP_HOST' '127.0.0.1'
$script:TcpPort = Get-XrInteger 'XR_TCP_PORT'

$script:StateDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'XR-Gate-WSL'
$script:StateFile = Join-Path $script:StateDir 'state.json'
$script:State = [ordered]@{
    Distribution = (Get-XrSetting 'XR_DISTRIBUTION'); LinuxUser = ''; ReleaseTag = ''
    UsbInstanceIds = @(); BluetoothInstanceId = ''
    FrequencyHz = (Get-XrInteger 'XR_DISPLAY_FREQUENCY_HZ' 1 1000)
    DisplayMode = (Get-XrSetting 'XR_DISPLAY_MODE'); DisplayOption = (Get-XrSetting 'XR_DISPLAY_OPTION')
    GearVR = $false; DriverPath = ''; RegisteredDriver = ''
    DisplayDeviceId = ''; DisplayDeviceName = ''
}
$script:LinuxExitCode = 0
$script:LinuxHelperPath = ''
. (Join-Path $PSScriptRoot 'windows\Display.ps1')

function Write-Step([string]$Message) { Write-Host ("[XR Gate] " + $Message) -ForegroundColor Cyan }
function Quote-PsLiteral([string]$Value) { return "'" + $Value.Replace("'", "''") + "'" }
function To-Base64Json($Value) {
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($Value | ConvertTo-Json -Depth 40 -Compress)))
}
function Save-State {
    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
    $temp = $script:StateFile + '.new'
    [IO.File]::WriteAllText($temp, ($script:State | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temp -Destination $script:StateFile -Force
}
function Load-State {
    if (Test-Path -LiteralPath $script:StateFile) {
        $saved = Get-Content -Raw -LiteralPath $script:StateFile | ConvertFrom-Json
        foreach ($p in $saved.PSObject.Properties) {
            if ($script:State.Contains($p.Name)) { $script:State[$p.Name] = $p.Value }
        }
    }
    # Migrate the selection saved by earlier XREAL-only launchers.
    if ((Test-Path -LiteralPath $script:StateFile) -and !$script:State.UsbInstanceIds.Count -and
        $saved.PSObject.Properties['XrealInstanceId'] -and $saved.XrealInstanceId) {
        $script:State.UsbInstanceIds = @([string]$saved.XrealInstanceId)
    }
    # A user's .env (and process environment) overrides saved interactive choices.
    $mapping = @{XR_DISTRIBUTION='Distribution'; XR_DISPLAY_FREQUENCY_HZ='FrequencyHz';
                 XR_DISPLAY_MODE='DisplayMode'; XR_DISPLAY_OPTION='DisplayOption'}
    foreach ($key in $mapping.Keys) {
        if ($script:SettingOverrides.ContainsKey($key) -and $script:SettingOverrides[$key] -ne '') {
            $script:State[$mapping[$key]] = $script:SettingOverrides[$key]
        }
    }
    if ($Distribution) { $script:State.Distribution = $Distribution }
    if ($FrequencyHz) { $script:State.FrequencyHz = $FrequencyHz }
    if ($DisplayMode) { $script:State.DisplayMode = $DisplayMode }
    if ($DisplayOption) { $script:State.DisplayOption = $DisplayOption }
    $gearVrSetting = Get-XrSetting 'XR_GEARVR'
    if ($gearVrSetting -ne '') { $script:State.GearVR = ($gearVrSetting -eq '1') }
    if ((Get-XrSetting 'XR_BLUETOOTH') -eq '0') { $script:WithoutBluetooth = $true }
    $bluetoothInstance = Get-XrSetting 'XR_BLUETOOTH_INSTANCE_ID'
    if ($bluetoothInstance -and !$BluetoothBusId) { $script:State.BluetoothInstanceId = $bluetoothInstance }
    if ($GearVR) { $script:State.GearVR = $true }
    if ($WithoutBluetooth) { $script:State.BluetoothInstanceId = '' }
    if ([int]$script:State.FrequencyHz -lt 1 -or [int]$script:State.FrequencyHz -gt 1000) { throw 'Invalid display frequency.' }
    if ($script:State.DisplayMode -notin @('direct','extended_sbs')) { throw 'Invalid XR_DISPLAY_MODE.' }
    if ($script:State.DisplayOption -notin @('current','60hz','90hz','prompt')) { throw 'Invalid XR_DISPLAY_OPTION.' }
    if ($script:State.Distribution -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw 'Distribution name contains unsupported characters.'
    }
}
function Get-Tool([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($Name -eq 'usbipd.exe') {
        $p = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'usbipd-win\usbipd.exe'
        if (Test-Path -LiteralPath $p) { return $p }
    }
    throw "$Name was not found. Run -Action Install; reopen PowerShell after installing system tools."
}
function Invoke-Native([string]$File, [string[]]$Arguments, [int[]]$AllowedCodes = @(0)) {
    & $File @Arguments | Out-Host
    $script:NativeExitCode = $LASTEXITCODE
    if ($AllowedCodes -notcontains $LASTEXITCODE) { throw "$File failed (exit $LASTEXITCODE)." }
}
function Get-WslText([string[]]$Arguments, [string]$User = '') {
    $a = @('--distribution', [string]$script:State.Distribution)
    if ($User) { $a += @('--user', $User) }
    $a += '--exec'; $a += $Arguments
    $lines = @(& (Get-Tool 'wsl.exe') @a)
    if ($LASTEXITCODE -ne 0) { throw "WSL command failed (exit $LASTEXITCODE): $($Arguments -join ' ')" }
    return (($lines -join [Environment]::NewLine) -replace '\x00', '').Trim()
}
function ConvertTo-WslPath([string]$Path) {
    return Get-WslText -Arguments @('wslpath', '-a', '-u', (Resolve-Path -LiteralPath $Path).Path)
}
function Invoke-Linux([string]$Operation, $Request, [switch]$AsRoot, [switch]$Capture) {
    if (!$script:LinuxHelperPath) {
        $helper = Join-Path $PSScriptRoot 'linux\xr_gate_wsl.py'
        if (!(Test-Path -LiteralPath $helper)) { throw 'Extract the whole ZIP, including the linux folder.' }
        $script:LinuxHelperPath = ConvertTo-WslPath $helper
    }
    $a = @('--distribution', [string]$script:State.Distribution)
    if ($AsRoot) { $a += @('--user', 'root') }
    else { $a += @('--user', [string]$script:State.LinuxUser) }
    $a += @('--exec', 'python3', $script:LinuxHelperPath, $Operation, (To-Base64Json $Request))
    if ($Capture) { $output = @(& (Get-Tool 'wsl.exe') @a) } else { & (Get-Tool 'wsl.exe') @a }
    $script:LinuxExitCode = $LASTEXITCODE
    if ($LASTEXITCODE -notin @(0,20)) { throw "Linux helper '$Operation' failed (exit $LASTEXITCODE). See the message above." }
    if ($Capture) { return (($output -join [Environment]::NewLine) -replace '\x00', '').Trim() }
}
function New-LinuxRequest {
    return @{
        user = [string]$script:State.LinuxUser; gearvr = [bool]$script:State.GearVR
        bluetooth = [bool]$script:State.BluetoothInstanceId
        display_option = [string]$script:State.DisplayOption
        settings = $script:Settings
    }
}
function Invoke-Elevated([string]$Task, [string[]]$BusIds = @()) {
    $body = '& ' + (Quote-PsLiteral $script:Self) + ' -Action Admin -AdminTask ' + (Quote-PsLiteral $Task)
    if ($BusIds.Count) {
        foreach ($id in $BusIds) { if ($id -notmatch '^\d+-\d+$') { throw 'Invalid USB BUSID.' } }
        $literals = @($BusIds | ForEach-Object { Quote-PsLiteral $_ })
        $body += ' -BindBusId @(' + ($literals -join ',') + ')'
    }
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    Write-Step 'Windows requests administrator rights for this setup step.'
    $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded)
    if ($p.ExitCode -notin @(0,20)) { throw "Administrator step failed (exit $($p.ExitCode))." }
    return $p.ExitCode
}
function Invoke-AdminTask {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'This internal action requires elevation.' }
    if ($AdminTask -eq 'Bind') {
        $usb = Get-Tool 'usbipd.exe'
        if (!$BindBusId.Count) { throw 'No USB devices to bind.' }
        foreach ($id in $BindBusId) {
            if ($id -notmatch '^\d+-\d+$') { throw 'Invalid USB BUSID.' }
            Invoke-Native $usb @('bind','--busid',$id)
        }
        return 0
    }
    $needsRestart = $false
    foreach ($feature in @('Microsoft-Windows-Subsystem-Linux','VirtualMachinePlatform')) {
        $f = Get-WindowsOptionalFeature -Online -FeatureName $feature
        if ($f.State -ne 'Enabled') {
            $result = Enable-WindowsOptionalFeature -Online -FeatureName $feature -All -NoRestart
            if ($result.RestartNeeded) { $needsRestart = $true }
        }
    }
    if ($needsRestart) { return 20 }
    Invoke-Native (Get-Tool 'wsl.exe') @('--update','--web-download') @(0,3010)
    if ($script:NativeExitCode -eq 3010) { return 20 }
    try { $null = Get-Tool 'usbipd.exe' } catch {
        Invoke-Native (Get-Tool 'winget.exe') @('install','--exact','--id','dorssel.usbipd-win','--silent','--accept-package-agreements','--accept-source-agreements','--disable-interactivity') @(0,3010)
        if ($script:NativeExitCode -eq 3010) { return 20 }
        $null = Get-Tool 'usbipd.exe'
    }
    return 0
}
function Read-WslDistributionVersion([string[]]$Lines, [string]$Distro) {
    $pattern = '^\s*\*?\s*' + [regex]::Escape($Distro) + '\s+.+?\s+(?<WslVersion>[12])\s*$'
    $clean = ($Lines -join [Environment]::NewLine) -replace '\x00', ''
    foreach ($line in ($clean -split '\r?\n')) {
        $row = $line.TrimStart([char]0xFEFF)
        $found = [regex]::Match($row, $pattern, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($found.Success) { return [int]$found.Groups['WslVersion'].Value }
    }
    throw "Cannot determine the WSL version of '$Distro'. Run wsl --list --verbose and check its VERSION column."
}
function Get-WslDistributionVersion {
    $lines = @(& (Get-Tool 'wsl.exe') --list --verbose)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read WSL distribution versions.' }
    return Read-WslDistributionVersion -Lines $lines -Distro ([string]$script:State.Distribution)
}
function Ensure-Distribution {
    $wsl = Get-Tool 'wsl.exe'
    $names = @(& $wsl --list --quiet) | ForEach-Object { ($_ -replace '\x00','').Trim() }
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list WSL distributions.' }
    if ($names -notcontains $script:State.Distribution) {
        Invoke-Native $wsl @('--set-default-version','2')
        Invoke-Native $wsl @('--install','--distribution',[string]$script:State.Distribution,'--web-download','--no-launch') @(0,3010)
        Save-State
        Write-Step 'In the new Ubuntu window create your Linux username/password, then type exit.'
        Write-Host 'After completing the first Ubuntu login, run -Action Install again.' -ForegroundColor Yellow
        Start-Process $wsl -ArgumentList @('--distribution', [string]$script:State.Distribution)
        return $false
    }
    $distroVersion = Get-WslDistributionVersion
    if ($distroVersion -eq 1) {
        Write-Step ('Converting ' + $script:State.Distribution + ' from WSL1 to WSL2...')
        Invoke-Native $wsl @('--set-version',[string]$script:State.Distribution,'2')
        if ((Get-WslDistributionVersion) -ne 2) { throw 'WSL2 conversion did not complete.' }
    } else {
        Write-Step ($script:State.Distribution + ' is already WSL2; continuing installation.')
    }
    $name = Get-WslText @('id','-un')
    if ($name -eq 'root' -or $name -notmatch '^[a-z_][a-z0-9_-]*[$]?$') {
        throw 'Ubuntu needs a regular default Linux user. Complete the first login; see START-HERE.html.'
    }
    $script:State.LinuxUser = $name
    return $true
}
function Get-ReleaseInfo([string]$Tag) {
    if ($Tag -ne 'latest' -and $Tag -notmatch '^[A-Za-z0-9._-]+$') { throw 'Invalid release tag.' }
    $suffix = if ($Tag -eq 'latest') { 'latest' } else { 'tags/' + $Tag }
    $r = Invoke-RestMethod -Uri "https://api.github.com/repos/$script:Repo/releases/$suffix" -Headers @{'User-Agent'='XR-Gate-WSL-Setup'} -TimeoutSec 45
    if ($r.tag_name -notmatch '^[A-Za-z0-9._-]+$') { throw 'Unexpected release tag returned by GitHub.' }
    return $r
}
function Select-ReleaseAsset($Release, [string]$Name) {
    $matches = @($Release.assets | Where-Object { $_.name -ceq $Name })
    if ($matches.Count -ne 1) { throw "Release $($Release.tag_name) has no unique asset $Name. Select a release that includes the requested package." }
    $asset = $matches[0]
    if (!$asset.PSObject.Properties['digest'] -or [string]$asset.digest -notmatch '^sha256:[0-9a-fA-F]{64}$') {
        throw "GitHub did not provide a SHA-256 digest for $Name."
    }
    $prefix = "https://github.com/$script:Repo/releases/download/"
    if (!([string]$asset.browser_download_url).StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Unexpected download host.' }
    return $asset
}
function Save-ReleaseAsset($Asset, [string]$Directory) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $path = Join-Path $Directory $Asset.name
    $hash = ([string]$Asset.digest).Substring(7).ToLowerInvariant()
    if (Test-Path -LiteralPath $path) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $hash) {
            Write-Step ("Using cached " + $Asset.name); return $path
        }
    }
    Write-Step ("Downloading " + $Asset.name)
    $partial = $path + '.download'; $previous = $ProgressPreference
    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -UseBasicParsing -Uri $Asset.browser_download_url -OutFile $partial -Headers @{'User-Agent'='XR-Gate-WSL-Setup'} -TimeoutSec 600
    } finally { $ProgressPreference = $previous }
    if ((Get-Item -LiteralPath $partial).Length -ne [long]$Asset.size) { throw 'Downloaded file size mismatch.' }
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
        Remove-Item -LiteralPath $partial -Force; throw "SHA-256 mismatch: $($Asset.name)"
    }
    Move-Item -LiteralPath $partial -Destination $path -Force
    return $path
}
function Find-BundledDriver([string]$Root) {
    $manifestPath = Join-Path $Root 'bundle.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return '' }
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($manifest.schema -ne 1) { throw 'Unsupported Windows bundle format.' }
    $variant = @($manifest.variants | Where-Object {
        [int]$_.frequency_hz -eq [int]$script:State.FrequencyHz -and $_.display_mode -eq $script:State.DisplayMode
    })
    if ($variant.Count -ne 1) {
        $choices = @($manifest.variants | ForEach-Object { "$($_.frequency_hz) Hz / $($_.display_mode)" }) -join ', '
        throw "The bundle contains: $choices. Set XR_DISPLAY_FREQUENCY_HZ and XR_DISPLAY_MODE in .env."
    }
    $relative = 'drivers/' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '/xr_tracking'
    $package = Join-Path $Root $relative
    foreach ($name in @('driver.vrdrivermanifest','bin/win64/driver_xr_tracking.dll','bin/win64/openvr_api.dll','resources/settings/default.vrsettings')) {
        if (!(Test-Path -LiteralPath (Join-Path $package $name) -PathType Leaf)) { throw "Windows bundle file missing: $relative/$name" }
    }
    return $package
}
function Get-DriverPackage($Release, [string]$Cache) {
    $localManifest = Join-Path $PSScriptRoot 'bundle.json'
    if (Test-Path -LiteralPath $localManifest -PathType Leaf) {
        $manifest = Get-Content -Raw -LiteralPath $localManifest | ConvertFrom-Json
        if ($manifest.release_tag -eq 'latest' -or $manifest.release_tag -eq $Release.tag_name) {
            return Find-BundledDriver $PSScriptRoot
        }
    }
    $bundles = @($Release.assets | Where-Object { $_.name -ceq 'xr-gate-windows.zip' })
    if ($bundles.Count) {
        $asset = Select-ReleaseAsset $Release 'xr-gate-windows.zip'
        $archive = Save-ReleaseAsset $asset $Cache
        $directory = Join-Path $Cache ('windows-' + ([string]$asset.digest).Substring(7,12))
        if (!(Test-Path -LiteralPath (Join-Path $directory 'xr-gate-windows/bundle.json'))) {
            Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
        }
        return Find-BundledDriver (Join-Path $directory 'xr-gate-windows')
    }
    # Older releases published each variant separately.
    $name = 'xr-gate-steamvr-windows-x64-' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '.zip'
    $asset = Select-ReleaseAsset $Release $name
    $archive = Save-ReleaseAsset $asset $Cache
    $directory = Join-Path $Cache ('driver-' + ([string]$asset.digest).Substring(7,12))
    if (!(Test-Path -LiteralPath (Join-Path $directory 'xr_tracking/driver.vrdrivermanifest'))) {
        Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
    }
    return Join-Path $directory 'xr_tracking'
}
function Get-UsbDevices {
    $lines = @(& (Get-Tool 'usbipd.exe') state)
    if ($LASTEXITCODE -ne 0) { throw 'usbipd state failed; install a current usbipd-win.' }
    $data = ($lines -join [Environment]::NewLine) | ConvertFrom-Json
    return @($data.Devices | Where-Object { $_.BusId })
}
function Select-RuntimeDevices([object[]]$Devices, [string[]]$ExplicitBusIds = @(), [string[]]$SavedInstanceIds = @()) {
    $selected = @()
    $instances = @(Get-XrList 'XR_USB_INSTANCE_IDS')
    if ($ExplicitBusIds.Count -or $instances.Count) {
        if ($ExplicitBusIds.Count) {
            foreach ($id in $ExplicitBusIds) {
                if ($id -notmatch '^\d+-\d+$') { throw "Invalid USB BUSID: $id" }
                $matches = @($Devices | Where-Object { $_.BusId -eq $id })
                if ($matches.Count -ne 1) { throw "USB BUSID $id was not found. Check usbipd list and XR_USB_BUS_IDS." }
                $selected += $matches[0]
            }
        } else {
            foreach ($id in $instances) {
                $matches = @($Devices | Where-Object { $_.InstanceId -eq $id })
                if ($matches.Count -ne 1) { throw "USB instance was not found: $id" }
                $selected += $matches[0]
            }
        }
    } else {
        foreach ($identity in @(Get-XrList 'XR_USB_IDS')) {
            if ($identity -notmatch '^([0-9a-fA-F]{4}):([0-9a-fA-F]{4})$') { throw 'XR_USB_IDS must contain VID:PID pairs.' }
            $pattern = '^USB\\VID_' + $Matches[1] + '&PID_' + $Matches[2] + '\\'
            $matches = @($Devices | Where-Object { $_.InstanceId -match $pattern })
            $saved = @($matches | Where-Object { $_.InstanceId -in $SavedInstanceIds })
            if ($saved.Count) { $matches = $saved }
            if (!$matches.Count) { throw "USB device $identity was not found. Check the hardware env and usbipd list." }
            if ($matches.Count -gt 1 -and !$saved.Count) {
                $matches | Format-Table BusId,Description,InstanceId | Out-Host
                throw 'Multiple matching USB devices. Set XR_USB_BUS_IDS in .env.'
            }
            $selected += $matches
        }
    }
    if (!$selected.Count -and (Get-XrSetting 'XR_REQUIRE_CAMERA') -eq '1') {
        throw 'Set XR_USB_IDS, XR_USB_BUS_IDS or XR_USB_INSTANCE_IDS in the hardware env.'
    }
    return @($selected | Sort-Object InstanceId -Unique)
}
function Get-SelectedUsb([object[]]$Devices) {
    $selected = @(Select-RuntimeDevices -Devices $Devices -ExplicitBusIds $UsbBusId -SavedInstanceIds @($script:State.UsbInstanceIds))
    $script:State.UsbInstanceIds = @($selected | ForEach-Object { [string]$_.InstanceId })
    if ($BluetoothBusId -and !$WithoutBluetooth) {
        $bt = @($Devices | Where-Object { $_.BusId -eq $BluetoothBusId })
        if ($bt.Count -ne 1 -or $bt[0].InstanceId -in $script:State.UsbInstanceIds) { throw 'Select a separate Bluetooth dongle BUSID from usbipd list.' }
        $script:State.BluetoothInstanceId = [string]$bt[0].InstanceId
    }
    if ($script:State.BluetoothInstanceId) {
        $bt = @($Devices | Where-Object { $_.InstanceId -eq $script:State.BluetoothInstanceId })
        if ($bt.Count -ne 1) { throw 'Saved Bluetooth dongle not found. Set XR_BLUETOOTH_BUS_ID or use -WithoutBluetooth.' }
        $selected += $bt[0]
    }
    if ((Get-XrSetting 'XR_BLUETOOTH') -eq '1' -and !$script:State.BluetoothInstanceId -and !$WithoutBluetooth) {
        throw 'Set XR_BLUETOOTH_BUS_ID or XR_BLUETOOTH_INSTANCE_ID for the Bluetooth dongle.'
    }
    Save-State; return $selected
}
function Connect-SelectedUsb {
    Start-Process -FilePath (Get-Tool 'wsl.exe') -WindowStyle Hidden -ArgumentList @('--distribution',[string]$script:State.Distribution,'--exec','sleep','90') | Out-Null
    $selected = @(Get-SelectedUsb @(Get-UsbDevices))
    $unbound = @($selected | Where-Object { !$_.PersistedGuid } | ForEach-Object { [string]$_.BusId })
    if ($unbound.Count) { $null = Invoke-Elevated -Task Bind -BusIds $unbound }
    foreach ($d in $selected) {
        if (!$d.ClientIPAddress) {
            Write-Step ("Attaching " + $d.Description + ' [' + $d.BusId + '] to WSL')
            Invoke-Native (Get-Tool 'usbipd.exe') @('attach','--wsl','--busid',[string]$d.BusId)
        } else { Write-Step ("Already attached: " + $d.BusId) }
    }
    Invoke-Linux -Operation prepare -Request (New-LinuxRequest) -AsRoot
}
function Get-SteamVrDirectory {
    if ($SteamVR) {
        $p = (Resolve-Path -LiteralPath $SteamVR).Path
        if (!(Test-Path -LiteralPath (Join-Path $p 'bin\win64\vrpathreg.exe'))) { throw 'The -SteamVR directory has no vrpathreg.exe.' }
        return $p
    }
    $paths = New-Object 'System.Collections.Generic.List[string]'
    $vpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path -LiteralPath $vpath) {
        $vr = Get-Content -Raw -LiteralPath $vpath | ConvertFrom-Json
        foreach ($p in @($vr.runtime)) { if ($p) { $paths.Add([string]$p) } }
    }
    $steamRoot = ''
    try { $steamRoot = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
    if (!$steamRoot) { $steamRoot = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Steam' }
    if ($steamRoot) {
        $paths.Add((Join-Path $steamRoot 'steamapps\common\SteamVR'))
        $libraries = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $libraries) {
            foreach ($m in [regex]::Matches((Get-Content -Raw -LiteralPath $libraries), '"path"\s+"([^"]+)"')) {
                $paths.Add((Join-Path ($m.Groups[1].Value.Replace('\\','\')) 'steamapps\common\SteamVR'))
            }
        }
    }
    foreach ($p in $paths) {
        if (Test-Path -LiteralPath (Join-Path $p 'bin\win64\vrpathreg.exe')) { return $p }
    }
    return ''
}
function Register-XrDriver([switch]$AllowDeferred) {
    if (!$script:State.DriverPath -or !(Test-Path -LiteralPath $script:State.DriverPath)) { throw 'Download the Windows driver with -Action Install first.' }
    $svr = Get-SteamVrDirectory
    if (!$svr -or (Get-Process vrserver,vrcompositor -ErrorAction SilentlyContinue)) {
        $message = 'Install SteamVR, run it once, close it, then run -Action Driver. Or pass -SteamVR D:\SteamLibrary\steamapps\common\SteamVR.'
        if ($AllowDeferred) { Write-Warning $message; return }; throw $message
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $script:State.DriverPath 'driver.vrdrivermanifest') | ConvertFrom-Json
    $reg = Join-Path $svr 'bin\win64\vrpathreg.exe'
    $vpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (Test-Path -LiteralPath $vpath) {
        $vr = Get-Content -Raw -LiteralPath $vpath | ConvertFrom-Json
        Copy-Item -LiteralPath $vpath -Destination (Join-Path $script:StateDir ('openvrpaths.before-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.json'))
        foreach ($old in @($vr.external_drivers)) {
            if (!$old -or $old -eq $script:State.DriverPath) { continue }
            $m = Join-Path $old 'driver.vrdrivermanifest'
            if (Test-Path -LiteralPath $m) {
                if ((Get-Content -Raw -LiteralPath $m | ConvertFrom-Json).name -eq $manifest.name) {
                    Write-Step ("Unregistering the previous XR Gate package: " + $old)
                    Invoke-Native $reg @('removedriver',[string]$old)
                }
            }
        }
    }
    Invoke-Native $reg @('adddriver',[string]$script:State.DriverPath)
    Invoke-Native $reg @('show')
    $script:State.RegisteredDriver = $script:State.DriverPath; Save-State
}
function Wait-XrStream([int]$Seconds) {
    $clock = [Diagnostics.Stopwatch]::StartNew(); $lastError = 'No connection.'; $sawFrames = $false
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $client = New-Object Net.Sockets.TcpClient; $reader = $null
        try {
            $pending = $client.BeginConnect($script:TcpHost,$script:TcpPort,$null,$null)
            if (!$pending.AsyncWaitHandle.WaitOne(1500)) { throw 'Connection timeout.' }
            $client.EndConnect($pending); $client.GetStream().ReadTimeout = 2000
            $reader = New-Object IO.StreamReader($client.GetStream())
            $helloLine = $reader.ReadLine()
            if (!$helloLine) { throw 'No XR Gate hello.' }
            $hello = $helloLine | ConvertFrom-Json
            if ($hello.protocol -ne 'xr_gate_app_v1') { throw 'The configured TCP endpoint is serving a different protocol.' }
            $lastSequence = ''; $lastPoseSource = ''; $valid = 0
            while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
                $line = $reader.ReadLine(); if (!$line) { throw 'TCP stream closed.' }
                $frame = $line | ConvertFrom-Json
                if ($frame.type -ne 'frame') { continue }
                $sawFrames = $true; $sequence = [string]$frame.sequence
                if ($sequence -eq $lastSequence) { $valid = 0; continue }
                $lastSequence = $sequence
                if ($frame.hmd -and $frame.hmd.pose_valid -eq $true) {
                    $source = [string]$frame.hmd.source_timestamp_ns
                    if ($source -and $source -ne '0' -and $source -ne $lastPoseSource) {
                        $valid++; $lastPoseSource = $source
                    }
                } else { $valid = 0; $lastPoseSource = '' }
                if ($valid -ge 3) {
                    Write-Host 'OK: xr_gate_app_v1, updating sequence, HMD pose_valid=true.' -ForegroundColor Green; return
                }
            }
        } catch { $lastError = $_.Exception.Message }
        finally { if ($reader) { $reader.Dispose() }; $client.Close() }
        Start-Sleep -Milliseconds 400
    }
    if ($sawFrames) { throw 'TCP frames arrived, but fresh valid HMD poses were not observed. Check the xr_client console/startup gate.' }
    throw "XR Gate TCP is not ready: $lastError Check the xr_client console and WSL localhost forwarding."
}
function Start-RuntimeWindow {
    $body = '& ' + (Quote-PsLiteral $script:Self) + ' -Action Runtime'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    Start-Process 'powershell.exe' -ArgumentList @('-NoLogo','-NoProfile','-NoExit','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) | Out-Null
}
function Show-Help {
    Write-Host @'
XR Gate / WSL2 / no Docker
  -Action Install                 Install Ubuntu runtime and Windows driver.
  -Action Run [-StartSteamVR]      Attach USB, open xr_client; optionally start SteamVR.
  -Action Usb                     Attach devices selected in the hardware env.
  -Action Usb -BluetoothBusId 1-4  Also attach your dedicated USB Bluetooth dongle.
  -Action Detach                  Return the selected USB devices to Windows.
  -Action Doctor                  Show Windows/WSL/USB/runtime diagnostics.
  -Action Test                    Wait for TCP and valid updating HMD frames.
  -Action Driver                  Register the downloaded SteamVR driver.
  -Action Display                 Select/save the glasses' extended SBS display.
  -Action Pair                    Open bluetoothctl in WSL as root.
  -Action Controllers             Register Gear VR / train another layout.
Options:
  -ReleaseTag vX.Y.Z               Pin a release (Install defaults to latest).
  -GearVR                         Configure the Gear VR provider/template.
  -ControllerConfig C:\gearvr.json Import existing controller JSON with backup.
  -WithoutBluetooth               Forget the saved Bluetooth dongle.
  -FrequencyHz 90                 Select a 90 Hz Windows driver (default 60).
  -DisplayMode extended_sbs        Desktop SBS output (default for new installs).
  -DisplayMode direct              Select an optional Direct Mode driver.
  -DisplayName \\.\DISPLAY2       Select this output with -Action Display.
  -DisplayOption current          Keep the SBS mode set in Windows (default).
  -SteamVR D:\SteamLibrary\steamapps\common\SteamVR
Read START-HERE.html for first-login, physical display and controller steps.
'@
}
if ($Action -eq 'Help') { Show-Help; return }
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Run this script in Windows PowerShell, not inside Ubuntu.' }
    if (![Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { throw 'Windows x64 is required.' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    if ($Action -eq 'Admin') { $result = Invoke-AdminTask; exit ([int]$result) }
    Load-State
    if ($Action -eq 'Install') {
        Save-State
        $rc = Invoke-Elevated -Task Prerequisites
        if ($rc -eq 20) { Write-Host 'Save work, restart Windows, and run Install again.' -ForegroundColor Yellow; exit 20 }
        if (!(Ensure-Distribution)) { exit 20 }
        if ($BluetoothBusId -and !$WithoutBluetooth) {
            $bt = @(Get-UsbDevices | Where-Object { $_.BusId -eq $BluetoothBusId })
            if ($bt.Count -ne 1) { throw 'The specified USB Bluetooth BUSID was not found.' }
            $script:State.BluetoothInstanceId = [string]$bt[0].InstanceId
        }
        Save-State
        Write-Step 'Preparing Python in the selected Ubuntu.'
        $wsl = Get-Tool 'wsl.exe'
        Invoke-Native $wsl @('-d',[string]$script:State.Distribution,'-u','root','--exec','env','DEBIAN_FRONTEND=noninteractive','apt-get','update')
        Invoke-Native $wsl @('-d',[string]$script:State.Distribution,'-u','root','--exec','env','DEBIAN_FRONTEND=noninteractive','apt-get','install','-y','--no-install-recommends','python3')
        $release = Get-ReleaseInfo $ReleaseTag; Write-Step ("Selected release: " + $release.tag_name)
        $runtimeAsset = Select-ReleaseAsset $release 'xr-gate-ubuntu-24.04-linux-x64.tar.gz'
        $modelsAsset = Select-ReleaseAsset $release 'hand-tracking-models-mercury.tar.gz'
        $cache = Join-Path $script:StateDir ('cache\' + $release.tag_name)
        $runtimeArchive = Save-ReleaseAsset $runtimeAsset $cache
        $modelsArchive = Save-ReleaseAsset $modelsAsset $cache
        $driverPackage = Get-DriverPackage $release $cache
        $request = New-LinuxRequest
        $request.tag = [string]$release.tag_name
        $request.runtime_archive = ConvertTo-WslPath $runtimeArchive
        $request.models_archive = ConvertTo-WslPath $modelsArchive
        $request.runtime_sha256 = ([string]$runtimeAsset.digest).Substring(7)
        $request.models_sha256 = ([string]$modelsAsset.digest).Substring(7)
        if ($ControllerConfig) { $request.controller_import = ConvertTo-WslPath $ControllerConfig }
        Invoke-Linux -Operation install -Request $request -AsRoot
        if ($script:LinuxExitCode -eq 20) {
            Write-Host ("Save work in this Ubuntu, run: wsl --terminate " + $script:State.Distribution) -ForegroundColor Yellow
            Write-Host 'Then run Install again. Other distributions do not need to be stopped.'; exit 20
        }
        $driverHash = (Get-FileHash -LiteralPath (Join-Path $driverPackage 'bin/win64/driver_xr_tracking.dll') -Algorithm SHA256).Hash.Substring(0,12)
        $settingsHash = (Get-FileHash -LiteralPath (Join-Path $driverPackage 'resources/settings/default.vrsettings') -Algorithm SHA256).Hash.Substring(0,12)
        $driverDir = Join-Path $script:StateDir ('drivers\' + $release.tag_name + '-' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '-' + $driverHash + $settingsHash)
        if (!(Test-Path -LiteralPath (Join-Path $driverDir 'xr_tracking\driver.vrdrivermanifest'))) {
            New-Item -ItemType Directory -Force $driverDir | Out-Null
            Copy-Item -LiteralPath $driverPackage -Destination $driverDir -Recurse
        }
        $script:State.ReleaseTag = [string]$release.tag_name
        $script:State.DriverPath = Join-Path $driverDir 'xr_tracking'
        Save-State; Register-XrDriver -AllowDeferred
        Write-Host 'Installation complete. Next: enable SBS, select the matching Hz in Windows, then DISPLAY.cmd and RUN.cmd. See START-HERE.html.' -ForegroundColor Green
        exit 0
    }
    if (!$script:State.LinuxUser) { throw 'Run -Action Install first.' }
    switch ($Action) {
        'Run' {
            if ($StartSteamVR -and !(Get-Process vrserver,vrcompositor -ErrorAction SilentlyContinue)) {
                if ($script:State.DisplayMode -eq 'extended_sbs') { Update-SavedSbsDisplay }
                else { Set-XrDisplaySettings -X 0 -Y 0 -Mode direct }
            }
            $status = (Invoke-Linux -Operation status -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
            if (!$status.installed) { throw 'Linux runtime is not installed. Run -Action Install.' }
            if (!$status.running) {
                Connect-SelectedUsb; Save-State; Start-RuntimeWindow
                Write-Step 'xr_client opened in a separate console. Complete its startup prompts there.'
            } else { Write-Step 'The managed xr_client is already running.' }
            if ($StartSteamVR) {
                if ($script:State.RegisteredDriver -ne $script:State.DriverPath) { Register-XrDriver }
                Write-Step 'Waiting for valid updating HMD poses before starting SteamVR...'
                Wait-XrStream $TimeoutSeconds; Start-Process 'steam://rungameid/250820'
            }
        }
        'Runtime' { Invoke-Linux -Operation run -Request (New-LinuxRequest) }
        'Usb' { Connect-SelectedUsb }
        'Detach' {
            $status = (Invoke-Linux -Operation status -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
            if ($status.running) { throw 'Stop xr_client with Ctrl+C before detaching USB.' }
            foreach ($d in @(Get-UsbDevices)) {
                if ($d.InstanceId -in (@($script:State.UsbInstanceIds) + @($script:State.BluetoothInstanceId)) -and $d.ClientIPAddress) {
                    Invoke-Native (Get-Tool 'usbipd.exe') @('detach','--busid',[string]$d.BusId)
                }
            }
            Write-Step 'Selected USB devices returned to Windows.'
        }
        'Doctor' {
            Invoke-Native (Get-Tool 'wsl.exe') @('--version')
            Invoke-Native (Get-Tool 'wsl.exe') @('--list','--verbose')
            Invoke-Native (Get-Tool 'usbipd.exe') @('list')
            Invoke-Linux -Operation doctor -Request (New-LinuxRequest)
            Write-Host ("Windows driver: " + $script:State.DriverPath)
            Write-Host ("Registered by this script: " + $script:State.RegisteredDriver)
            Write-Host ("Display profile: " + $script:State.FrequencyHz + ' Hz / ' + $script:State.DisplayMode)
            Write-Host ("Selected desktop output: " + $script:State.DisplayDeviceName)
        }
        'Test' { Wait-XrStream $TimeoutSeconds }
        'Driver' { Register-XrDriver }
        'Display' { Choose-SbsDisplay }
        'Pair' { Connect-SelectedUsb; Invoke-Linux -Operation pair -Request (New-LinuxRequest) -AsRoot }
        'Controllers' { Connect-SelectedUsb; Invoke-Linux -Operation controllers -Request (New-LinuxRequest) }
    }
    exit 0
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'See START-HERE.html. For device/runtime details use -Action Doctor.'; exit 1
}

