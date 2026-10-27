# Literal env files shared by the Windows launcher and its Linux helper.
function Read-XrEnv([string]$Path, [switch]$Required) {
    $values = @{}
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($Required) { throw "Environment file not found: $Path" }
        return $values
    }
    $number = 0
    foreach ($raw in [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)) {
        $number++; $line = $raw.Trim()
        if (!$line -or $line.StartsWith('#')) { continue }
        if ($line -notmatch '^(XR_[A-Za-z0-9_]+|SBS_RESOLUTION)=(.*)$') { throw "${Path}:${number}: expected XR_KEY=value or SBS_RESOLUTION=WIDTHxHEIGHT" }
        $key = $Matches[1]; $value = $Matches[2].Trim()
        if ($value.StartsWith('"') -or $value.StartsWith("'")) {
            if ($value.Length -lt 2 -or $value[$value.Length - 1] -ne $value[0]) { throw "${Path}:${number}: unclosed quote" }
            $value = $value.Substring(1, $value.Length - 2)
        }
        if ($value.Contains('$') -or $value.Contains('`')) { throw "${Path}:${number}: use literal values" }
        if ($key -eq 'SBS_RESOLUTION' -and $value.Trim() -and $value.Trim() -notmatch '^[1-9][0-9]{0,4}[xX][1-9][0-9]{0,4}$') {
            throw "${Path}:${number}: SBS_RESOLUTION must be WIDTHxHEIGHT, for example 3840x1080, or empty."
        }
        $values[$key] = $value
    }
    return $values
}
function Initialize-XrSettings([string]$Root) {
    $defaults = Read-XrEnv (Join-Path $Root '.env.example') -Required
    $overrides = Read-XrEnv (Join-Path $Root '.env')
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        if ([string]$entry.Key -cmatch '^XR_') { $overrides[[string]$entry.Key] = [string]$entry.Value }
    }
    $hardware = [string]$defaults['XR_HARDWARE_ENV']
    if ($overrides.ContainsKey('XR_HARDWARE_ENV')) { $hardware = [string]$overrides['XR_HARDWARE_ENV'] }
    if ($hardware) {
        if (![IO.Path]::IsPathRooted($hardware)) { $hardware = Join-Path $Root $hardware }
        $preset = Read-XrEnv $hardware -Required
        foreach ($key in $preset.Keys) { $defaults[$key] = $preset[$key] }
    }
    $script:Settings = @{}
    foreach ($key in $defaults.Keys) { $script:Settings[$key] = $defaults[$key] }
    foreach ($key in $overrides.Keys) { $script:Settings[$key] = $overrides[$key] }
    foreach ($key in @('XR_REQUIRE_CAMERA','XR_REQUIRE_HIDRAW','XR_CAMERA_AUTO_SELECT','XR_WSL_FIRMWARE','XR_AUDIO_ENABLED','XR_GLOBAL_HOTKEYS')) {
        if ($script:Settings.ContainsKey($key) -and $script:Settings[$key] -notin @('0','1')) {
            throw "$key must be 0 or 1."
        }
    }
    foreach ($key in @('XR_BLUETOOTH','XR_GEARVR')) {
        if ($script:Settings.ContainsKey($key) -and $script:Settings[$key] -notin @('','0','1')) {
            throw "$key must be empty, 0 or 1."
        }
    }
    if ($script:Settings.ContainsKey('XR_CONTROLLER_MODE') -and $script:Settings['XR_CONTROLLER_MODE'] -notin @('','gearvr','gestures','no')) {
        throw 'XR_CONTROLLER_MODE must be gearvr, gestures or no, or empty for legacy settings.'
    }
    $script:SettingOverrides = $overrides
}
function Get-XrSetting([string]$Key, [string]$Fallback = '') {
    if ($script:Settings.ContainsKey($Key)) { return [string]$script:Settings[$Key] }
    return $Fallback
}
function Initialize-XrStopSettings([string]$Root, [string]$StateFile = '') {
    # Cleanup uses the configuration which attached the devices, even if .env
    # was later damaged or changed to a different Linux root/distribution.
    $path = $StateFile
    if (!$path) { $path = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'XR-Gate-WSL\state.json' }
    if (Test-Path -LiteralPath $path) {
        $saved = Get-Content -Raw -Encoding UTF8 -LiteralPath $path | ConvertFrom-Json
        if ($saved.PSObject.Properties['CleanupSettings'] -and $saved.CleanupSettings) {
            $script:Settings = @{}
            foreach ($property in $saved.CleanupSettings.PSObject.Properties) {
                if ($property.Name -notmatch '^(XR_[A-Za-z0-9_]+|SBS_RESOLUTION)$' -or $property.Value -isnot [string]) { throw 'Invalid cleanup settings in state.json.' }
                $script:Settings[$property.Name] = [string]$property.Value
            }
            $script:SettingOverrides = @{}
            return
        }
    }
    Initialize-XrSettings $Root # Compatibility with state saved by older versions.
}
function Get-XrList([string]$Key) {
    return @((Get-XrSetting $Key) -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
function Get-XrInteger([string]$Key, [int]$Minimum = 1, [int]$Maximum = 65535) {
    $value = 0
    if (![int]::TryParse((Get-XrSetting $Key), [ref]$value) -or $value -lt $Minimum -or $value -gt $Maximum) {
        throw "$Key must be an integer from $Minimum to $Maximum in the env file."
    }
    return $value
}
