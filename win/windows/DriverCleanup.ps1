# Only launcher-owned copies under this user's XR Gate driver cache are removable.
function Get-XrDriverCacheRoot {
    return [IO.Path]::GetFullPath((Join-Path $script:StateDir 'drivers')).TrimEnd([char[]]@('\','/'))
}
function Test-XrManagedDriverPath([string]$Path) {
    if (!$Path) { return $false }
    try {
        $root = Get-XrDriverCacheRoot
        $full = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\','/'))
        if (!$full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        $parts = $full.Substring($root.Length + 1).Split([char[]]@('\','/'))
        if ($parts.Count -notin @(2,3) -or $parts[-1] -ine 'xr_tracking' -or
            $parts[-2] -notmatch '^.+-[0-9]+Hz-(direct|extended_sbs)-[0-9a-fA-F]{12,64}$') { return $false }
        $current = $full
        while ($true) {
            if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $false }
            if ($current -eq $root) { break }
            $current = [IO.Path]::GetDirectoryName($current)
        }
        $marker = Join-Path $full 'xr-gate-installed.json'
        if (Test-Path -LiteralPath $marker -PathType Leaf) {
            $owner = Get-Content -Raw -Encoding UTF8 -LiteralPath $marker | ConvertFrom-Json
            return $owner.schema -eq 1 -and $owner.owner -ceq 'XR Gate Windows launcher'
        }
        # Compatibility with launcher versions which did not write ownership
        # markers, including stale registrations whose files are already gone.
        $manifest = Join-Path $full 'driver.vrdrivermanifest'
        if (Test-Path -LiteralPath $manifest -PathType Leaf) {
            return (Get-Content -Raw -Encoding UTF8 -LiteralPath $manifest | ConvertFrom-Json).name -ceq 'xr_tracking'
        }
        return $true
    } catch { return $false }
}
function Get-XrExternalDriverPaths {
    $registry = [IO.Path]::Combine([string]$env:LOCALAPPDATA, 'openvr/openvrpaths.vrpath')
    if (!(Test-Path -LiteralPath $registry -PathType Leaf)) { return @() }
    $data = Get-Content -Raw -Encoding UTF8 -LiteralPath $registry | ConvertFrom-Json
    if (!$data.PSObject.Properties['external_drivers']) { return @() }
    return @($data.external_drivers | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath([string]$_).TrimEnd([char[]]@('\','/')) })
}
function Get-XrManagedDriverPaths {
    $found = New-Object 'System.Collections.Generic.List[string]'
    foreach ($key in @('DriverPath','RegisteredDriver')) {
        if ($script:State.Contains($key) -and (Test-XrManagedDriverPath ([string]$script:State[$key]))) { $found.Add([IO.Path]::GetFullPath([string]$script:State[$key]).TrimEnd([char[]]@('\','/'))) }
    }
    $root = Get-XrDriverCacheRoot
    if (Test-Path -LiteralPath $root -PathType Container) {
        $pending = New-Object 'System.Collections.Generic.Stack[string]'
        $pending.Push($root)
        while ($pending.Count) {
            $directory = $pending.Pop()
            if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ([IO.Path]::GetFileName($directory) -ieq 'xr_tracking') {
                if (Test-XrManagedDriverPath $directory) { $found.Add($directory) }
                continue
            }
            foreach ($child in @(Get-ChildItem -LiteralPath $directory -Directory -Force)) {
                if (!($child.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $pending.Push($child.FullName) }
            }
        }
    } elseif (!$found.Count) { return @() }
    foreach ($path in @(Get-XrExternalDriverPaths)) {
        if (Test-XrManagedDriverPath $path) { $found.Add([IO.Path]::GetFullPath($path).TrimEnd([char[]]@('\','/'))) }
    }
    return @($found | Sort-Object -Unique)
}
function Assert-XrDriverTree([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Path)
    while ($pending.Count) {
        foreach ($item in @(Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Driver cleanup refuses a link or junction: ' + $item.FullName) }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}
function Remove-XrManagedDrivers {
    $paths = @(Get-XrManagedDriverPaths)
    if (!$paths.Count) { return 0 }
    foreach ($path in $paths) { Assert-XrDriverTree $path }
    Assert-SteamVrStopped
    $registered = @(Get-XrExternalDriverPaths)
    $registeredOwned = @($paths | Where-Object { $_ -in $registered })
    if ($registeredOwned.Count) {
        $reg = Join-Path (Get-SteamVrDirectory) 'bin/win64/vrpathreg.exe'
        foreach ($path in $registeredOwned) {
            Write-Step ('Unregistering XR Gate driver: ' + $path)
            Invoke-Native $reg @('removedriver', $path)
        }
        foreach ($path in $registeredOwned) {
            if (Test-XrDriverRegistration $path) { throw ('XR Gate driver is still registered: ' + $path + '. Close SteamVR and retry Delete.') }
        }
    }
    $root = Get-XrDriverCacheRoot
    foreach ($path in $paths) {
        if (!(Test-XrManagedDriverPath $path)) { throw ('Driver ownership changed during cleanup: ' + $path) }
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        $parent = [IO.Path]::GetDirectoryName($path)
        while ($parent.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $parent -PathType Container)) {
            if (@(Get-ChildItem -LiteralPath $parent -Force).Count) { break }
            Remove-Item -LiteralPath $parent -Force
            if ($parent -eq $root) { break }
            $parent = [IO.Path]::GetDirectoryName($parent)
        }
    }
    Write-Step ('Removed ' + $paths.Count + ' XR Gate driver package(s).')
    return $paths.Count
}
