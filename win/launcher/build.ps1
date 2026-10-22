#Requires -Version 5.1
[CmdletBinding()]
param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Join-Path $PSScriptRoot 'XrGate.Launcher.csproj'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'out/windows-launcher' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
function Invoke-Dotnet([string[]]$Arguments) {
    & dotnet @Arguments
    if ($LASTEXITCODE -ne 0) { throw ('dotnet failed: ' + ($Arguments -join ' ')) }
}
Invoke-Dotnet @('run','--project',(Join-Path $PSScriptRoot 'tests/XrGate.Launcher.Tests.csproj'),
    '-c','Release','--',(Split-Path -Parent $PSScriptRoot))
Invoke-Dotnet @('publish',$project,'-c','Release','-r','win-x64','--self-contained','true',
    '-p:PublishSingleFile=true','-p:IncludeNativeLibrariesForSelfExtract=true','-o',$OutputDirectory)
$exe = Join-Path $OutputDirectory 'XR-Gate.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Publish did not produce XR-Gate.exe.' }
if (@(Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File -Filter '*.dll').Count) {
    throw 'Publish left DLLs outside the executable; the self-contained single-file package is incomplete.'
}
# Redistribute the license notices supplied with the bundled .NET runtime packs.
$assets = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'obj/project.assets.json') | ConvertFrom-Json
foreach ($framework in $assets.project.frameworks.PSObject.Properties) {
    foreach ($dependency in $framework.Value.downloadDependencies) {
        if ($dependency.name -notmatch '^Microsoft\.(NETCore|WindowsDesktop)\.App\.Runtime\.win-x64$') { continue }
        $version = [regex]::Match([string]$dependency.version, '\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?').Value
        $pack = $dependency.name.ToLowerInvariant() + '/' + $version
        foreach ($packages in $assets.packageFolders.PSObject.Properties.Name) {
            $directory = Join-Path $packages $pack
            if (!(Test-Path -LiteralPath $directory)) { continue }
            $notices = Join-Path $OutputDirectory ('licenses/' + $pack)
            New-Item -ItemType Directory -Force $notices | Out-Null
            Get-ChildItem -LiteralPath $directory -File | Where-Object { $_.Name -match '^(LICENSE|THIRD.PARTY.NOTICES)' } |
                Copy-Item -Destination $notices
        }
    }
}
# Exercise the published apphost, WPF/BAML resources, settings and controls.
# No WSL, hardware, SteamVR, elevation or network access is used by smoke mode.
if ($env:OS -eq 'Windows_NT') {
    $bundle = Split-Path -Parent $PSScriptRoot
    $stderr = Join-Path $OutputDirectory 'smoke.stderr.log'
    $stdout = Join-Path $OutputDirectory 'smoke.stdout.log'
    $process = Start-Process -FilePath $exe -PassThru -RedirectStandardError $stderr -RedirectStandardOutput $stdout `
        -ArgumentList @('--smoke-test','--bundle-root',('"' + $bundle + '"'))
    if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'Published WPF launcher smoke test timed out.' }
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue | Write-Host
        throw ('Published WPF launcher smoke test failed: ' + $process.ExitCode)
    }
}
Write-Host ('OK: self-contained Windows x64 WPF launcher: ' + $exe)
