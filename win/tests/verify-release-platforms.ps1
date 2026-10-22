#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:WindowsRelease = [pscustomobject]@{tag_name='windows-only';draft=$false;prerelease=$false;assets=@([pscustomobject]@{name='xr-gate-windows-wsl-usbip.zip'})}
$script:UbuntuRelease = [pscustomobject]@{tag_name='ubuntu-runtime';draft=$false;prerelease=$false;assets=@(
    [pscustomobject]@{name='xr-gate-ubuntu-24.04-linux-x64.tar.gz'},
    [pscustomobject]@{name='hand-tracking-models-mercury.tar.gz'})}
$script:Requests = New-Object 'System.Collections.Generic.List[string]'
function Invoke-RestMethod($Uri, $Headers, $TimeoutSec) {
    $script:Requests.Add($Uri)
    if ($Uri.EndsWith('/latest') -or $Uri.EndsWith('/tags/windows-only')) { return $script:WindowsRelease }
    return @($script:WindowsRelease,$script:UbuntuRelease)
}
Assert ((Get-ReleaseInfo 'latest').tag_name -eq 'ubuntu-runtime') 'A Windows-only latest release hid the usable Ubuntu runtime.'
$before = $script:Requests.Count
Assert ((Get-ReleaseInfo 'windows-only').tag_name -eq 'windows-only') 'An explicit release tag was silently redirected.'
Assert ($script:Requests.Count -eq $before + 1) 'Explicit tags unexpectedly scanned other releases.'
Write-Host 'OK: independent platform releases and latest-runtime discovery.'
