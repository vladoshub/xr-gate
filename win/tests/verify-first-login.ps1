#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-wsl-update-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $fixture = Join-Path $temporary "O'Brien update fixture.ps1"
    [IO.File]::WriteAllText($fixture, "Write-Output 'fixture stdout'; [Console]::Error.WriteLine('fixture stderr'); exit 7", (New-Object Text.UTF8Encoding($true)))
    $previousExit = Get-Variable LASTEXITCODE -ValueOnly -ErrorAction SilentlyContinue
    $native = Invoke-XrSetupCommand (Get-Process -Id $PID).Path @('-NoLogo','-NoProfile','-File',$fixture)
    Assert ($native.ExitCode -eq 7 -and ($native.Output -join ' ') -like '*fixture stdout*' -and ($native.Output -join ' ') -like '*fixture stderr*') 'UAC diagnostics lost native stdout/stderr or the actual exit code.'
    Assert ((Get-Variable LASTEXITCODE -ValueOnly -ErrorAction SilentlyContinue) -eq $previousExit) 'Capturing WSL diagnostics changed the caller native exit code.'
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }

function Test-WslUpdateRecovery {
    $script:UpdateResults = @(); $script:UpdateCalls = 0
    function Invoke-XrSetupCommand($File, $Arguments) {
        Assert ($File -eq 'fixture-wsl' -and ($Arguments -join ' ') -eq '--update --web-download') 'Recovery changed the WSL update command or targeted a Linux distribution.'
        $result = $script:UpdateResults[$script:UpdateCalls]
        $script:UpdateCalls++
        return $result
    }
    function UpdateResult($Code, $Text='') { return [pscustomobject]@{ExitCode=$Code;Output=@($Text)} }
    foreach ($code in @(0,3010)) {
        $script:UpdateResults = @(UpdateResult $code); $script:UpdateCalls = 0
        Assert ((Update-XrWsl 'fixture-wsl' -RetryDelayMilliseconds 0) -eq $code -and $script:UpdateCalls -eq 1) 'A successful WSL update or required reboot was retried.'
        $script:UpdateResults = @((UpdateResult 1 'fixture initial failure'), (UpdateResult $code)); $script:UpdateCalls = 0
        $before = $script:ActionWarnings.Count
        Assert ((Update-XrWsl 'fixture-wsl' -RetryDelayMilliseconds 0) -eq $code -and $script:UpdateCalls -eq 2 -and $script:ActionWarnings.Count -eq $before + 1) 'A transient WSL failure did not recover once, lost reboot handling or hid the recovery warning.'
        Assert ($script:ActionWarnings[-1].Contains('fixture initial failure')) 'A successful retry discarded diagnostics from the initial WSL failure.'
    }
    foreach ($case in @('repeated','other','empty')) {
        $script:UpdateResults = switch ($case) {
            'repeated' { (UpdateResult 1 'first failure'); (UpdateResult 1 'fixture final service diagnostic') }
            'other' { (UpdateResult 5 'fixture permission diagnostic') }
            'empty' { (UpdateResult 5) }
        }
        $script:UpdateResults = @($script:UpdateResults); $script:UpdateCalls = 0
        $failure = ''
        try { Update-XrWsl 'fixture-wsl' -RetryDelayMilliseconds 0 | Out-Null } catch { $failure = $_.Exception.Message }
        $expected = switch ($case) { 'repeated' { 'fixture final service diagnostic' }; 'other' { 'fixture permission diagnostic' }; 'empty' { 'no diagnostic output' } }
        $calls = if ($case -eq 'repeated') { 2 } else { 1 }
        Assert ($failure.Contains($expected) -and $script:UpdateCalls -eq $calls) 'WSL update swallowed a persistent failure, lost diagnostics or retried more than once.'
        if ($case -eq 'repeated') { Assert ($failure.Contains('first failure')) 'The retry discarded diagnostics from the initial WSL failure.' }
    }
}
Test-WslUpdateRecovery
Add-Type @'
using System;
public sealed class LoginProbeFixture : IDisposable {
    public bool Finished { get; set; }
    public int ExitCode { get; set; }
    public bool Disposed;
    private string text;
    public LoginProbeFixture(string name, bool finished, int code) { text=name; Finished=finished; ExitCode=code; }
    public object ReadOutput() { if(text==null) return null; var line=new LoginLineFixture(text); text=null; return line; }
    public void Dispose() { Disposed=true; }
}
public sealed class LoginLineFixture {
    public string Text { get; private set; }
    public bool IsError { get { return false; } }
    public LoginLineFixture(string value) { Text=value; }
}
'@
function New-XrLoginProbe { return $script:Probe }
foreach ($userName in @('annterr','fixture_user','somebody-else')) {
    $script:Probe = New-Object LoginProbeFixture($userName, $true, 0)
    Assert ((Get-XrLoginUser) -ceq $userName -and $script:Probe.Disposed) 'The created Linux username was not detected dynamically.'
}
foreach ($userName in @('root', '', 'Enter new username:', "name`nother")) {
    $script:Probe = New-Object LoginProbeFixture($userName, $true, 0)
    Assert (!(Get-XrLoginUser)) 'A first-login prompt or root was mistaken for a configured user.'
}
$script:Probe = New-Object LoginProbeFixture('', $false, 0)
$clock = [Diagnostics.Stopwatch]::StartNew()
Assert (!(Get-XrLoginUser -TimeoutMilliseconds 100) -and $script:Probe.Disposed -and $clock.Elapsed.TotalSeconds -lt 2) 'A blocked first-login probe was not bounded or disposed.'

$script:Opened = 0; $script:Saved = 0; $script:State.Distribution = 'Ubuntu-fixture'
function Get-Tool($Name) { return 'FakeWsl' }
function FakeWsl { $global:LASTEXITCODE = 0; return 'Ubuntu-fixture' }
function Get-WslDistributionVersion { return 2 }
function Save-State { $script:Saved++ }
function Get-XrLoginUser { return $script:ConfiguredUser }
function Open-XrFirstLogin($Wsl) { $script:Opened++; $script:NextStep = 'ubuntu_first_login' }
$script:ConfiguredUser = ''; $script:NextStep = ''
Assert (!(Ensure-Distribution) -and $script:Opened -eq 1 -and $script:NextStep -eq 'ubuntu_first_login') 'Unfinished Ubuntu did not expose the first-login step.'
$script:ConfiguredUser = 'new_account'; $script:NextStep = ''
Assert ((Ensure-Distribution) -and $script:State.LinuxUser -ceq 'new_account' -and $script:Opened -eq 1 -and !$script:NextStep) 'Completed first login did not continue with its real user without an unnecessary restart.'
Write-Host 'OK: WSL update recovery/diagnostics, bounded first-login probes, arbitrary Linux usernames and direct continuation without a restart.'
