using System.Collections;
using System.ComponentModel;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace XrGate.Launcher.Core;

public sealed class SettingsReadException(string path, string message, Exception? inner = null) : FormatException(message, inner)
{
    public string FilePath { get; } = Path.GetFullPath(path);
}

public sealed class LauncherSettings : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    private Dictionary<string, string> baseline = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> explicitKeys = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> profileChoices = new(StringComparer.OrdinalIgnoreCase);
    private string loadedHardware = "";
    private string? profileLabelOverride;
    private string? deviceNameOverride;
    public string HardwareEnv { get; set; } = "";
    public string DeviceName { get; private set; } = "XR device";
    public string ControllerProfileLabel { get; private set; } = "Configured BLE / IMU controller profile";
    public string Distribution { get; set; } = "Ubuntu-24.04";
    public string ReleaseTag { get; set; } = "latest";
    public string UsbBusIds { get; set; } = "";
    public string BluetoothBusId { get; set; } = "";
    public string BluetoothUsbId { get; set; } = "";
    public bool BluetoothEnabled { get; set; }
    public bool GearVr { get; set; }
    public bool AutoDriverFrequency { get; set; } = true;
    public string FrequencyHz { get; set; } = "60";
    public string DisplayMode { get; set; } = "extended_sbs";
    public string DisplayName { get; set; } = "";
    public string SteamVrPath { get; set; } = "";
    public string ControllerConfig { get; set; } = "";
    public string SavedBluetoothInstance { get; private set; } = "";
    public int StartTimeoutSeconds { get; private set; } = 180;

    public static Dictionary<string, string> ReadEnv(string path, bool required = false)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(path))
        {
            if (required) throw new SettingsReadException(path, "Environment file not found: " + path);
            return result;
        }
        int number = 0;
        string[] lines;
        try { lines = File.ReadAllLines(path, Encoding.UTF8); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { throw new SettingsReadException(path, "Could not read " + path + ": " + error.Message, error); }
        foreach (string raw in lines)
        {
            number++;
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            Match match = Regex.Match(line, @"^(XR_[A-Za-z0-9_]+)=(.*)$");
            if (!match.Success) throw new SettingsReadException(path, $"{path}:{number}: expected XR_KEY=value.");
            string value = match.Groups[2].Value.Trim();
            if (value.StartsWith('"') || value.StartsWith('\''))
            {
                if (value.Length < 2 || value[^1] != value[0]) throw new SettingsReadException(path, $"{path}:{number}: unclosed quote.");
                value = value[1..^1];
            }
            try { ValidateLiteral(value); }
            catch (FormatException error) { throw new SettingsReadException(path, $"{path}:{number}: " + error.Message, error); }
            result[match.Groups[1].Value] = value;
        }
        return result;
    }

    public static LauncherSettings Load(string root, string? stateFile = null, string? hardwareOverride = null, bool defaultsOnly = false)
    {
        var values = ReadEnv(Path.Combine(root, ".env.example"), true);
        var overrides = defaultsOnly ? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) : ReadEnv(Path.Combine(root, ".env"));
        if (!defaultsOnly)
            foreach (DictionaryEntry entry in Environment.GetEnvironmentVariables())
                if (entry.Key is string key && key.StartsWith("XR_", StringComparison.Ordinal)) overrides[key] = entry.Value?.ToString() ?? "";
        string hardware = hardwareOverride ?? overrides.GetValueOrDefault("XR_HARDWARE_ENV", values.GetValueOrDefault("XR_HARDWARE_ENV", ""));
        if (hardware.Length > 0)
        {
            string path;
            try { path = Path.GetFullPath(hardware, root); }
            catch (ArgumentException error) { throw new SettingsReadException(Path.Combine(root, ".env"), "Invalid hardware preset path: " + error.Message, error); }
            foreach (var pair in ReadEnv(path, true)) values[pair.Key] = pair.Value;
        }
        foreach (var pair in overrides) values[pair.Key] = pair.Value;
        var saved = new Dictionary<string, string>();
        stateFile ??= SettingsRecovery.DefaultStateFile;
        if (!defaultsOnly && File.Exists(stateFile))
        {
            try
            {
                using var state = JsonDocument.Parse(File.ReadAllText(stateFile, Encoding.UTF8));
                if (state.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException("Expected a JSON object.");
                foreach (JsonProperty property in state.RootElement.EnumerateObject()) saved[property.Name] = property.Value.ToString();
            }
            catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException)
            { throw new SettingsReadException(stateFile, "Could not read saved installation state: " + error.Message, error); }
        }
        string Get(string key, string fallback = "") => values.GetValueOrDefault(key, fallback);
        string Choice(string key, string name, string fallback) => overrides.ContainsKey(key)
            ? Get(key, fallback) : saved.GetValueOrDefault(name, Get(key, fallback));
        bool Flag(string key, bool fallback) => Get(key) switch { "1" => true, "0" => false, _ => fallback };
        string bluetoothInstance = Get("XR_BLUETOOTH_INSTANCE_ID", saved.GetValueOrDefault("BluetoothInstanceId", ""));
        if (bluetoothInstance.Length == 0) bluetoothInstance = saved.GetValueOrDefault("BluetoothInstanceId", "");
        var settings = new LauncherSettings
        {
            HardwareEnv = hardware, Distribution = Choice("XR_DISTRIBUTION", "Distribution", "Ubuntu-24.04"),
            DeviceName = Get("XR_DEVICE_NAME", Get("XR_TARGET_DEVICE", "XR device")),
            ControllerProfileLabel = Get("XR_CONTROLLER_PROFILE_LABEL", "Configured BLE / IMU controller profile"),
            ReleaseTag = Get("XR_RELEASE_TAG", "latest"), UsbBusIds = Get("XR_USB_BUS_IDS"),
            BluetoothBusId = Get("XR_BLUETOOTH_BUS_ID"), SavedBluetoothInstance = bluetoothInstance,
            BluetoothUsbId = Get("XR_BLUETOOTH_USB_ID"),
            BluetoothEnabled = Flag("XR_BLUETOOTH", bluetoothInstance.Length > 0 || Get("XR_BLUETOOTH_BUS_ID").Length > 0),
            GearVr = Flag("XR_GEARVR", saved.GetValueOrDefault("GearVR", "False").Equals("True", StringComparison.OrdinalIgnoreCase)),
            AutoDriverFrequency = Flag("XR_AUTO_DRIVER_FREQUENCY", true),
            FrequencyHz = Choice("XR_DISPLAY_FREQUENCY_HZ", "FrequencyHz", "60"),
            DisplayMode = Choice("XR_DISPLAY_MODE", "DisplayMode", "extended_sbs"),
            DisplayName = Get("XR_DISPLAY_NAME"), SteamVrPath = Get("XR_STEAMVR_PATH"), ControllerConfig = Get("XR_CONTROLLER_CONFIG"),
            StartTimeoutSeconds = int.TryParse(Get("XR_START_TIMEOUT"), out int seconds) && seconds is >= 5 and <= 600 ? seconds : 180
        };
        settings.loadedHardware = hardware;
        settings.profileLabelOverride = overrides.GetValueOrDefault("XR_CONTROLLER_PROFILE_LABEL");
        settings.deviceNameOverride = overrides.GetValueOrDefault("XR_DEVICE_NAME");
        settings.baseline = settings.GuiValues();
        if (!defaultsOnly)
        {
            if (overrides.ContainsKey("XR_HARDWARE_ENV")) settings.explicitKeys.Add("XR_HARDWARE_ENV");
            var inherited = Load(root, hardwareOverride: hardware, defaultsOnly: true).GuiValues();
            foreach (var pair in settings.baseline)
                if (overrides.ContainsKey(pair.Key) && (pair.Value != inherited[pair.Key] ||
                    (pair.Key is "XR_GEARVR" or "XR_BLUETOOTH" && Get(pair.Key) is "0" or "1"))) settings.explicitKeys.Add(pair.Key);
            // A literal default can still intentionally override a saved choice.
            foreach (var pair in new Dictionary<string, string> { ["XR_DISTRIBUTION"]="Distribution", ["XR_DISPLAY_MODE"]="DisplayMode", ["XR_DISPLAY_FREQUENCY_HZ"]="FrequencyHz" })
                if (overrides.ContainsKey(pair.Key) && saved.TryGetValue(pair.Value, out string? choice) && choice != settings.baseline[pair.Key] &&
                    (pair.Key != "XR_DISPLAY_FREQUENCY_HZ" || !settings.AutoDriverFrequency)) settings.explicitKeys.Add(pair.Key);
        }
        if (hardwareOverride is not null && !defaultsOnly) settings.explicitKeys.Add("XR_HARDWARE_ENV");
        return settings;
    }

    private Dictionary<string, string> GuiValues() => new(StringComparer.OrdinalIgnoreCase)
    {
            ["XR_HARDWARE_ENV"] = HardwareEnv.Trim(), ["XR_DISTRIBUTION"] = Distribution.Trim(),
            ["XR_RELEASE_TAG"] = ReleaseTag.Trim(), ["XR_USB_BUS_IDS"] = UsbBusIds.Trim(),
            ["XR_BLUETOOTH_BUS_ID"] = BluetoothBusId.Trim(), ["XR_BLUETOOTH"] = BluetoothEnabled ? "1" : "0",
            ["XR_BLUETOOTH_USB_ID"] = BluetoothUsbId.Trim(),
            ["XR_GEARVR"] = GearVr ? "1" : "0", ["XR_AUTO_DRIVER_FREQUENCY"] = AutoDriverFrequency ? "1" : "0",
            ["XR_DISPLAY_FREQUENCY_HZ"] = FrequencyHz.Trim(), ["XR_DISPLAY_MODE"] = DisplayMode.Trim(),
            ["XR_DISPLAY_NAME"] = DisplayName.Trim(), ["XR_STEAMVR_PATH"] = SteamVrPath.Trim(),
            ["XR_CONTROLLER_CONFIG"] = ControllerConfig.Trim()
    };

    private void RememberEdits()
    {
        foreach (var pair in GuiValues())
            if (!baseline.TryGetValue(pair.Key, out string? previous) || pair.Value != previous) explicitKeys.Add(pair.Key);
    }

    public void ApplyHardwarePreset(string root)
    {
        if (HardwareEnv.Trim() == loadedHardware) return;
        RememberEdits();
        var selected = Load(root, hardwareOverride: HardwareEnv.Trim(), defaultsOnly: true);
        var profile = ReadEnv(Path.GetFullPath(HardwareEnv.Trim(), root), true);
        DeviceName = deviceNameOverride ?? selected.DeviceName;
        string Keep(string key, string current, string inherited) => explicitKeys.Contains(key) ? current : inherited;
        Distribution = Keep("XR_DISTRIBUTION", Distribution, selected.Distribution);
        ReleaseTag = Keep("XR_RELEASE_TAG", ReleaseTag, selected.ReleaseTag);
        UsbBusIds = Keep("XR_USB_BUS_IDS", UsbBusIds, selected.UsbBusIds);
        BluetoothBusId = Keep("XR_BLUETOOTH_BUS_ID", BluetoothBusId, selected.BluetoothBusId);
        BluetoothUsbId = Keep("XR_BLUETOOTH_USB_ID", BluetoothUsbId, selected.BluetoothUsbId);
        if (!explicitKeys.Contains("XR_BLUETOOTH") && (SavedBluetoothInstance.Length == 0 || profile.GetValueOrDefault("XR_BLUETOOTH") is "0" or "1")) BluetoothEnabled = selected.BluetoothEnabled;
        if (!explicitKeys.Contains("XR_GEARVR") && profile.GetValueOrDefault("XR_GEARVR") is "0" or "1") GearVr = selected.GearVr;
        if (!explicitKeys.Contains("XR_AUTO_DRIVER_FREQUENCY")) AutoDriverFrequency = selected.AutoDriverFrequency;
        FrequencyHz = Keep("XR_DISPLAY_FREQUENCY_HZ", FrequencyHz, selected.FrequencyHz);
        DisplayMode = Keep("XR_DISPLAY_MODE", DisplayMode, selected.DisplayMode);
        DisplayName = Keep("XR_DISPLAY_NAME", DisplayName, selected.DisplayName);
        SteamVrPath = Keep("XR_STEAMVR_PATH", SteamVrPath, selected.SteamVrPath);
        ControllerConfig = Keep("XR_CONTROLLER_CONFIG", ControllerConfig, selected.ControllerConfig);
        ControllerProfileLabel = profileLabelOverride ?? selected.ControllerProfileLabel;
        // New profile choices must also supersede choices cached by the CLI.
        profileChoices.UnionWith(["XR_DISPLAY_FREQUENCY_HZ", "XR_DISPLAY_MODE", "XR_DISTRIBUTION", "XR_GEARVR", "XR_BLUETOOTH"]);
        loadedHardware = HardwareEnv.Trim();
        baseline = GuiValues();
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));
    }

    public void Save(string root)
    {
        ApplyHardwarePreset(root);
        RememberEdits();
        var values = GuiValues();
        foreach (string value in values.Values) ValidateLiteral(value);
        if (!Regex.IsMatch(Distribution, @"^[A-Za-z0-9][A-Za-z0-9._-]*$")) throw new FormatException("Invalid Ubuntu distribution name.");
        if (!Regex.IsMatch(ReleaseTag, @"^[A-Za-z0-9_.-]+$")) throw new FormatException("Use latest or a release tag such as v2.3.0.");
        if (!int.TryParse(FrequencyHz, out int hz) || hz < 1 || hz > 1000) throw new FormatException("Refresh rate must be 1–1000 Hz.");
        if (DisplayMode is not ("direct" or "extended_sbs")) throw new FormatException("Display mode must be direct or extended_sbs.");
        foreach (string id in UsbBusIds.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) ValidateBusId(id);
        if (BluetoothBusId.Length > 0) ValidateBusId(BluetoothBusId.Trim());
        if (BluetoothUsbId.Trim().Length > 0 && !Regex.IsMatch(BluetoothUsbId.Trim(), @"^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$"))
            throw new FormatException("Bluetooth VID:PID must look like 1234:abcd, or be empty to use the saved adapter.");
        ReadEnv(Path.GetFullPath(HardwareEnv.Trim(), root), true);
        string path = Path.Combine(root, ".env");
        string original = File.Exists(path) ? File.ReadAllText(path, Encoding.UTF8) : "# Settings saved by XR Gate Launcher.\n";
        // Preserve advanced settings and comments, including keys the GUI does not expose.
        var persisted = values.Where(pair => explicitKeys.Contains(pair.Key) || profileChoices.Contains(pair.Key))
            .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.OrdinalIgnoreCase);
        var removed = values.Keys.Where(key => !persisted.ContainsKey(key)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        string updated = UpdateEnv(original, persisted, removed);
        if (updated == original) return;
        string temporary = path + ".launcher-" + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllText(temporary, updated, new UTF8Encoding(false));
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        baseline = values;
    }

    public static string UpdateEnv(string original, IReadOnlyDictionary<string, string> values, IReadOnlySet<string>? removed = null)
    {
        var remaining = new HashSet<string>(values.Keys, StringComparer.OrdinalIgnoreCase);
        var lines = new List<string>();
        foreach (string line in original.Replace("\r\n", "\n").TrimEnd('\n').Split('\n'))
        {
            Match match = Regex.Match(line.TrimStart(), @"^(XR_[A-Za-z0-9_]+)=");
            string key = match.Groups[1].Value;
            if (match.Success && removed?.Contains(key) == true) continue;
            if (match.Success && values.TryGetValue(key, out string? value))
            {
                if (remaining.Remove(key)) lines.Add(key + "=" + value);
            }
            else lines.Add(line);
        }
        foreach (string key in remaining) lines.Add(key + "=" + values[key]);
        return string.Join("\n", lines) + "\n";
    }

    private static void ValidateBusId(string value)
    {
        if (!Regex.IsMatch(value, @"^\d+-\d+$")) throw new FormatException("USB BUSID must look like 4-16. Use List USB devices to find it.");
    }
    private static void ValidateLiteral(string value)
    {
        if (value.IndexOfAny(['\r', '\n', '$', '`']) >= 0) throw new FormatException("Settings must be literal values without newlines, $ or backticks.");
    }
}
