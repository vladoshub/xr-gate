#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Must-Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action } catch { $rejected = $true }
    Assert $rejected $Message
}

# Exercise native-enumeration/WMI joining without reading actual Windows devices.
Add-Type @'
namespace XrGateWsl {
    public static class DisplayNative {
        public static object[] Displays;
        public static object[] Enumerate() { return Displays; }
    }
}
'@
$ultraId = '\\?\DISPLAY#MRG3136#5&3376c635&0&UID4354#{monitor-interface}'
$mainId = '\\?\DISPLAY#ACR0D1F#5&3376c635&0&UID4358#{monitor-interface}'
$ultra = [pscustomobject]@{DeviceName='\\.\DISPLAY3';DeviceId=$ultraId;Name='Generic PnP Monitor';Width=3840;Height=1080;Frequency=90}
$main = [pscustomobject]@{DeviceName='\\.\DISPLAY1';DeviceId=$mainId;Name='Z57';Width=3840;Height=1080;Frequency=90}
[XrGateWsl.DisplayNative]::Displays = @($main, $ultra)
$script:EdidName = 'Air 2 Ultra'
function Get-CimInstance {
    param($Namespace, $ClassName, $ErrorAction)
    Assert ($Namespace -eq 'root\wmi' -and $ClassName -eq 'WmiMonitorID') 'Unexpected WMI query.'
    return @(
        [pscustomobject]@{Active=$true;InstanceName='DISPLAY\MRG3136\5&3376c635&0&UID4354_0';UserFriendlyName=[Text.Encoding]::ASCII.GetBytes($script:EdidName)},
        [pscustomobject]@{Active=$true;InstanceName='DISPLAY\ACR0D1F\5&3376c635&0&UID4358_0';UserFriendlyName=[Text.Encoding]::ASCII.GetBytes('Z57')}
    )
}
$script:Settings['XR_TARGET_DEVICE'] = 'fixture'
$script:Settings['XR_DISPLAY_MODEL_ID'] = 'MRG3136'
$script:Settings['XR_DISPLAY_MODEL_NAME'] = 'Air 2 Ultra'
$script:Settings['SBS_RESOLUTION'] = '3840x1080'
$script:State = [ordered]@{DriverPath='';RegisteredDriver='';InstallPending=$false;InstallNextStep='';DisplayDeviceId='';DisplayDeviceName='';DisplayTargetDevice='fixture'}
$DisplayName = ''
Assert ((Get-XrMonitorIdentityKey $ultraId) -eq (Get-XrMonitorIdentityKey 'DISPLAY\MRG3136\5&3376c635&0&UID4354_0' -Wmi)) 'WMI and monitor interface identities did not join.'
$displays = @(Get-DesktopDisplays)
Assert ($ultra.Name -eq 'Air 2 Ultra' -and $ultra.EdidName -eq 'Air 2 Ultra') 'The EDID name was replaced by Generic PnP Monitor.'
Assert ((Find-AutomaticXrDisplay $displays).DeviceId -eq $ultraId) 'Exact Ultra identity did not select its output.'
Must-Reject { Select-DesktopDisplay $displays -Name $main.DeviceName } 'Manual selection bypassed the configured model/name checks.'
$script:EdidName = 'Air 2 Pro'
$script:State.DisplayDeviceId = $ultraId
Assert ($null -eq (Find-AutomaticXrDisplay @(Get-DesktopDisplays))) 'A saved device ID bypassed the required EDID name.'
$script:EdidName = 'Air 2 Ultra'
$ultra.DeviceId = $ultraId.Replace('MRG3136','MRG3137')
$ultra | Add-Member -NotePropertyName EdidName -NotePropertyValue 'Air 2 Ultra' -Force
Assert (!(Test-XrDisplayIdentity $ultra)) 'The correct name bypassed a wrong model code.'
$ultra.DeviceId = $ultraId
$script:State.DisplayDeviceId = ''
$null = Get-DesktopDisplays
$extra = [pscustomobject]@{DeviceName='\\.\DISPLAY4';DeviceId=$ultraId.Replace('UID4354','UID9999');Name='Air 2 Ultra';EdidName='Air 2 Ultra';Width=3840;Height=1080;Frequency=90}
Assert ($null -eq (Find-AutomaticXrDisplay @($ultra,$extra))) 'Two identical Ultra displays were guessed automatically.'
Assert ((Get-XrSetupObservation $false $true).sbs_enabled -eq $true) 'A matching current resolution did not report SBS enabled.'
$ultra.Width = 1920
$status = Get-XrSetupObservation $false $true
Assert ($status.display_connected -and $status.sbs_required -and $status.sbs_enabled -eq $false -and $status.display_width -eq 1920) 'A connected monoscopic output did not report SBS OFF.'
Must-Reject { Assert-XrSbsEnabled } 'Starting VR with SBS OFF was accepted.'
$ultra.Width = 3840
Assert-XrSbsEnabled
$ultra.Height = 1200
Must-Reject { Assert-XrSbsEnabled } 'SBS validation checked only the width.'
$ultra.Height = 1080
[XrGateWsl.DisplayNative]::Displays = @($main)
$status = Get-XrSetupObservation $false $true
Assert (!$status.display_connected -and $null -eq $status.sbs_enabled -and $null -eq $status.display_width) 'An absent display was falsely identified as SBS OFF.'
$script:Settings.Remove('SBS_RESOLUTION')
Assert ((Get-XrSbsObservation $null).sbs_enabled -eq $true) 'A device without SBS_RESOLUTION was required to switch modes.'
Assert-XrSbsEnabled
$script:Settings['SBS_RESOLUTION'] = ' '
Assert-XrSbsEnabled
$script:Settings.Remove('SBS_RESOLUTION')
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-sbs-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $envFile = Join-Path $temporary 'device.env'
    'SBS_RESOLUTION=2560X1440' | Set-Content -Encoding UTF8 -LiteralPath $envFile
    Assert ((Read-XrEnv $envFile)['SBS_RESOLUTION'] -eq '2560X1440') 'The device SBS setting was rejected by the env parser.'
    'SBS_RESOLUTION=broken' | Set-Content -Encoding UTF8 -LiteralPath $envFile
    Must-Reject { Read-XrEnv $envFile } 'Malformed SBS_RESOLUTION silently disabled mode validation.'
    $state = Join-Path $temporary 'state.json'
    @{CleanupSettings=@{XR_DISTRIBUTION='Fixture';SBS_RESOLUTION='3840x1080'}} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $state
    Initialize-XrStopSettings $temporary $state
    Assert ((Get-XrSetting 'SBS_RESOLUTION') -eq '3840x1080') 'SBS metadata prevented cleanup of a saved session.'
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
Write-Host 'OK: EDID model AND name, exact monitor instance, ambiguity, SBS on/off/unknown, optional devices, launch rejection and cleanup compatibility.'
