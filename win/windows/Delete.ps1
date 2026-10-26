# Delete only the installation named in both the saved state and the confirmation.
function Get-XrRegisteredDistributions {
    $names = @(& (Get-Tool 'wsl.exe') --list --quiet)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list WSL distributions before deletion.' }
    return @($names | ForEach-Object { ($_ -replace '\x00','').Trim() } | Where-Object { $_ })
}
function Remove-XrDistribution([string]$ConfirmedDistribution) {
    if (!$ConfirmedDistribution -or $ConfirmedDistribution -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
        $ConfirmedDistribution -like 'docker-desktop*') { throw 'A valid confirmed XR Gate Ubuntu distribution is required.' }
    $saved = Get-Content -Raw -Encoding UTF8 -LiteralPath $script:StateFile | ConvertFrom-Json
    if (!$saved.PSObject.Properties['Distribution'] -or [string]$saved.Distribution -cne $ConfirmedDistribution -or
        [string]$script:State.Distribution -cne $ConfirmedDistribution) {
        throw 'The saved Ubuntu distribution changed. Click Delete again and review its name before confirming.'
    }
    if (!$saved.LinuxUser -and !$saved.InstallPending) { throw 'No saved XR Gate installation is available to delete.' }
    $exists = $ConfirmedDistribution -in @(Get-XrRegisteredDistributions)
    Stop-XrSteamVr
    Request-RuntimeWindowStop
    if ($exists -and $script:State.LinuxUser) { Invoke-Linux -Operation stop -Request (New-LinuxRequest) }
    Stop-RuntimeWindow
    Disconnect-SelectedUsb
    if ($exists -and $script:State.LinuxUser) {
        Invoke-Linux -Operation firmware_cleanup -Request (New-LinuxRequest) -AsRoot
    }
    Write-Step ("Deleting Ubuntu distribution: " + $ConfirmedDistribution)
    if ($exists) { Invoke-Native (Get-Tool 'wsl.exe') @('--unregister', $ConfirmedDistribution) }
    $script:State.LinuxUser = ''; $script:State.ReleaseTag = ''
    $script:State.InstallPending = $false; $script:State.InstallNextStep = ''
    $script:State.AttachedUsbInstanceIds = @()
    $script:State.CleanupSettings = $null
    Save-State
    $window = Get-RuntimeWindowPath
    if (Test-Path -LiteralPath $window) { Remove-Item -LiteralPath $window }
    $script:ActionData = @{distribution=$ConfirmedDistribution; deleted=$true; installed=$false; running=$false}
}
