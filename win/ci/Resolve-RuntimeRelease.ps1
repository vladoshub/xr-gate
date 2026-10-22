#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repository = $env:GITHUB_REPOSITORY,
    [string]$BuildUbuntu = $env:BUILD_UBUNTU,
    [string]$PublishRelease = $env:PUBLISH_RELEASE,
    [string]$ReleaseTag = $env:RELEASE_TAG,
    [string]$Token = $env:GH_TOKEN,
    [string]$OutputFile = $env:GITHUB_ENV
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runtimeTag = 'latest'
if ($BuildUbuntu -eq 'true' -and $PublishRelease -eq 'true') {
    if ($ReleaseTag -notmatch '^[A-Za-z0-9._-]+$') { throw 'Invalid release tag.' }
    $runtimeTag = $ReleaseTag
} elseif ($BuildUbuntu -ne 'true') {
    # An independent Windows build can reuse an existing published runtime.
    # Lookup is best-effort: compiling Windows does not require Linux assets.
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid repository.' }
    $headers = @{Accept='application/vnd.github+json'; 'User-Agent'='XR-Gate-Windows-Build'}
    if ($Token) { $headers.Authorization = "Bearer $Token" }
    try {
        $found = $null
        for ($page = 1; $page -le 10 -and !$found; $page++) {
            # Do not wrap the REST call in @(...): a JSON array is emitted as
            # one pipeline object, which would create an extra nested array.
            $response = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases?per_page=100&page=$page" -TimeoutSec 30
            $releases = @($response)
            if (!$releases.Count) { break }
            foreach ($release in $releases) {
                $names = @($release.assets | ForEach-Object { $_.name })
                if (!$release.draft -and $release.tag_name -match '^[A-Za-z0-9._-]+$' -and
                    'xr-gate-ubuntu-24.04-linux-x64.tar.gz' -in $names -and
                    'hand-tracking-models-mercury.tar.gz' -in $names) { $found = $release; break }
            }
        }
        if ($found) {
            $runtimeTag = [string]$found.tag_name
        } else {
            Write-Warning 'No published Ubuntu runtime/models release was found. Windows will still build with XR_RELEASE_TAG=latest. Publish an Ubuntu release or select an existing runtime tag in launcher Settings before installing.'
        }
    } catch {
        Write-Warning 'GitHub runtime release lookup failed. Windows will still build with XR_RELEASE_TAG=latest. Select an available Ubuntu runtime release in launcher Settings before installing.'
    }
}
if ($OutputFile) {
    "WINDOWS_BUNDLE_RELEASE=$runtimeTag" | Out-File -FilePath $OutputFile -Encoding utf8 -Append
}
Write-Host "Windows installer runtime release: $runtimeTag"
