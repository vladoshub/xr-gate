using System.Text.RegularExpressions;

namespace XrGate.Launcher.Core;

public sealed record ControllerModeChoice(string Id, string Label)
{
    public static IReadOnlyList<ControllerModeChoice> All { get; } = [
        new("simple_gestures", "Gestures"), new("gestures", "Gestures (controller emulation)"),
        new("gearvr", "Gear VR Controllers"), new("no", "No")];
}

public sealed record HardwarePresetChoice(string Path, string Name, IReadOnlySet<string> UsbIds)
{
    public static HardwarePresetChoice None { get; } = new("", "Select a device…", new HashSet<string>());
    public string Label => Path.Length == 0 ? Name : Name + " — " + System.IO.Path.GetFileName(Path);

    public static List<HardwarePresetChoice> Load(string root, string selected, Action<string>? warning = null)
    {
        string directory = System.IO.Path.Combine(root, "hardware");
        var files = Directory.Exists(directory) ? Directory.GetFiles(directory, "*.env").ToList() : [];
        if (selected.Length > 0 && !files.Any(path => SamePath(root, path, selected))) files.Add(System.IO.Path.GetFullPath(selected, root));
        var choices = new List<HardwarePresetChoice>();
        foreach (string path in files.Order(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                var env = LauncherSettings.ReadEnv(path, required: true);
                var ids = env.GetValueOrDefault("XR_USB_IDS", "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                if (ids.Any(id => !Regex.IsMatch(id, @"^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$"))) throw new FormatException("Invalid XR_USB_IDS.");
                choices.Add(new(System.IO.Path.GetRelativePath(root, path), env.GetValueOrDefault("XR_DEVICE_NAME", System.IO.Path.GetFileNameWithoutExtension(path)),
                    ids.Select(id => id.ToLowerInvariant()).ToHashSet(StringComparer.OrdinalIgnoreCase)));
            }
            catch (Exception error) when (error is FormatException or IOException or UnauthorizedAccessException)
            { warning?.Invoke(System.IO.Path.GetFileName(path) + ": " + error.Message); }
        }
        return choices;
    }

    public static bool SamePath(string root, string left, string right) =>
        !string.IsNullOrWhiteSpace(left) && !string.IsNullOrWhiteSpace(right) &&
        string.Equals(System.IO.Path.GetFullPath(left, root), System.IO.Path.GetFullPath(right, root), StringComparison.OrdinalIgnoreCase);

    public static IReadOnlySet<string> UsbIdsFromInstances(IEnumerable<string> instances)
    {
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string instance in instances)
        {
            var match = Regex.Match(instance, @"^USB\\VID_([0-9a-f]{4})&PID_([0-9a-f]{4})(?:&|\\)", RegexOptions.IgnoreCase);
            if (match.Success) ids.Add(match.Groups[1].Value.ToLowerInvariant() + ":" + match.Groups[2].Value.ToLowerInvariant());
        }
        return ids;
    }

    // Multiple matching profiles need a manual choice, including profiles which
    // share VID:PID but differ in tracking/runtime settings.
    public static List<HardwarePresetChoice> Matches(IEnumerable<HardwarePresetChoice> choices, IReadOnlySet<string> connected) =>
        choices.Where(choice => choice.UsbIds.Count > 0 && choice.UsbIds.All(connected.Contains)).ToList();
}
