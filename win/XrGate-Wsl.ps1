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
    [string]$ReleaseTag = 'latest',
    [string]$XrealBusId = '',
    [string]$BluetoothBusId = '',
    [switch]$WithoutBluetooth,
    [switch]$GearVR,
    [string]$ControllerConfig = '',
    [ValidateSet(0,60,90)][int]$FrequencyHz = 0,
    [ValidateSet('','direct','extended_sbs')][string]$DisplayMode = '',
    [ValidateSet('','current','60hz','90hz','prompt')][string]$DisplayOption = '',
    [string]$DisplayName = '',
    [string]$SteamVR = '',
    [switch]$StartSteamVR,
    [ValidateRange(5,600)][int]$TimeoutSeconds = 180,
    [ValidateSet('Prerequisites','Bind')][string]$AdminTask = 'Prerequisites',
    [string[]]$BindBusId = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Repo = 'vladoshub/xr-gate'
$script:Self = $PSCommandPath
$script:StateDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'XR-Gate-WSL'
$script:StateFile = Join-Path $script:StateDir 'state.json'
$script:State = [ordered]@{
    Distribution = 'Ubuntu-24.04'; LinuxUser = ''; ReleaseTag = ''
    XrealInstanceId = ''; BluetoothInstanceId = ''
    FrequencyHz = 60; DisplayMode = 'extended_sbs'; DisplayOption = 'current'
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
    if ($Distribution) { $script:State.Distribution = $Distribution }
    if ($FrequencyHz) { $script:State.FrequencyHz = $FrequencyHz }
    if ($DisplayMode) { $script:State.DisplayMode = $DisplayMode }
    if ($DisplayOption) { $script:State.DisplayOption = $DisplayOption }
    if ($GearVR) { $script:State.GearVR = $true }
    if ($WithoutBluetooth) { $script:State.BluetoothInstanceId = '' }
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
    if ($matches.Count -ne 1) { throw "Release $($Release.tag_name) has no unique asset $Name. Use v2.8.0 or a newer complete release." }
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
function Get-UsbDevices {
    $lines = @(& (Get-Tool 'usbipd.exe') state)
    if ($LASTEXITCODE -ne 0) { throw 'usbipd state failed; install a current usbipd-win.' }
    $data = ($lines -join [Environment]::NewLine) | ConvertFrom-Json
    return @($data.Devices | Where-Object { $_.BusId })
}
function Select-XrealDevice([object[]]$Devices, [string]$ExplicitBusId = '', [string]$SavedInstanceId = '') {
    $matches = @($Devices | Where-Object { $_.InstanceId -match '^USB\\VID_3318&PID_0426\\' })
    if ($ExplicitBusId) { $matches = @($matches | Where-Object { $_.BusId -eq $ExplicitBusId }) }
    elseif ($SavedInstanceId) {
        $saved = @($matches | Where-Object { $_.InstanceId -eq $SavedInstanceId })
        if ($saved.Count) { $matches = $saved }
    }
    if (!$matches.Count) { throw 'XREAL Air 2 Ultra USB (3318:0426) was not found. Connect it and run -Action Usb.' }
    if ($matches.Count -gt 1) {
        $matches | Format-Table BusId,Description,InstanceId | Out-Host
        throw 'More than one XREAL is connected. Specify -XrealBusId BUSID.'
    }
    return $matches[0]
}
function Get-SelectedUsb([object[]]$Devices) {
    $x = Select-XrealDevice -Devices $Devices -ExplicitBusId $XrealBusId -SavedInstanceId ([string]$script:State.XrealInstanceId)
    $script:State.XrealInstanceId = [string]$x.InstanceId; $selected = @($x)
    if ($BluetoothBusId) {
        $bt = @($Devices | Where-Object { $_.BusId -eq $BluetoothBusId })
        if ($bt.Count -ne 1 -or $bt[0].InstanceId -eq $x.InstanceId) { throw 'Select the dedicated USB Bluetooth dongle BUSID from usbipd list.' }
        $script:State.BluetoothInstanceId = [string]$bt[0].InstanceId
    }
    if ($script:State.BluetoothInstanceId) {
        $bt = @($Devices | Where-Object { $_.InstanceId -eq $script:State.BluetoothInstanceId })
        if ($bt.Count -ne 1) { throw 'Saved Bluetooth dongle not found. Use -BluetoothBusId NEW_BUSID or -WithoutBluetooth.' }
        $selected += $bt[0]
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
            $pending = $client.BeginConnect('127.0.0.1',45810,$null,$null)
            if (!$pending.AsyncWaitHandle.WaitOne(1500)) { throw 'Connection timeout.' }
            $client.EndConnect($pending); $client.GetStream().ReadTimeout = 2000
            $reader = New-Object IO.StreamReader($client.GetStream())
            $helloLine = $reader.ReadLine()
            if (!$helloLine) { throw 'No XR Gate hello.' }
            $hello = $helloLine | ConvertFrom-Json
            if ($hello.protocol -ne 'xr_gate_app_v1') { throw 'Port 45810 is serving a different protocol.' }
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
  -Action Usb                     Detect/attach XREAL and save the selection.
  -Action Usb -BluetoothBusId 1-4  Also attach your dedicated USB Bluetooth dongle.
  -Action Detach                  Return the selected USB devices to Windows.
  -Action Doctor                  Show Windows/WSL/USB/runtime diagnostics.
  -Action Test                    Wait for TCP and valid updating HMD frames.
  -Action Driver                  Register the downloaded SteamVR driver.
  -Action Display                 Select/save the glasses' extended SBS display.
  -Action Pair                    Open bluetoothctl in WSL as root.
  -Action Controllers             Register Gear VR / train another layout.
Options:
  -ReleaseTag v2.8.0               Pin a release (Install defaults to latest).
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
        if ($BluetoothBusId) {
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
        $driverName = 'xr-gate-steamvr-windows-x64-' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '.zip'
        $driverAsset = Select-ReleaseAsset $release $driverName
        $cache = Join-Path $script:StateDir ('cache\' + $release.tag_name)
        $runtimeArchive = Save-ReleaseAsset $runtimeAsset $cache
        $modelsArchive = Save-ReleaseAsset $modelsAsset $cache
        $driverArchive = Save-ReleaseAsset $driverAsset $cache
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
        $driverDir = Join-Path $script:StateDir ('drivers\' + $release.tag_name + '-' + $script:State.FrequencyHz + 'Hz-' + $script:State.DisplayMode + '-' + ([string]$driverAsset.digest).Substring(7,12))
        if (!(Test-Path -LiteralPath (Join-Path $driverDir 'xr_tracking\driver.vrdrivermanifest'))) { Expand-Archive -LiteralPath $driverArchive -DestinationPath $driverDir -Force }
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
                if ($d.InstanceId -in @($script:State.XrealInstanceId,$script:State.BluetoothInstanceId) -and $d.ClientIPAddress) {
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

