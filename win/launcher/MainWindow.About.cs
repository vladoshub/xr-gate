using System.Reflection;
using System.Text.Json;
using System.Windows;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private AboutWindow? aboutWindow;
    private string observedRuntimeVersion = "Not checked yet";
    private string observedRuntimeDistribution = "";
    private string observedRuntimeRoot = "";

    private string RuntimeVersionForAbout => observedRuntimeDistribution == settings.Distribution && observedRuntimeRoot == settings.LinuxRoot
        ? observedRuntimeVersion : "Not checked yet";

    private void ObserveAboutVersion(JsonElement data)
    {
        if (!data.TryGetProperty("installed", out var present) ||
            present.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) return;
        observedRuntimeDistribution = data.TryGetProperty("distribution", out var distribution) &&
            distribution.ValueKind == JsonValueKind.String ? distribution.GetString() ?? "" : settings.Distribution;
        observedRuntimeRoot = settings.LinuxRoot;
        // Use the installed release reported by Status, never the requested tag
        // in Settings (which may be "latest" or an update not installed yet).
        observedRuntimeVersion = present.ValueKind == JsonValueKind.False ? "Not installed" :
            data.TryGetProperty("release", out var release) && release.ValueKind == JsonValueKind.String &&
            !string.IsNullOrWhiteSpace(release.GetString()) ? release.GetString()! : "Version unavailable";
        aboutWindow?.UpdateRuntimeVersion(RuntimeVersionForAbout);
    }

    private void About_Click(object sender, RoutedEventArgs e)
    {
        if (aboutWindow is not null) { aboutWindow.Activate(); return; }
        var assembly = typeof(MainWindow).Assembly;
        string launcherVersion = assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
            ?? assembly.GetName().Version?.ToString() ?? "Version unavailable";
        var window = new AboutWindow(root, RuntimeVersionForAbout, launcherVersion) { Owner = this };
        aboutWindow = window;
        window.Activated += (_, _) => window.UpdateRuntimeVersion(RuntimeVersionForAbout);
        window.Closed += (_, _) => aboutWindow = null;
        window.Show();
    }
}
