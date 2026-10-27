#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$windowsRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Split-Path -Parent $windowsRoot
. (Join-Path $windowsRoot 'XrGate-Wsl.ps1') -Action Help *> $null
$script:Settings['XR_TARGET_DEVICE'] = 'xreal_ultra'
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-contracts-' + [Guid]::NewGuid())
$previousLocalAppData = $env:LOCALAPPDATA
$shell = (Get-Process -Id $PID).Path
$utf8 = New-Object Text.UTF8Encoding($false)
New-Item -ItemType Directory $temporary | Out-Null
try {
    $env:LOCALAPPDATA = $temporary
    $script:StateDir = $temporary
    $script:StateFile = Join-Path $temporary 'state.json'
    $unicode = [string][char]0x0410 + [char]0x043D + [char]0x043D + [char]0x0430
    $driver = Join-Path $temporary ("O'Brien " + $unicode)
    $driverSettings = Join-Path $driver 'resources/settings/default.vrsettings'
    New-Item -ItemType Directory (Split-Path -Parent $driverSettings) -Force | Out-Null
    Copy-Item (Join-Path $sourceRoot 'drivers/openvr_driver/resources/settings/default.vrsettings') $driverSettings
    # Installed packages include device metadata added by the production renderer.
    & python (Join-Path $sourceRoot 'drivers/openvr_driver/scripts/render_display_settings.py') `
        --settings $driverSettings --device-profile (Get-XrDriverDeviceId) --display-frequency 60 --display-mode direct
    Assert ($LASTEXITCODE -eq 0) 'The real driver package settings could not be rendered.'
    $before = (Get-FileHash -LiteralPath $driverSettings).Hash
    $configuration = Get-XrDriverConfiguration $driverSettings
    Assert ($configuration.FrequencyHz -eq 60 -and !$configuration.OnDesktop) 'The real driver JSON with case-distinct keys could not be read.'
    Assert ($configuration.Device -ceq (Get-XrDriverDeviceId)) 'The installed driver lost its selected device profile.'
    Assert ((Get-FileHash -LiteralPath $driverSettings).Hash -eq $before) 'Reading driver configuration modified its JSON.'
    $script:State.DriverPath = $driver
    $script:State.RegisteredDriver = $driver
    $script:State.ReleaseTag = 'audit'
    Save-State
    $script:State.DriverPath = ''
    Load-State
    Assert ($script:State.DriverPath -ceq $driver) 'UTF-8 state without BOM corrupted a Unicode driver path.'

    $script:RegistrationCalls = 0
    function Select-AutomaticDriverFrequency {
        $script:State.FrequencyHz = 60
        $script:State.DisplayMode = 'direct'
    }
    function Register-XrDriver {
        $script:RegistrationCalls++
        $openvr = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
        New-Item -ItemType Directory (Split-Path -Parent $openvr) -Force | Out-Null
        [IO.File]::WriteAllText($openvr, (@{external_drivers=@([string]$script:State.DriverPath)} | ConvertTo-Json), $utf8)
        $script:State.RegisteredDriver = $script:State.DriverPath
    }
    Update-AutomaticWindowsDriver
    Assert ($script:RegistrationCalls -eq 1) 'Missing OpenVR registration was trusted because of the local cache.'
    Update-AutomaticWindowsDriver
    Assert ($script:RegistrationCalls -eq 1) 'An already registered driver was registered twice.'
    $openvr = Join-Path $temporary 'openvr/openvrpaths.vrpath'
    [IO.File]::WriteAllText($openvr, (@{external_drivers=@($driver.ToUpperInvariant() + [IO.Path]::DirectorySeparatorChar)} | ConvertTo-Json), $utf8)
    Assert (Test-XrDriverRegistration $driver) 'Normalized Windows driver paths did not compare case-insensitively.'
    [IO.File]::WriteAllText($openvr, '{"external_drivers":[]}', $utf8)
    Ensure-XrDriverRegistration
    Assert ($script:RegistrationCalls -eq 2) 'Registration removed by SteamVR was not restored.'

    # Execute the real encoded elevated-command body in a child PowerShell,
    # replacing only UAC and the prerequisite script with a harmless fixture.
    $parentExitCode = Get-Variable LASTEXITCODE -ValueOnly -ErrorAction SilentlyContinue
    $script:Self = Join-Path $temporary ("admin O'Brien " + $unicode + '.ps1')
    $fixture = @'
param([string]$Action,[string]$AdminTask,[switch]$Launcher,[string]$ResultPath,[string[]]$BindBusId)
$code = [int]$env:XR_GATE_ADMIN_FIXTURE_CODE
$result = @{exit_code=$code;next_step='';message='fixture administrator detail';warnings=@();data=@{}}
[IO.File]::WriteAllText($ResultPath, ($result | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
exit $code
'@
    [IO.File]::WriteAllText($script:Self, $fixture, (New-Object Text.UTF8Encoding($true)))
    function Start-Process {
        param([string]$FilePath,[string]$Verb,[switch]$Wait,[switch]$PassThru,[string[]]$ArgumentList)
        Assert ($Verb -eq 'RunAs') 'The fixture no longer exercises the UAC invocation.'
        # Match the real Start-Process exit-code contract. Calling the child
        # with & leaks its intentionally nonzero exit into LASTEXITCODE, which
        # GitHub Actions then treats as the result of the successful test step.
        $process = Microsoft.PowerShell.Management\Start-Process -FilePath $shell -NoNewWindow -Wait -PassThru `
            -ArgumentList (@('-OutputFormat','Text') + $ArgumentList)
        try { return [pscustomobject]@{ExitCode=$process.ExitCode} }
        finally { $process.Dispose() }
    }
    foreach ($code in @(0,20)) {
        $env:XR_GATE_ADMIN_FIXTURE_CODE = [string]$code
        $observed = Invoke-Elevated -Task Prerequisites
        Assert ($observed -eq $code) ('Administrator exit code was lost: ' + $code)
    }
    $env:XR_GATE_ADMIN_FIXTURE_CODE = '1'
    $rejected = $false
    try { Invoke-Elevated -Task Bind -BusIds @('2-3') }
    catch { $rejected = $_.Exception.Message.Contains('fixture administrator detail') }
    Assert $rejected 'The administrator failure lost its structured error detail.'
    Assert (@(Get-ChildItem -LiteralPath $temporary -Filter 'admin-result-*').Count -eq 0) 'Administrator result files were not cleaned up.'
    Assert ((Get-Variable LASTEXITCODE -ValueOnly -ErrorAction SilentlyContinue) -eq $parentExitCode) 'The administrator fixture leaked its native exit code into the caller.'
    'OK real driver JSON, UTF-8 state, actual OpenVR registration and administrator exit/result contract'
} finally {
    Remove-Item Function:Start-Process -ErrorAction SilentlyContinue
    Remove-Item Env:XR_GATE_ADMIN_FIXTURE_CODE -ErrorAction SilentlyContinue
    $env:LOCALAPPDATA = $previousLocalAppData
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
