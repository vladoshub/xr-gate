#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Value, [string]$Message) { if (!$Value) { throw $Message } }
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('xr-source-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$script:StateDir = Join-Path $fixture 'state'
$script:InputSettings = @{}
$script:Requests = New-Object 'System.Collections.Generic.List[string]'
$script:Downloads = New-Object 'System.Collections.Generic.List[string]'
$script:Payload = 'runtime-v1'
$script:DownloadFails = $false
function Get-XrSetting([string]$Key, [string]$Fallback = '') {
    if ($script:InputSettings.ContainsKey($Key)) { return [string]$script:InputSettings[$Key] }
    return $Fallback
}
function Digest([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-ReleaseInfo([string]$Tag) {
    $script:Requests.Add($Tag)
    $resolved = if ($Tag -eq 'latest') { 'v-test-current' } else { $Tag }
    return [pscustomobject]@{tag_name=$resolved; assets=@(
        [pscustomobject]@{name='xr-gate-ubuntu-24.04-linux-x64.tar.gz'; digest=('sha256:' + (Digest 'runtime-v1')); size=10; browser_download_url="https://github.com/vladoshub/xr-gate/releases/download/$resolved/xr-gate-ubuntu-24.04-linux-x64.tar.gz"},
        [pscustomobject]@{name='hand-tracking-models-mercury.tar.gz'; digest=('sha256:' + (Digest 'models-v1')); size=9; browser_download_url="https://github.com/vladoshub/xr-gate/releases/download/$resolved/hand-tracking-models-mercury.tar.gz"}
    )}
}
function Invoke-WebRequest([switch]$UseBasicParsing, $Uri, $OutFile, $Headers, $TimeoutSec) {
    $script:Downloads.Add([string]$Uri)
    $body = if ([string]$Uri -like '*hand-tracking-models-mercury.tar.gz*') { 'models-v1' } else { $script:Payload }
    [IO.File]::WriteAllBytes($OutFile, [Text.Encoding]::UTF8.GetBytes($body))
    if ($script:DownloadFails) { throw 'Simulated download failure.' }
}
try {
    $source = Get-XrRuntimeSource 'latest'
    Assert ($source.Kind -eq 'release' -and $source.Release.tag_name -eq 'v-test-current') 'Default source did not resolve the selected GitHub release.'
    Assert ($source.RuntimeUrl -like '*/download/v-test-current/xr-gate-ubuntu-24.04-linux-x64.tar.gz') 'The displayed URL is not the selected archive URL.'
    $download = Save-XrRuntimeArchive $source.Runtime $source.Cache
    Assert ((Get-FileHash -LiteralPath $download).Hash.ToLowerInvariant() -eq (Digest 'runtime-v1')) 'Official archive bytes changed.'
    $count = $script:Downloads.Count
    Save-XrRuntimeArchive $source.Runtime $source.Cache | Out-Null
    Assert ($script:Downloads.Count -eq $count) 'Verified official cache was downloaded again.'

    $script:InputSettings = @{XR_RUNTIME_URL='https://github.com/vladoshub/xr-gate/releases/download/v-pinned/xr-gate-ubuntu-24.04-linux-x64.tar.gz'}
    $source = Get-XrRuntimeSource 'latest'
    Assert ($source.Release.tag_name -eq 'v-pinned' -and $source.Kind -eq 'release') 'An official archive URL lost its tag or SHA verification.'

    $local = Join-Path $fixture "O'Brien локальные архивы"
    New-Item -ItemType Directory -Path $local | Out-Null
    $runtime = Join-Path $local 'xr-gate-ubuntu-24.04-linux-x64.tar.gz'
    $models = Join-Path $local 'hand-tracking-models-mercury.tar.gz'
    [IO.File]::WriteAllText($runtime, 'local-runtime')
    [IO.File]::WriteAllText($models, 'local-models')
    $script:InputSettings = @{XR_RUNTIME_LOCAL_PATH=$local; XR_RUNTIME_URL='not even a URL'}
    $requestsBefore = $script:Requests.Count; $downloadsBefore = $script:Downloads.Count
    $source = Get-XrRuntimeSource 'latest'
    Assert ($source.Kind -eq 'local' -and $source.RuntimeUrl -eq '' -and (Test-XrCustomRuntimeSource $source.Release)) 'Local files did not override the URL.'
    Assert ((Save-XrRuntimeArchive $source.Runtime $source.Cache) -eq $runtime) 'Local runtime was copied or downloaded instead of used directly.'
    Assert ($source.Runtime.digest -eq ('sha256:' + (Digest 'local-runtime'))) 'Local archive hash was not supplied to the Linux installer.'
    $script:InputSettings.XR_RUNTIME_LOCAL_PATH = $runtime
    Assert ((Get-XrRuntimeSource 'latest').Models.local_path -eq $models) 'Selecting a runtime file lost its sibling models archive.'
    Remove-Item -LiteralPath $models
    $failed = $false
    try { Get-XrRuntimeSource 'latest' | Out-Null } catch { $failed = $_.Exception.Message -like '*Local XR Gate archive not found*' }
    Assert $failed 'Missing local models silently fell back to a download.'
    $script:InputSettings.XR_RUNTIME_LOCAL_PATH = Join-Path $fixture 'missing'
    $failed = $false
    try { Get-XrRuntimeSource 'latest' | Out-Null } catch { $failed = $_.Exception.Message -like '*Local path XR Gate does not exist*' }
    Assert $failed 'An invalid local path was ignored.'
    Assert ($script:Requests.Count -eq $requestsBefore -and $script:Downloads.Count -eq $downloadsBefore) 'Local source accessed the URL or GitHub API.'

    $script:InputSettings = @{XR_RUNTIME_URL='https://mirror.example/My%20release/runtime.tar.gz?download=1'}
    $source = Get-XrRuntimeSource 'latest'
    Assert ($source.Kind -eq 'url' -and $source.RuntimeUrl -like '*runtime.tar.gz?download=1') 'A custom runtime URL was not preserved.'
    Assert ($source.Models.browser_download_url -eq 'https://mirror.example/My%20release/hand-tracking-models-mercury.tar.gz') 'Custom models URL was not resolved beside the runtime.'
    $first = Save-XrRuntimeArchive $source.Runtime $source.Cache
    $hash = $source.Runtime.digest
    $script:Payload = 'runtime-v2'
    $second = Save-XrRuntimeArchive $source.Runtime $source.Cache
    Assert ($first -eq $second -and $source.Runtime.digest -ne $hash -and [IO.File]::ReadAllText($second) -eq 'runtime-v2') 'Update reused stale custom-URL bytes.'
    $script:DownloadFails = $true
    $failed = $false
    try { Save-XrRuntimeArchive $source.Runtime $source.Cache | Out-Null } catch { $failed = $true }
    Assert ($failed -and [IO.File]::ReadAllText($second) -eq 'runtime-v2') 'A failed mirror download overwrote the working archive.'
    Assert (@(Get-ChildItem -LiteralPath $source.Cache -Filter '*.download').Count -eq 0) 'A failed download left staging files.'
    $script:DownloadFails = $false
    $script:InputSettings = @{XR_RUNTIME_URL='https://mirror.example/downloads/'}
    Assert ((Get-XrRuntimeSource 'latest').RuntimeUrl -eq 'https://mirror.example/downloads/xr-gate-ubuntu-24.04-linux-x64.tar.gz') 'A download folder URL was not accepted.'
    Assert ($script:Requests.Count -eq $requestsBefore) 'A mirror unexpectedly queried GitHub.'

    foreach ($url in @('file:///tmp/runtime.tar.gz', 'not a URL', 'https://mirror.example/page')) {
        $script:InputSettings = @{XR_RUNTIME_URL=$url}
        $failed = $false
        try { Get-XrRuntimeSource 'latest' | Out-Null } catch { $failed = $true }
        Assert $failed 'An unsupported runtime URL was accepted.'
    }

    $bundle = Join-Path $fixture 'windows-bundle'
    New-Item -ItemType Directory -Path $bundle | Out-Null
    [IO.File]::WriteAllText((Join-Path $bundle 'bundle.json'), '{"schema":2,"release_tag":"different-driver-release"}')
    Assert ((Get-XrCustomSourceDriverRoot $bundle) -eq $bundle) 'Custom runtime required the Windows drivers to share its synthetic tag.'
    $script:FixtureBundle = $bundle
    function Get-XrCustomSourceDriverRoot([string]$Root) { return $script:FixtureBundle }
    function Find-BundledDriver([string]$Root) { return Join-Path $Root 'driver' }
    $release = [pscustomobject]@{tag_name='local-123456789abc'; assets=@(); xr_runtime_source='local'}
    Assert ((Get-AutomaticDriverBundle $release $fixture) -eq $bundle) 'Automatic refresh-rate selection did not use the local Windows bundle.'
    Assert ((Get-DriverPackage $release $fixture) -eq (Join-Path $bundle 'driver')) 'Manual refresh-rate installation did not use the local Windows bundle.'
    Assert ($script:Requests.Count -eq $requestsBefore) 'Local/custom driver selection downloaded release metadata.'
    Write-Host 'OK: runtime URLs, verified official cache, local precedence, mirror updates, failure cleanup and bundled driver selection.'
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force }
