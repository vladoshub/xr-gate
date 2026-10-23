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
    [ValidateSet('Install','Run','Runtime','Stop','Delete','Status','DeviceStatus','BluetoothScan','Usb','Detach','Doctor','Test','Driver','Display','Pair','Controllers','Help','Admin')]
    [string]$Action = 'Help',
    [string]$Distribution = '',
    [string]$ReleaseTag = '',
    [Alias('XrealBusId')][string[]]$UsbBusId = @(),
    [string]$BluetoothBusId = '',
    [string]$BluetoothUsbId = '',
    [switch]$WithoutBluetooth,
    [switch]$GearVR,
    [string]$ControllerConfig = '',
    [string]$LeftControllerMac = '',
    [string]$RightControllerMac = '',
    [ValidateRange(0,1000)][int]$FrequencyHz = 0,
    [ValidateSet('','direct','extended_sbs')][string]$DisplayMode = '',
    [ValidateSet('','current','60hz','90hz','prompt')][string]$DisplayOption = '',
    [string]$DisplayName = '',
    [string]$SteamVR = '',
    [switch]$StartSteamVR,
    [switch]$Launcher,
    [string]$DeleteDistributionName = '',
    [string]$RuntimeLaunchToken = '',
    [string]$ResultPath = '',
    [ValidateRange(0,600)][int]$TimeoutSeconds = 0,
    [ValidateSet('Prerequisites','Bind')][string]$AdminTask = 'Prerequisites',
    [string[]]$BindBusId = @()
)
if ($Launcher) {
    $OutputEncoding = [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    $env:PYTHONIOENCODING = 'utf-8'
    $ProgressPreference = 'SilentlyContinue'
}
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Repo = 'vladoshub/xr-gate'
$script:Self = $PSCommandPath
. (Join-Path $PSScriptRoot 'windows\Environment.ps1')
if ($Action -in @('Stop','Delete')) { Initialize-XrStopSettings $PSScriptRoot }
else { Initialize-XrSettings $PSScriptRoot }
if (!$ReleaseTag) { $ReleaseTag = Get-XrSetting 'XR_RELEASE_TAG' 'latest' }
if (!$TimeoutSeconds) { $TimeoutSeconds = Get-XrInteger 'XR_START_TIMEOUT' 5 600 }
if (!$SteamVR) { $SteamVR = Get-XrSetting 'XR_STEAMVR_PATH' }
if (!$ControllerConfig) { $ControllerConfig = Get-XrSetting 'XR_CONTROLLER_CONFIG' }
if (!$DisplayName) { $DisplayName = Get-XrSetting 'XR_DISPLAY_NAME' }
if (!$UsbBusId.Count) { $UsbBusId = @(Get-XrList 'XR_USB_BUS_IDS') }
if (!$BluetoothBusId) { $BluetoothBusId = Get-XrSetting 'XR_BLUETOOTH_BUS_ID' }
$script:BluetoothUsbIdExplicit = $PSBoundParameters.ContainsKey('BluetoothUsbId') -and [bool]$BluetoothUsbId
if (!$PSBoundParameters.ContainsKey('BluetoothUsbId')) { $BluetoothUsbId = Get-XrSetting 'XR_BLUETOOTH_USB_ID' }
$script:TcpHost = Get-XrSetting 'XR_TCP_HOST' '127.0.0.1'
$script:TcpPort = Get-XrInteger 'XR_TCP_PORT'

$script:StateDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'XR-Gate-WSL'
$script:StateFile = Join-Path $script:StateDir 'state.json'
$script:State = [ordered]@{
    Distribution = (Get-XrSetting 'XR_DISTRIBUTION'); LinuxUser = ''; ReleaseTag = ''
    UsbInstanceIds = @(); BluetoothInstanceId = ''; AttachedUsbInstanceIds = @()
    CleanupSettings = $null
    InstallPending = $false; InstallNextStep = ''
    FrequencyHz = (Get-XrInteger 'XR_DISPLAY_FREQUENCY_HZ' 1 1000)
    DisplayMode = (Get-XrSetting 'XR_DISPLAY_MODE'); DisplayOption = (Get-XrSetting 'XR_DISPLAY_OPTION')
    GearVR = $false; DriverPath = ''; RegisteredDriver = ''
    DisplayDeviceId = ''; DisplayDeviceName = ''
}
$script:LinuxExitCode = 0
$script:LinuxHelperPath = ''
$script:NextStep = ''
$script:ActionWarnings = New-Object 'System.Collections.Generic.List[string]'
$script:ActionData = $null
. (Join-Path $PSScriptRoot 'windows\Display.ps1')
. (Join-Path $PSScriptRoot 'windows\DeviceStatus.ps1')
. (Join-Path $PSScriptRoot 'windows\Driver.ps1')
. (Join-Path $PSScriptRoot 'windows\Usb.ps1')
. (Join-Path $PSScriptRoot 'windows\RuntimeWindow.ps1')
. (Join-Path $PSScriptRoot 'windows\Delete.ps1')
. (Join-Path $PSScriptRoot 'windows\SetupStatus.ps1')

function Write-Step([string]$Message) { Write-Host ("[XR Gate] " + $Message) -ForegroundColor Cyan }
function Write-XrWarning([string]$Message) {
    $script:ActionWarnings.Add($Message)
    Write-Warning $Message
}
function Complete-XrAction([int]$Code, [string]$Message = '') {
    if ($Action -eq 'Install') {
        $script:State.InstallPending = $Code -ne 0
        $script:State.InstallNextStep = $script:NextStep
        Save-State
    }
    if ($ResultPath) {
        if ($null -eq $script:ActionData) { $script:ActionData = @{distribution=[string]$script:State.Distribution} }
        $payload = @{exit_code=$Code; next_step=$script:NextStep; message=$Message;
                     warnings=@($script:ActionWarnings.ToArray()); data=$script:ActionData}
        $temporary = $ResultPath + '.new'
        [IO.File]::WriteAllText($temporary, ($payload | ConvertTo-Json -Depth 30), (New-Object Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $temporary -Destination $ResultPath -Force
    }
    exit $Code
}
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
        $saved = Get-Content -Encoding UTF8 -Raw -LiteralPath $script:StateFile | ConvertFrom-Json
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
        if ($key -eq 'XR_DISPLAY_FREQUENCY_HZ' -and (Get-XrSetting 'XR_AUTO_DRIVER_FREQUENCY') -eq '1') { continue }
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
    $controllerMode = Get-XrSetting 'XR_CONTROLLER_MODE'
    if ($controllerMode) { $script:State.GearVR = ($controllerMode -eq 'gearvr') }
    if ((Get-XrSetting 'XR_BLUETOOTH') -eq '0') { $script:WithoutBluetooth = $true }
    $bluetoothInstance = Get-XrSetting 'XR_BLUETOOTH_INSTANCE_ID'
    if ($bluetoothInstance -and !$BluetoothBusId) { $script:State.BluetoothInstanceId = $bluetoothInstance }
    if ($GearVR) { $script:State.GearVR = $true }
    if ($GearVR) { $script:Settings['XR_CONTROLLER_MODE'] = 'gearvr' }
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
    if ([IO.Path]::GetFileNameWithoutExtension($File) -eq 'usbipd') {
        Write-Step ('usbipd ' + ($Arguments -join ' '))
        $result = Invoke-UsbipdCommand -File $File -Arguments $Arguments -StreamOutput
        $script:NativeExitCode = $result.ExitCode
        if ($AllowedCodes -notcontains $result.ExitCode) { throw "$File failed (exit $($result.ExitCode))." }
        return
    }
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
    if ($Operation -eq 'run' -and $Launcher -and (Get-XrSetting 'XR_SHOW_COMMAND_PROMPT' '0') -ne '1') {
        # The long-lived Runtime process owns both the WSL client and log files.
        # Redirecting from the transient Run process would lose its pipe readers
        # when Start VR returns. Visible consoles keep their interactive stdin.
        $files = Get-RuntimeWindowFiles (Read-RuntimeWindow)
        $script:LinuxExitCode = Invoke-LoggedRuntime -Arguments $a -Files $files
        if ($script:LinuxExitCode -notin @(0,20)) { throw "Linux helper '$Operation' failed (exit $script:LinuxExitCode). See the runtime log above." }
        return
    }
    if ($Capture) { $output = @(& (Get-Tool 'wsl.exe') @a) } else { & (Get-Tool 'wsl.exe') @a }
    $script:LinuxExitCode = $LASTEXITCODE
    if ($LASTEXITCODE -notin @(0,20)) { throw "Linux helper '$Operation' failed (exit $LASTEXITCODE). See the message above." }
    if ($Capture) { return (($output -join [Environment]::NewLine) -replace '\x00', '').Trim() }
}
function New-LinuxRequest {
    $bluetoothId = ''
    $identity = [regex]::Match([string]$script:State.BluetoothInstanceId, '(?i)^USB\\VID_([0-9a-f]{4})&PID_([0-9a-f]{4})(?:&[^\\]+)?\\')
    if ($identity.Success) { $bluetoothId = $identity.Groups[1].Value.ToLowerInvariant() + ':' + $identity.Groups[2].Value.ToLowerInvariant() }
    $request = @{
        user = [string]$script:State.LinuxUser; gearvr = [bool]$script:State.GearVR
        bluetooth = [bool]$script:State.BluetoothInstanceId
        bluetooth_usb_id = $bluetoothId
        display_option = [string]$script:State.DisplayOption
        settings = $script:Settings
    }
    if ($RuntimeLaunchToken) {
        if ($RuntimeLaunchToken -notmatch '^[a-f0-9]{32}$') { throw 'Invalid runtime launch token.' }
        Wait-RuntimeWindowRegistration $RuntimeLaunchToken
        $request.runtime_guard = @{path=(ConvertTo-WslPath (Get-RuntimeWindowPath)); token=$RuntimeLaunchToken}
    }
    return $request
}
function Invoke-Elevated([string]$Task, [string[]]$BusIds = @()) {
    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
    $resultFile = Join-Path $script:StateDir ('admin-result-' + [Guid]::NewGuid().ToString('N') + '.json')
    $body = '& ' + (Quote-PsLiteral $script:Self) + ' -Action Admin -AdminTask ' + (Quote-PsLiteral $Task)
    $body += ' -Launcher -ResultPath ' + (Quote-PsLiteral $resultFile)
    if ($BusIds.Count) {
        foreach ($id in $BusIds) { if ($id -notmatch '^\d+-\d+$') { throw 'Invalid USB BUSID.' } }
        $literals = @($BusIds | ForEach-Object { Quote-PsLiteral $_ })
        $body += ' -BindBusId @(' + ($literals -join ',') + ')'
    }
    $body += '; exit $LASTEXITCODE'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    Write-Step 'Windows requests administrator rights for this setup step.'
    try {
        $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded)
        if (!(Test-Path -LiteralPath $resultFile -PathType Leaf)) {
            throw "Administrator step returned no details (exit $($p.ExitCode)). Check the setup console."
        }
        $result = Get-Content -Encoding UTF8 -Raw -LiteralPath $resultFile | ConvertFrom-Json
        if (!$result.PSObject.Properties['exit_code'] -or [int]$result.exit_code -ne $p.ExitCode) {
            throw 'Administrator result does not match its process exit code.'
        }
        foreach ($warning in @($result.warnings)) { Write-XrWarning ([string]$warning) }
        if ($p.ExitCode -notin @(0,20)) { throw ("Administrator step failed (exit $($p.ExitCode)). " + $result.message) }
        return $p.ExitCode
    } finally {
        foreach ($file in @($resultFile, ($resultFile + '.new'))) {
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        }
    }
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
        if ($script:NativeExitCode -eq 3010) {
            $script:NextStep = 'restart_windows'
            Write-Host 'Save work, restart Windows, and run Install again.' -ForegroundColor Yellow
            return $false
        }
        $script:NextStep = 'ubuntu_first_login'
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
        $script:NextStep = 'ubuntu_first_login'
        Start-Process $wsl -ArgumentList @('--distribution', [string]$script:State.Distribution)
        Write-Host 'Complete the first Ubuntu login with a regular Linux user, type exit, then run Install again.'
        return $false
    }
    $script:State.LinuxUser = $name
    return $true
}
function Get-ReleaseInfo([string]$Tag) {
    if ($Tag -ne 'latest' -and $Tag -notmatch '^[A-Za-z0-9._-]+$') { throw 'Invalid release tag.' }
    $suffix = if ($Tag -eq 'latest') { 'latest' } else { 'tags/' + $Tag }
    $r = Invoke-RestMethod -Uri "https://api.github.com/repos/$script:Repo/releases/$suffix" -Headers @{'User-Agent'='XR-Gate-WSL-Setup'} -TimeoutSec 45
    if ($Tag -eq 'latest') {
        $names = @($r.assets | ForEach-Object { $_.name })
        if ('xr-gate-ubuntu-24.04-linux-x64.tar.gz' -notin $names -or 'hand-tracking-models-mercury.tar.gz' -notin $names) {
            # A newer Windows-only release can become GitHub's latest. Explicit
            # tags keep their meaning; only "latest" follows a usable runtime.
            $found = $null
            for ($page = 1; $page -le 10 -and !$found; $page++) {
                # Invoke-RestMethod emits a JSON array as one pipeline object.
                # Assign it first: @(Invoke-RestMethod ...) nests the array and
                # turns the draft/prerelease tests into tests of whole lists.
                $response = Invoke-RestMethod -Uri "https://api.github.com/repos/$script:Repo/releases?per_page=100&page=$page" -Headers @{'User-Agent'='XR-Gate-WSL-Setup'} -TimeoutSec 45
                $releases = @($response)
                if (!$releases.Count) { break }
                foreach ($release in $releases) {
                    $names = @($release.assets | ForEach-Object { $_.name })
                    if (!$release.draft -and !$release.prerelease -and 'xr-gate-ubuntu-24.04-linux-x64.tar.gz' -in $names -and 'hand-tracking-models-mercury.tar.gz' -in $names) { $found = $release; break }
                }
            }
            if (!$found) { throw 'No published Ubuntu runtime/models release was found. Select its explicit release tag in Settings.' }
            $r = $found
        }
    }
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
    $manifest = Get-Content -Encoding UTF8 -Raw -LiteralPath $manifestPath | ConvertFrom-Json
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
    if ($script:ResolvedDriverRoot) { return Find-BundledDriver $script:ResolvedDriverRoot }
    $localManifest = Join-Path $PSScriptRoot 'bundle.json'
    if (Test-Path -LiteralPath $localManifest -PathType Leaf) {
        $manifest = Get-Content -Encoding UTF8 -Raw -LiteralPath $localManifest | ConvertFrom-Json
        if ($manifest.release_tag -eq 'latest' -or $manifest.release_tag -eq $Release.tag_name) {
            return Find-BundledDriver $PSScriptRoot
        }
    }
    $bundles = @($Release.assets | Where-Object { $_.name -ceq 'xr-gate-windows-wsl-usbip.zip' })
    if (!$bundles.Count) { $bundles = @($Release.assets | Where-Object { $_.name -ceq 'xr-gate-windows.zip' }) }
    if ($bundles.Count) {
        $asset = Select-ReleaseAsset $Release ([string]$bundles[0].name)
        $archive = Save-ReleaseAsset $asset $Cache
        $directory = Join-Path $Cache ('windows-' + ([string]$asset.digest).Substring(7,12))
        $bundleRoot = if ($asset.name -ceq 'xr-gate-windows.zip') { 'xr-gate-windows' } else { 'xr-gate-windows-wsl-usbip' }
        if (!(Test-Path -LiteralPath (Join-Path $directory ($bundleRoot + '/bundle.json')))) {
            Expand-Archive -LiteralPath $archive -DestinationPath $directory -Force
        }
        return Find-BundledDriver (Join-Path $directory $bundleRoot)
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
    $result = Invoke-UsbipdCommand -File (Get-Tool 'usbipd.exe') -Arguments @('state')
    if ($result.ExitCode -ne 0) { throw 'usbipd state failed; install a current usbipd-win.' }
    $data = $result.Output | ConvertFrom-Json
    return @($data.Devices | Where-Object { $_.BusId })
}
function Get-CurrentUsbDevice([string]$InstanceId) {
    $current = @(Get-UsbDevices | Where-Object { $_.InstanceId -eq $InstanceId })
    if ($current.Count -ne 1) { throw 'A selected USB device disconnected. Reconnect it and use List USB devices before retrying.' }
    return $current[0]
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
function Select-BluetoothDevice([object[]]$Devices, [string]$ExplicitBusId = '', [string]$ExplicitInstanceId = '',
                                [string]$UsbId = '', [string]$SavedInstanceId = '') {
    if ($ExplicitBusId) {
        if ($ExplicitBusId -notmatch '^\d+-\d+$') { throw 'Bluetooth USB BUSID must look like 2-7. Use List USB devices.' }
        $candidates = @($Devices | Where-Object { $_.BusId -eq $ExplicitBusId })
        if ($candidates.Count -ne 1) { throw "Bluetooth BUSID $ExplicitBusId was not found. Check List USB devices; automatic VID:PID selection is only used when BUSID is empty." }
    } elseif ($ExplicitInstanceId) {
        $candidates = @($Devices | Where-Object { $_.InstanceId -eq $ExplicitInstanceId })
        if ($candidates.Count -ne 1) { throw 'Configured Bluetooth USB instance was not found. Check XR_BLUETOOTH_INSTANCE_ID.' }
    } elseif ($UsbId) {
        if ($UsbId -notmatch '^([0-9a-fA-F]{4}):([0-9a-fA-F]{4})$') { throw 'XR_BLUETOOTH_USB_ID must be one VID:PID pair, for example 1234:abcd.' }
        $pattern = '^USB\\VID_' + $Matches[1] + '&PID_' + $Matches[2] + '(?:&[^\\]+)?\\'
        $candidates = @($Devices | Where-Object { $_.InstanceId -match $pattern })
        if (!$candidates.Count) { throw "Bluetooth adapter $UsbId was not found. Connect the configured adapter, change Bluetooth VID:PID / BUSID in Controllers, or disable Bluetooth. The system adapter is not selected automatically." }
        if ($candidates.Count -gt 1) {
            $saved = @($candidates | Where-Object { $_.InstanceId -eq $SavedInstanceId })
            if ($saved.Count -eq 1) { $candidates = $saved }
            else { throw "Multiple Bluetooth adapters match $UsbId. Select one using Bluetooth USB BUSID from List USB devices." }
        }
    } elseif ($SavedInstanceId) {
        $candidates = @($Devices | Where-Object { $_.InstanceId -eq $SavedInstanceId })
        if ($candidates.Count -ne 1) { throw 'Saved Bluetooth dongle not found. Set Bluetooth VID:PID / BUSID in Controllers or disable Bluetooth.' }
    } else { return $null }
    return $candidates[0]
}
function Test-BluetoothSelectionRequested {
    if ($WithoutBluetooth -or (Get-XrSetting 'XR_BLUETOOTH') -eq '0') { return $false }
    return [bool]((Get-XrSetting 'XR_BLUETOOTH') -eq '1' -or $BluetoothBusId -or
        (Get-XrSetting 'XR_BLUETOOTH_INSTANCE_ID') -or $script:State.BluetoothInstanceId -or $script:BluetoothUsbIdExplicit)
}
function Set-SelectedBluetooth([object[]]$Devices, [string[]]$RuntimeInstanceIds = @()) {
    if ($WithoutBluetooth -or (Get-XrSetting 'XR_BLUETOOTH') -eq '0') {
        $script:State.BluetoothInstanceId = ''
        return $null
    }
    if (!(Test-BluetoothSelectionRequested)) { return $null }
    $instance = Get-XrSetting 'XR_BLUETOOTH_INSTANCE_ID'
    $bt = Select-BluetoothDevice -Devices $Devices -ExplicitBusId $BluetoothBusId -ExplicitInstanceId $instance `
        -UsbId $BluetoothUsbId -SavedInstanceId ([string]$script:State.BluetoothInstanceId)
    if (!$bt) { throw 'Set Bluetooth VID:PID / BUSID or XR_BLUETOOTH_INSTANCE_ID for the Bluetooth dongle.' }
    if ($bt.InstanceId -in $RuntimeInstanceIds) { throw 'Select a separate Bluetooth adapter; its USB device is already selected for camera / IMU access.' }
    $script:State.BluetoothInstanceId = [string]$bt.InstanceId
    Write-Step ('Selected Bluetooth adapter: ' + $bt.Description + ' [' + $bt.BusId + ']')
    return $bt
}
function Get-SelectedUsb([object[]]$Devices) {
    $selected = @(Select-RuntimeDevices -Devices $Devices -ExplicitBusIds $UsbBusId -SavedInstanceIds @($script:State.UsbInstanceIds))
    $script:State.UsbInstanceIds = @($selected | ForEach-Object { [string]$_.InstanceId })
    $bt = Set-SelectedBluetooth -Devices $Devices -RuntimeInstanceIds @($script:State.UsbInstanceIds)
    if ($bt) { $selected += $bt }
    Save-State; return $selected
}
function Connect-SelectedUsb {
    $script:State['CleanupSettings'] = $script:Settings.Clone()
    Save-State
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = Get-Tool 'wsl.exe'
    $info.Arguments = '--distribution ' + [string]$script:State.Distribution + ' --user root --exec cat'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true; $info.RedirectStandardInput = $true
    $lease = [Diagnostics.Process]::Start($info)
    try { Connect-SelectedUsbCore }
    finally {
        if ($lease) {
            $lease.StandardInput.Close()
            if (!$lease.WaitForExit(2000)) { $lease.Kill() }
            $lease.Dispose()
        }
    }
}
function Connect-SelectedUsbCore {
    $selected = @(Get-SelectedUsb @(Get-UsbDevices))
    # Remember ownership before attach. Cancellation, disabling Bluetooth or a
    # changed selection must not make Stop forget an already attached device.
    $script:State.AttachedUsbInstanceIds = @((@($script:State.AttachedUsbInstanceIds) +
        @($selected | ForEach-Object { [string]$_.InstanceId })) | Where-Object { $_ } | Select-Object -Unique)
    Save-State
    $firmware = $null
    if ($script:State.BluetoothInstanceId) {
        Write-Step 'Preparing Bluetooth firmware access in WSL...'
        $firmware = (Invoke-Linux -Operation firmware -Request (New-LinuxRequest) -AsRoot -Capture) | ConvertFrom-Json
        foreach ($message in @($firmware.messages)) { Write-Step ([string]$message) }
    }
    $selected = @($selected | ForEach-Object { Get-CurrentUsbDevice ([string]$_.InstanceId) })
    $unbound = @($selected | Where-Object { !$_.PersistedGuid } | ForEach-Object { [string]$_.BusId })
    if ($unbound.Count) { $null = Invoke-Elevated -Task Bind -BusIds $unbound }
    foreach ($d in $selected) {
        $d = Get-CurrentUsbDevice ([string]$d.InstanceId)
        $reconnect = $firmware -and $firmware.enabled -and !$firmware.ready -and $d.ClientIPAddress -and $d.InstanceId -eq $script:State.BluetoothInstanceId
        if ($reconnect) {
            Write-Step ('Reconnecting the selected Bluetooth dongle after firmware setup: ' + $d.BusId)
            Invoke-Native (Get-Tool 'usbipd.exe') @('detach','--busid',[string]$d.BusId)
        }
        if (!$d.ClientIPAddress -or $reconnect) {
            Write-Step ("Attaching " + $d.Description + ' [' + $d.BusId + '] to WSL')
            Invoke-Native (Get-Tool 'usbipd.exe') @('attach','--wsl','--busid',[string]$d.BusId)
        } else { Write-Step ("Already attached: " + $d.BusId) }
    }
    try { Invoke-Linux -Operation prepare -Request (New-LinuxRequest) -AsRoot }
    catch {
        $prepareError = $_
        if (!$script:State.BluetoothInstanceId) { throw }
        # The kernel may reveal a compressed firmware filename only on the first
        # probe. Materialize that file and retry once, for this dongle alone.
        $repair = (Invoke-Linux -Operation firmware -Request (New-LinuxRequest) -AsRoot -Capture) | ConvertFrom-Json
        foreach ($message in @($repair.messages)) { Write-Step ([string]$message) }
        if (!$repair.enabled -or (!$repair.changed -and !@($repair.repaired_files).Count)) { throw $prepareError }
        $bluetooth = @($selected | Where-Object { $_.InstanceId -eq $script:State.BluetoothInstanceId })
        if ($bluetooth.Count -ne 1) { throw $prepareError }
        $bluetooth = @(Get-CurrentUsbDevice ([string]$bluetooth[0].InstanceId))
        Write-Step 'Firmware repaired; retrying the selected Bluetooth dongle once...'
        Invoke-Native (Get-Tool 'usbipd.exe') @('detach','--busid',[string]$bluetooth[0].BusId)
        Invoke-Native (Get-Tool 'usbipd.exe') @('attach','--wsl','--busid',[string]$bluetooth[0].BusId)
        Invoke-Linux -Operation prepare -Request (New-LinuxRequest) -AsRoot
    }
}
function Get-SteamVrDirectory {
    if ($SteamVR) {
        try {
            $p = (Resolve-Path -LiteralPath $SteamVR -ErrorAction Stop).Path
            $tool = [IO.Path]::Combine($p, 'bin/win64/vrpathreg.exe')
            if (!(Test-Path -LiteralPath $tool -PathType Leaf -ErrorAction SilentlyContinue)) { throw 'vrpathreg.exe was not found.' }
        } catch {
            throw ("Configured SteamVR folder is unavailable: '" + $SteamVR + "'. Correct Settings > SteamVR folder / XR_STEAMVR_PATH, or clear it for automatic detection.")
        }
        return $p
    }
    $paths = New-Object 'System.Collections.Generic.List[string]'
    $vpath = [IO.Path]::Combine($env:LOCALAPPDATA, 'openvr/openvrpaths.vrpath')
    if (Test-Path -LiteralPath $vpath -PathType Leaf -ErrorAction SilentlyContinue) {
        $vr = Get-Content -Encoding UTF8 -Raw -LiteralPath $vpath | ConvertFrom-Json
        if ($vr.PSObject.Properties['runtime']) {
            foreach ($p in @($vr.runtime)) { if ($p) { $paths.Add([string]$p) } }
        }
    }
    $steamRoot = ''
    try { $steamRoot = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
    if (!$steamRoot) {
        $programs = [Environment]::GetFolderPath('ProgramFilesX86')
        if ($programs) { $steamRoot = [IO.Path]::Combine($programs, 'Steam') }
    }
    if ($steamRoot) {
        $paths.Add([IO.Path]::Combine($steamRoot, 'steamapps/common/SteamVR'))
        $libraries = [IO.Path]::Combine($steamRoot, 'steamapps/libraryfolders.vdf')
        if (Test-Path -LiteralPath $libraries -PathType Leaf -ErrorAction SilentlyContinue) {
            foreach ($m in [regex]::Matches((Get-Content -Encoding UTF8 -Raw -LiteralPath $libraries), '"path"\s+"([^"]+)"')) {
                $paths.Add([IO.Path]::Combine($m.Groups[1].Value.Replace('\\','\'), 'steamapps/common/SteamVR'))
            }
        }
    }
    foreach ($p in $paths) {
        $tool = [IO.Path]::Combine($p, 'bin/win64/vrpathreg.exe')
        if (Test-Path -LiteralPath $tool -PathType Leaf -ErrorAction SilentlyContinue) { return $p }
        Write-Step ("Skipping unavailable SteamVR location: " + $p)
    }
    return ''
}
function Register-XrDriver([switch]$AllowDeferred) {
    if (!$script:State.DriverPath -or !(Test-Path -LiteralPath $script:State.DriverPath)) { throw 'Download the Windows driver with -Action Install first.' }
    try { $svr = Get-SteamVrDirectory }
    catch {
        if (!$AllowDeferred) { throw }
        Write-XrWarning ($_.Exception.Message + ' Linux installation is complete; correct the SteamVR folder and run Register SteamVR driver.')
        return
    }
    if (!$svr -or (Get-Process vrserver,vrcompositor -ErrorAction SilentlyContinue)) {
        $message = 'Install SteamVR, run it once, close it, then run -Action Driver. Or pass -SteamVR D:\SteamLibrary\steamapps\common\SteamVR.'
        if ($AllowDeferred) { Write-XrWarning $message; return }; throw $message
    }
    $manifest = Get-Content -Encoding UTF8 -Raw -LiteralPath (Join-Path $script:State.DriverPath 'driver.vrdrivermanifest') | ConvertFrom-Json
    $reg = Join-Path $svr 'bin\win64\vrpathreg.exe'
    $vpath = [IO.Path]::Combine($env:LOCALAPPDATA, 'openvr/openvrpaths.vrpath')
    if (Test-Path -LiteralPath $vpath) {
        $vr = Get-Content -Encoding UTF8 -Raw -LiteralPath $vpath | ConvertFrom-Json
        Copy-Item -LiteralPath $vpath -Destination (Join-Path $script:StateDir ('openvrpaths.before-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.json'))
        $external = @()
        if ($vr.PSObject.Properties['external_drivers']) { $external = @($vr.external_drivers) }
        foreach ($old in $external) {
            if (!$old -or $old -eq $script:State.DriverPath) { continue }
            $m = [IO.Path]::Combine([string]$old, 'driver.vrdrivermanifest')
            if (Test-Path -LiteralPath $m -PathType Leaf -ErrorAction SilentlyContinue) {
                if ((Get-Content -Encoding UTF8 -Raw -LiteralPath $m | ConvertFrom-Json).name -eq $manifest.name) {
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
function Wait-XrStream([int]$Seconds, $RuntimeWindow = $null) {
    if (!('XrGateWsl.DeadlineLineReader' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'windows/DeadlineLineReader.cs')
    }
    $clock = [Diagnostics.Stopwatch]::StartNew(); $lastError = 'No connection.'; $sawFrames = $false
    $owner = $null
    if ($RuntimeWindow) {
        $owner = Get-RuntimeWindowProcess $RuntimeWindow
        if (!$owner) { Show-RuntimeExit $RuntimeWindow }
    }
    try {
        while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
            if ($owner -and $owner.HasExited) { Show-RuntimeExit $RuntimeWindow }
            $client = New-Object Net.Sockets.TcpClient
            try {
                $pending = $client.BeginConnect($script:TcpHost,$script:TcpPort,$null,$null)
                try {
                    $remaining = [Math]::Max(1, [Math]::Min(1500, ($Seconds * 1000 - [int]$clock.ElapsedMilliseconds)))
                    if (!$pending.AsyncWaitHandle.WaitOne([int]$remaining)) { throw 'Connection timeout.' }
                    $client.EndConnect($pending)
                } finally { $pending.AsyncWaitHandle.Close() }
                $reader = New-Object XrGateWsl.DeadlineLineReader($client.GetStream(), $clock, ($Seconds * 1000))
                if ($owner) {
                    $reader.Validate = [Action]({ if ($owner.HasExited) { throw 'The managed XR Gate process exited.' } }.GetNewClosure())
                }
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
            finally { $client.Close() }
            if ($owner -and $owner.HasExited) { Show-RuntimeExit $RuntimeWindow }
            $remaining = $Seconds * 1000 - [int]$clock.ElapsedMilliseconds
            if ($remaining -gt 0) { Start-Sleep -Milliseconds ([Math]::Min(400, $remaining)) }
        }
        if ($sawFrames) { throw 'TCP frames arrived, but fresh valid HMD poses were not observed. Check the xr_client console/startup gate.' }
        throw "XR Gate TCP is not ready: $lastError Check the xr_client console and WSL localhost forwarding."
    } finally { if ($owner) { $owner.Dispose() } }
}
function Start-RuntimeWindow {
    $old = Get-RuntimeWindowProcess (Read-RuntimeWindow)
    if ($old) { $old.Dispose(); throw 'A tracking console is still starting or stopping. Click Stop before retrying.' }
    $token = [Guid]::NewGuid().ToString('N')
    $files = Get-RuntimeWindowFiles ([pscustomobject]@{Token=$token})
    $body = '& ' + (Quote-PsLiteral $script:Self) + ' -Action Runtime -RuntimeLaunchToken ' + (Quote-PsLiteral $token)
    $body += ' -ResultPath ' + (Quote-PsLiteral $files.Result)
    if ($Launcher) { $body += ' -Launcher' }
    $body += '; exit $LASTEXITCODE'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded)
    if (!$Launcher) { $arguments = @('-NoExit') + $arguments }
    $style = 'Normal'
    if ($Launcher -and (Get-XrSetting 'XR_SHOW_COMMAND_PROMPT' '0') -ne '1') { $style = 'Hidden' }
    $process = Start-Process 'powershell.exe' -ArgumentList $arguments -WindowStyle $style -PassThru
    try {
        Write-RuntimeWindow @{Bundle=[IO.Path]::GetFullPath($PSScriptRoot); ProcessId=$process.Id;
            StartTicks=$process.StartTime.ToUniversalTime().Ticks.ToString(); Token=$token; Stopping=$false}
    } finally { $process.Dispose() }
}
function Disconnect-SelectedUsb {
    $owned = @((@($script:State.AttachedUsbInstanceIds) + @($script:State.UsbInstanceIds) +
        @($script:State.BluetoothInstanceId)) | Where-Object { $_ } | Select-Object -Unique)
    if (!$owned.Count) { return }
    $errors = New-Object 'System.Collections.Generic.List[string]'
    foreach ($d in @(Get-UsbDevices)) {
        if ($d.InstanceId -in $owned -and $d.ClientIPAddress) {
            try { Invoke-Native (Get-Tool 'usbipd.exe') @('detach','--busid',[string]$d.BusId) }
            catch { $errors.Add([string]$_.Exception.Message) }
        }
    }
    $remaining = @(Get-UsbDevices | Where-Object { $_.InstanceId -in $owned -and $_.ClientIPAddress })
    $script:State.AttachedUsbInstanceIds = @($remaining | ForEach-Object { [string]$_.InstanceId })
    Save-State
    if ($remaining.Count) {
        throw ('Some selected USB devices are still attached: ' + (($remaining | ForEach-Object { $_.BusId }) -join ', ') +
            '. Retry Stop. ' + ($errors -join ' '))
    }
    foreach ($message in $errors) { Write-XrWarning $message }
    Write-Step 'Selected USB devices returned to Windows.'
}
function Show-Help {
    Write-Host @'
XR Gate / WSL2 / no Docker
  -Action Install                 Install Ubuntu runtime and Windows driver.
  -Action Run [-StartSteamVR]      Attach USB, open xr_client; optionally start SteamVR.
  -Action Stop                    Stop this managed runtime, then return its USB devices.
  -Action Delete -DeleteDistributionName NAME  Permanently delete the saved Ubuntu and ALL its Linux data.
  -Action Status                  Report installed/running state as JSON.
  -Action Usb                     Attach devices selected in the hardware env.
  -Action Usb -BluetoothBusId 1-4  Also attach your dedicated USB Bluetooth dongle.
  -Action Usb -BluetoothUsbId VID:PID  Select a dongle by USB identity when BUSID is empty.
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
    if ($Action -eq 'Admin') { $result = Invoke-AdminTask; Complete-XrAction ([int]$result) }
    if ($Action -eq 'Delete') {
        if (!$DeleteDistributionName) { throw 'Deletion requires the exact Ubuntu distribution name confirmed in the launcher.' }
        $Distribution = $DeleteDistributionName
    }
    Load-State
    if ($Action -eq 'Delete') {
        Remove-XrDistribution $DeleteDistributionName
        Complete-XrAction 0 ('Ubuntu distribution ' + $DeleteDistributionName + ' and its Linux data were deleted. Click Install to set up XR Gate again.')
    }
    if ($Action -eq 'DeviceStatus') {
        $script:ActionData = Get-XrDeviceObservation -Devices @(Get-UsbDevices)
        $setup = Get-XrSetupObservation $false
        foreach ($key in @('display_selected','display_connected','display_number')) { $script:ActionData[$key] = $setup[$key] }
        $script:ActionData | ConvertTo-Json -Compress
        Complete-XrAction 0
    }
    if ($Action -eq 'Status') {
        $status = @{installed=$false; running=$false}
        if ($script:State.LinuxUser) {
            $status = (Invoke-Linux -Operation status -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
        }
        $script:ActionData = @{installed=[bool]$status.installed; running=[bool]$status.running;
            distribution=[string]$script:State.Distribution; release=[string]$script:State.ReleaseTag;
            frequency_hz=[int]$script:State.FrequencyHz; display_mode=[string]$script:State.DisplayMode;
            can_delete=([bool]$script:State.LinuxUser -or [bool]$script:State.InstallPending)}
        $setup = Get-XrSetupObservation ([bool]$status.installed)
        foreach ($key in $setup.Keys) { $script:ActionData[$key] = $setup[$key] }
        $script:ActionData['starting'] = $false
        $windowRecord = Read-RuntimeWindow
        $window = Get-RuntimeWindowProcess $windowRecord
        if ($window) {
            try { $script:ActionData['starting'] = !$status.running -and !$windowRecord.Stopping }
            finally { $window.Dispose() }
        }
        if (!$status.running -and !$script:ActionData['starting'] -and $windowRecord -and !$windowRecord.Stopping) {
            $exit = Get-RuntimeExitObservation $windowRecord
            $script:ActionData['runtime_exit_message'] = $exit.message
            $script:ActionData['runtime_log_tail'] = $exit.log_tail
        }
        $displayObservation = Get-XrDisplayObservation
        $script:ActionData['display_frequency_hz'] = $displayObservation.frequency_hz
        $script:ActionData['display_name'] = $displayObservation.display_name
        $script:ActionData['display_detection_message'] = $displayObservation.message
        try {
            $device = Get-XrDeviceObservation -Devices @(Get-UsbDevices)
            foreach ($key in $device.Keys) { $script:ActionData[$key] = $device[$key] }
        } catch { Write-XrWarning ('Device status is unavailable: ' + $_.Exception.Message) }
        $script:ActionData | ConvertTo-Json -Compress
        Complete-XrAction 0
    }
    if ($Action -eq 'Install') {
        $displaySetup = Get-XrSetupObservation $false
        if (!$displaySetup.display_selected -or !$displaySetup.display_connected) {
            throw 'Select the connected glasses display number before Install.'
        }
        $script:State.InstallPending = $true
        $script:State.InstallNextStep = ''
        Save-State
        $rc = Invoke-Elevated -Task Prerequisites
        if ($rc -eq 20) {
            $script:NextStep = 'restart_windows'
            Write-Host 'Save work, restart Windows, and run Install again.' -ForegroundColor Yellow
            Complete-XrAction 20
        }
        if (!(Ensure-Distribution)) { Complete-XrAction 20 }
        if (Test-BluetoothSelectionRequested) { Set-SelectedBluetooth -Devices @(Get-UsbDevices) | Out-Null }
        Save-State
        Write-Step 'Preparing Python in the selected Ubuntu.'
        $wsl = Get-Tool 'wsl.exe'
        Invoke-Native $wsl @('-d',[string]$script:State.Distribution,'-u','root','--exec','env','DEBIAN_FRONTEND=noninteractive','apt-get','update')
        Invoke-Native $wsl @('-d',[string]$script:State.Distribution,'-u','root','--exec','env','DEBIAN_FRONTEND=noninteractive','apt-get','install','-y','--no-install-recommends','python3')
        $release = Get-ReleaseInfo $ReleaseTag; Write-Step ("Selected release: " + $release.tag_name)
        $runtimeAsset = Select-ReleaseAsset $release 'xr-gate-ubuntu-24.04-linux-x64.tar.gz'
        $modelsAsset = Select-ReleaseAsset $release 'hand-tracking-models-mercury.tar.gz'
        $cache = Join-Path $script:StateDir ('cache\' + $release.tag_name)
        if ((Get-XrSetting 'XR_AUTO_DRIVER_FREQUENCY') -eq '1') { Select-AutomaticDriverFrequency $release $cache }
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
            $script:NextStep = 'restart_ubuntu'
            Write-Host 'Then run Install again. Other distributions do not need to be stopped.'
            Complete-XrAction 20
        }
        Install-SelectedWindowsDriver $driverPackage ([string]$release.tag_name)
        Register-XrDriver -AllowDeferred
        Write-Host 'Installation complete. Next: enable SBS, select the matching Hz in Windows, then click Start VR. See START-HERE.html.' -ForegroundColor Green
        Complete-XrAction 0 'Installation complete. Click Start VR.'
    }
    if ($Action -eq 'Display') {
        Choose-SbsDisplay -SelectOnly
        Complete-XrAction 0 'Display selected. Click Install if setup is unfinished; otherwise click Start VR.'
    }
    if (!$script:State.LinuxUser -and $Action -eq 'Doctor') {
        foreach ($tool in @('wsl.exe','usbipd.exe')) {
            try { Invoke-Native (Get-Tool $tool) @('--version') }
            catch { Write-XrWarning $_.Exception.Message }
        }
        Write-Host 'Linux setup is not complete. Click Install first.'
        Complete-XrAction 0
    }
    if ($Action -eq 'Stop') {
        Request-RuntimeWindowStop
        if ($script:State.LinuxUser) { Invoke-Linux -Operation stop -Request (New-LinuxRequest) }
        Stop-RuntimeWindow
        Disconnect-SelectedUsb
        Complete-XrAction 0 'All managed runtime services stopped and selected USB devices returned to Windows.'
    }
    if (!$script:State.LinuxUser) { throw 'Run -Action Install first.' }
    switch ($Action) {
        'Run' {
            if ($Launcher -and $StartSteamVR) {
                $setup = Get-XrSetupObservation $true
                if (!$setup.setup_complete) { throw 'Finish Install before starting VR.' }
                if (!$setup.display_selected -or !$setup.display_connected) { throw 'Select the connected glasses display before starting VR.' }
            }
            if ((Get-XrSetting 'XR_AUTO_DRIVER_FREQUENCY') -eq '1') { Update-AutomaticWindowsDriver }
            if ($StartSteamVR -and !(Get-Process vrserver,vrcompositor -ErrorAction SilentlyContinue)) {
                if ($script:State.DisplayMode -eq 'extended_sbs') { Update-SavedSbsDisplay }
                else { Set-XrDisplaySettings -X 0 -Y 0 -Mode direct }
            }
            $status = (Invoke-Linux -Operation status -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
            if (!$status.installed) { throw 'Linux runtime is not installed. Run -Action Install.' }
            if ($status.PSObject.Properties['busy'] -and $status.busy -and !$status.running) { throw 'Controller setup is still running. Finish it or click Stop before starting VR.' }
            $runtimeWindow = $null
            if (!$status.running) {
                Connect-SelectedUsb; Save-State; Start-RuntimeWindow
                $runtimeWindow = Read-RuntimeWindow
                if (!$Launcher -or (Get-XrSetting 'XR_SHOW_COMMAND_PROMPT') -eq '1') {
                    Write-Step 'xr_client opened in a separate console. Complete its startup prompts there.'
                } else { Write-Step 'xr_client started in the background. Use Show command prompt before starting when console calibration is needed.' }
            } else {
                Write-Step 'The managed xr_client is already running.'
                $record = Read-RuntimeWindow
                $owner = Get-RuntimeWindowProcess $record
                if ($owner) { $runtimeWindow = $record; $owner.Dispose() }
            }
            if ($StartSteamVR) {
                Ensure-XrDriverRegistration
                Write-Step 'Waiting for valid updating HMD poses before starting SteamVR...'
                Wait-XrStream $TimeoutSeconds -RuntimeWindow $runtimeWindow; Start-Process 'steam://rungameid/250820'
            }
        }
        'Runtime' { Invoke-Linux -Operation run -Request (New-LinuxRequest) }
        'Usb' { Connect-SelectedUsb }
        'Detach' {
            $status = (Invoke-Linux -Operation status -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
            if ($status.running -or ($status.PSObject.Properties['busy'] -and $status.busy)) { throw 'Stop tracking or controller setup before detaching USB.' }
            Disconnect-SelectedUsb
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
        'BluetoothScan' {
            if (!(Test-BluetoothSelectionRequested)) { throw 'Enable Bluetooth in Controllers first.' }
            Connect-SelectedUsb
            $script:ActionData = (Invoke-Linux -Operation bluetooth_scan -Request (New-LinuxRequest) -Capture) | ConvertFrom-Json
        }
        'Pair' {
            if (!(Test-BluetoothSelectionRequested)) { throw 'Enable Bluetooth in Controllers first.' }
            Connect-SelectedUsb
            $request = New-LinuxRequest
            if ($LeftControllerMac -or $RightControllerMac) {
                $request.left_controller = $LeftControllerMac
                $request.right_controller = $RightControllerMac
                $script:ActionData = (Invoke-Linux -Operation pair -Request $request -Capture) | ConvertFrom-Json
            } else { Invoke-Linux -Operation pair -Request $request }
        }
        'Controllers' { Connect-SelectedUsb; Invoke-Linux -Operation controllers -Request (New-LinuxRequest) }
    }
    Complete-XrAction 0
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'See START-HERE.html. For device/runtime details use -Action Doctor.'
    Complete-XrAction 1 $_.Exception.Message
}

