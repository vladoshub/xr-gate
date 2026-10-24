function Restart-XrTracking {
    # Restart only XR Gate's owned runtime. Keep SteamVR, its driver/display
    # configuration and USB attachments intact throughout the restart.
    Write-Step 'Restarting XR Gate tracking. SteamVR and USB stay connected.'
    Request-RuntimeWindowStop
    Invoke-Linux -Operation stop -Request (New-LinuxRequest)
    Stop-RuntimeWindow
    Invoke-Linux -Operation prepare -Request (New-LinuxRequest) -AsRoot
    Start-RuntimeWindow
    $window = Read-RuntimeWindow
    Write-Step 'Waiting for valid updating HMD poses after tracking restart...'
    Wait-XrStream $TimeoutSeconds -RuntimeWindow $window
    Write-Step 'Tracking restarted. SteamVR was left running.'
}
