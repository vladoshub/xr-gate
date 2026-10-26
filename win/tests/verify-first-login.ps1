#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
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
Write-Host 'OK: bounded first-login probes, arbitrary Linux usernames, incomplete prompts and direct continuation without a restart.'
