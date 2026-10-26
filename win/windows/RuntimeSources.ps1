# Resolve the Ubuntu runtime and Mercury models independently of Windows drivers.
function Test-XrCustomRuntimeSource($Release) {
    return ($null -ne $Release.PSObject.Properties['xr_runtime_source'] -and
        [string]$Release.xr_runtime_source -in @('local','url'))
}

function Get-XrCustomSourceDriverRoot([string]$Root) {
    $path = Join-Path $Root 'bundle.json'
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'Local/custom XR Gate archives need the Windows driver packages included with this launcher. Extract the complete Windows release first.'
    }
    $bundle = Get-Content -Raw -Encoding UTF8 -LiteralPath $path | ConvertFrom-Json
    if ($bundle.schema -notin @(1,2)) { throw 'Unsupported Windows driver bundle schema.' }
    return $Root
}

function Get-XrSourceId([string]$Value) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Value)))).Replace('-','').Substring(0,12).ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-XrLocalArchive([string]$Path, [string]$Name) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Local XR Gate archive not found: $Path. Put the runtime and hand-tracking-models-mercury.tar.gz together." }
    $file = Get-Item -LiteralPath $Path
    if ($file -isnot [IO.FileInfo] -or !$file.Name.EndsWith('.tar.gz', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Local XR Gate archives must be .tar.gz files.'
    }
    return [pscustomobject]@{name=$Name; local_path=$file.FullName; size=[long]$file.Length;
        digest=('sha256:' + (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant())}
}

function Get-XrRuntimeSource([string]$Tag) {
    $runtimeName = 'xr-gate-ubuntu-24.04-linux-x64.tar.gz'
    $modelsName = 'hand-tracking-models-mercury.tar.gz'
    $local = (Get-XrSetting 'XR_RUNTIME_LOCAL_PATH' '').Trim()
    $url = (Get-XrSetting 'XR_RUNTIME_URL' '').Trim()
    $kind = 'release'
    $sourcePath = ''
    if ($local) {
        # A selected local path never falls back to the URL or GitHub metadata.
        if (![IO.Path]::IsPathRooted($local)) { $local = Join-Path (Split-Path -Parent $PSScriptRoot) $local }
        if (!(Test-Path -LiteralPath $local)) { throw "Local path XR Gate does not exist: $local" }
        $entry = Get-Item -LiteralPath $local
        $sourcePath = $entry.FullName
        if ($entry -is [IO.DirectoryInfo]) { $runtimePath = Join-Path $entry.FullName $runtimeName; $folder = $entry.FullName }
        elseif ($entry -is [IO.FileInfo]) { $runtimePath = $entry.FullName; $folder = $entry.DirectoryName }
        else { throw 'Local path XR Gate must be a folder or a runtime .tar.gz archive.' }
        $runtime = Get-XrLocalArchive $runtimePath $runtimeName
        $models = Get-XrLocalArchive (Join-Path $folder $modelsName) $modelsName
        if ($runtime.local_path -eq $models.local_path) { throw 'Select the XR Gate runtime archive, not the Mercury models archive.' }
        $kind = 'local'
        $release = [pscustomobject]@{tag_name=('local-' + ([string]$runtime.digest).Substring(7,12)); assets=@(); xr_runtime_source=$kind}
        $runtimeUrl = ''
    } else {
        $uri = $null
        if ($url) {
            if (![Uri]::TryCreate($url, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -notin @('https','http')) {
                throw 'URL XR Gate must be an HTTP(S) runtime .tar.gz URL or download folder.'
            }
            if ($uri.AbsolutePath.EndsWith('/')) { $uri = New-Object Uri($uri, $runtimeName) }
            if (!$uri.AbsolutePath.EndsWith('.tar.gz', [StringComparison]::OrdinalIgnoreCase)) {
                throw 'URL XR Gate must point to a runtime .tar.gz archive or end with / for a download folder.'
            }
            # Official URLs retain the existing GitHub SHA-256 verification and
            # latest-release selection; mirrors are an explicit user choice.
            $official = '^/' + [regex]::Escape($script:Repo) + '/releases/(?:download/(?<tag>[^/]+)|(?<latest>latest)/download)/' + [regex]::Escape($runtimeName) + '$'
            if ($uri.Scheme -eq 'https' -and $uri.Host -eq 'github.com' -and $uri.AbsolutePath -match $official) {
                $Tag = if ($Matches['latest']) { 'latest' } else { [Uri]::UnescapeDataString($Matches['tag']) }
            } else { $kind = 'url' }
        }
        if ($kind -eq 'release') {
            $release = Get-ReleaseInfo $Tag
            $runtime = Select-ReleaseAsset $release $runtimeName
            $models = Select-ReleaseAsset $release $modelsName
            $runtimeUrl = [string]$runtime.browser_download_url
        } else {
            $modelsUri = New-Object UriBuilder($uri)
            $modelsUri.Path = $uri.AbsolutePath.Substring(0, $uri.AbsolutePath.LastIndexOf('/') + 1) + $modelsName
            $modelsUri.Query = ''; $modelsUri.Fragment = ''
            $runtimeUrl = $uri.AbsoluteUri
            $runtime = [pscustomobject]@{name=$runtimeName; browser_download_url=$runtimeUrl; custom_url=$true; digest=''; size=0}
            $models = [pscustomobject]@{name=$modelsName; browser_download_url=$modelsUri.Uri.AbsoluteUri; custom_url=$true; digest=''; size=0}
            $release = [pscustomobject]@{tag_name=('url-' + (Get-XrSourceId $runtimeUrl)); assets=@(); xr_runtime_source=$kind}
        }
    }
    return [pscustomobject]@{Kind=$kind; Release=$release; Runtime=$runtime; Models=$models;
        Cache=(Join-Path $script:StateDir ('cache/' + $release.tag_name)); RuntimeUrl=$runtimeUrl; LocalPath=$sourcePath}
}

function Save-XrRuntimeArchive($Asset, [string]$Directory) {
    if ($Asset.PSObject.Properties['local_path']) { return [string]$Asset.local_path }
    if (!$Asset.PSObject.Properties['custom_url']) { return Save-ReleaseAsset $Asset $Directory }
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $target = Join-Path $Directory $Asset.name
    $partial = $target + '.' + [Guid]::NewGuid().ToString('N') + '.download'
    $previous = $ProgressPreference
    try {
        $ProgressPreference = 'SilentlyContinue'
        Write-Step ('Downloading ' + $Asset.name + ' from ' + $Asset.browser_download_url)
        # Re-fetch custom URLs on Update: a mirror may replace bytes at one URL.
        Invoke-WebRequest -UseBasicParsing -Uri $Asset.browser_download_url -OutFile $partial -Headers @{'User-Agent'='XR-Gate-WSL-Setup'} -TimeoutSec 600
        $Asset.size = [long](Get-Item -LiteralPath $partial).Length
        if (!$Asset.size) { throw ('Downloaded archive is empty: ' + $Asset.name) }
        $Asset.digest = 'sha256:' + (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
        Move-Item -LiteralPath $partial -Destination $target -Force
        return $target
    } finally {
        $ProgressPreference = $previous
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}
