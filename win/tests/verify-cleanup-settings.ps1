#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'windows/Environment.ps1')
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-cleanup-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $state = Join-Path $temporary 'state.json'
    [IO.File]::WriteAllText((Join-Path $temporary '.env'), 'broken settings')
    @{CleanupSettings=@{XR_DISTRIBUTION='Fixture-Ubuntu';XR_LINUX_ROOT='~/owned-runtime';XR_TCP_PORT='45899'}} |
        ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $state
    Initialize-XrStopSettings $temporary $state
    Assert ((Get-XrSetting 'XR_LINUX_ROOT') -eq '~/owned-runtime') 'Stop used a changed/broken .env instead of the attached session root.'
    Assert ((Get-XrSetting 'XR_DISTRIBUTION') -eq 'Fixture-Ubuntu') 'Stop lost the session distribution.'
    @{CleanupSettings=@{XR_LINUX_ROOT=@('invalid','nonliteral')}} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath $state
    $rejected = $false
    try { Initialize-XrStopSettings $temporary $state } catch { $rejected = $_.Exception.Message -like '*Invalid cleanup settings*' }
    Assert $rejected 'Invalid cleanup state was executed.'
    Write-Host 'OK: cleanup uses the saved session configuration even with a damaged .env.'
} finally { Remove-Item -Recurse -Force -LiteralPath $temporary }
