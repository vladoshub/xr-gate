namespace XrGate.Launcher.Core;

public sealed record HeaderArtwork(string? Device, string? Controller)
{
    public static HeaderArtwork Select(string root, LauncherSettings settings, bool deviceConnected, bool runtimeActive) => new(
        deviceConnected ? LocalImage(root, settings.DeviceImage) : null,
        runtimeActive && settings.ControllerMode != "no" && settings.ControllerImages.TryGetValue(settings.ControllerMode, out string? image)
            ? LocalImage(root, image) : null);

    public static string? LocalImage(string root, string? relative)
    {
        if (string.IsNullOrWhiteSpace(relative) || Path.IsPathRooted(relative)) return null;
        try
        {
            string directory = Path.GetFullPath(Path.Combine(root, "images")) + Path.DirectorySeparatorChar;
            string path = Path.GetFullPath(relative.Replace('\\', Path.DirectorySeparatorChar), root);
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!path.StartsWith(directory, comparison) || !File.Exists(path)) return null;
            return Path.GetExtension(path).ToLowerInvariant() is ".png" or ".jpg" or ".jpeg" or ".bmp" or ".gif" ? path : null;
        }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or NotSupportedException)
        { return null; }
    }
}
