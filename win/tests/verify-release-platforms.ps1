#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:WindowsRelease = [pscustomobject]@{tag_name='windows-only';draft=$false;prerelease=$false;assets=@([pscustomobject]@{name='xr-gate-windows-wsl-usbip.zip'})}
$script:UbuntuRelease = [pscustomobject]@{tag_name='ubuntu-runtime';draft=$false;prerelease=$false;assets=@(
    [pscustomobject]@{name='xr-gate-ubuntu-24.04-linux-x64.tar.gz'},
    [pscustomobject]@{name='hand-tracking-models-mercury.tar.gz'})}
$script:Prerelease = [pscustomobject]@{tag_name='ubuntu-preview';draft=$false;prerelease=$true;assets=$script:UbuntuRelease.assets}
$script:Draft = [pscustomobject]@{tag_name='unpublished';draft=$true;prerelease=$false;assets=$script:UbuntuRelease.assets}
$script:Requests = New-Object 'System.Collections.Generic.List[string]'
$script:Pages = @{'1'=@($script:Draft,$script:WindowsRelease,$script:Prerelease,$script:UbuntuRelease); '2'=@()}
$script:LookupFails = $false
function Invoke-RestMethod($Uri, $Headers, $TimeoutSec) {
    $Requests.Add($Uri)
    if ($LookupFails) { throw 'Simulated GitHub API outage.' }
    if ($Uri.EndsWith('/latest') -or $Uri.EndsWith('/tags/windows-only')) { return $WindowsRelease }
    $page = [regex]::Match($Uri, '[?&]page=(\d+)').Groups[1].Value
    # Real Invoke-RestMethod returns JSON arrays as ONE pipeline object. The
    # unary comma preserves that contract; emitting releases one by one hides
    # the nested-array failure in both the installer and Windows-only build.
    return ,($Pages[$page])
}
Assert ((Get-ReleaseInfo 'latest').tag_name -eq 'ubuntu-runtime') 'A Windows-only latest release hid the usable Ubuntu runtime.'
$before = $script:Requests.Count
Assert ((Get-ReleaseInfo 'windows-only').tag_name -eq 'windows-only') 'An explicit release tag was silently redirected.'
Assert ($script:Requests.Count -eq $before + 1) 'Explicit tags unexpectedly scanned other releases.'

# Empty/Windows-only first pages must not prevent finding the stable runtime
# on another page. Single-item and empty JSON arrays retain the same shape.
$script:Pages = @{'1'=@($script:WindowsRelease); '2'=@($script:UbuntuRelease); '3'=@()}
Assert ((Get-ReleaseInfo 'latest').tag_name -eq 'ubuntu-runtime') 'Runtime lookup did not follow paginated JSON arrays.'
$script:Pages = @{'1'=@(); '2'=@()}
$notFound = $false
try { Get-ReleaseInfo 'latest' | Out-Null } catch { $notFound = $_.Exception.Message -like '*No published Ubuntu*' }
Assert $notFound 'An installer with no usable runtime did not report the missing dependency.'

function Resolve-BuildTag([string]$BuildUbuntu='false', [string]$Publish='false', [string]$Tag='new-release') {
    $output = [IO.Path]::GetTempFileName()
    try {
        & (Join-Path (Split-Path -Parent $PSScriptRoot) 'ci/Resolve-RuntimeRelease.ps1') `
            -Repository 'example/xr-gate' -BuildUbuntu $BuildUbuntu -PublishRelease $Publish `
            -ReleaseTag $Tag -Token '' -OutputFile $output -WarningAction SilentlyContinue -WarningVariable buildWarnings
        $line = [IO.File]::ReadAllText($output).Trim()
        Assert ($line -match '^WINDOWS_BUNDLE_RELEASE=[A-Za-z0-9._-]+$') 'CI did not write the runtime tag to GITHUB_ENV.'
        return [pscustomobject]@{Tag=$line.Substring('WINDOWS_BUNDLE_RELEASE='.Length); Warnings=@($buildWarnings)}
    } finally { Remove-Item -LiteralPath $output -Force }
}
$script:Pages = @{'1'=@($script:Draft,$script:WindowsRelease,$script:Prerelease,$script:UbuntuRelease); '2'=@()}
Assert ((Resolve-BuildTag).Tag -eq 'ubuntu-preview') 'Windows-only CI did not pin the first published usable runtime.'
$script:Pages = @{'1'=@($script:WindowsRelease); '2'=@($script:UbuntuRelease); '3'=@()}
Assert ((Resolve-BuildTag).Tag -eq 'ubuntu-runtime') 'Windows-only CI did not follow paginated JSON arrays.'
$script:Pages = @{'1'=@(); '2'=@()}
$result = Resolve-BuildTag
Assert ($result.Tag -eq 'latest' -and $result.Warnings.Count -gt 0) 'Missing Ubuntu releases blocked Windows compilation or lacked a warning.'
$script:LookupFails = $true
$result = Resolve-BuildTag
Assert ($result.Tag -eq 'latest' -and $result.Warnings.Count -gt 0) 'GitHub lookup failure blocked independent Windows compilation.'
$before = $script:Requests.Count
Assert ((Resolve-BuildTag -BuildUbuntu 'true' -Publish 'true' -Tag 'this-release').Tag -eq 'this-release') 'Combined publishing did not select its new runtime tag.'
Assert ((Resolve-BuildTag -BuildUbuntu 'true').Tag -eq 'latest') 'An unpublished combined build did not retain latest.'
Assert ($script:Requests.Count -eq $before) 'Combined builds unexpectedly queried existing releases.'
Write-Host 'OK: REST array contract, pagination, independent Windows builds and latest-runtime discovery.'
