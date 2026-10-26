using System.Text.Json;
using XrGate.Launcher.Core;

internal static class InstallSourceTests
{
    public static void Run(string source, string parent)
    {
        static void Check(bool valid, string detail) { if (!valid) throw new Exception(detail); }
        string root = Path.Combine(parent, "install-source");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        File.Copy(Path.Combine(source, ".env.example"), Path.Combine(root, ".env.example"), true);
        File.Copy(Path.Combine(source, "hardware/xreal_ultra.env"), Path.Combine(root, "hardware/xreal_ultra.env"), true);
        string state = Path.Combine(root, "state.json"), env = Path.Combine(root, ".env");
        var settings = LauncherSettings.Load(root, state);
        Check(settings.RuntimeDownloadUrl.EndsWith("/releases/latest/download/xr-gate-ubuntu-24.04-linux-x64.tar.gz"), "The default runtime URL is blank or incorrect.");
        settings.ObserveRuntimeDownloadUrl("https://github.com/vladoshub/xr-gate/releases/download/v-resolved/xr-gate-ubuntu-24.04-linux-x64.tar.gz");
        settings.Save(root);
        Check(LauncherSettings.ReadEnv(env).GetValueOrDefault("XR_RUNTIME_URL", "") == "", "Observing the current archive URL pinned the automatic release selection.");
        settings.ReleaseTag = "v-next";
        Check(settings.RuntimeDownloadUrl.Contains("/download/v-next/"), "Changing a release tag kept a stale resolved URL.");
        settings.ObserveRuntimeDownloadUrl("https://mirror.example/old-runtime.tar.gz", "url", "url-old");
        Check(settings.RuntimeDownloadUrl.Contains("/download/v-next/"), "Status from an old custom source replaced the selected official URL.");
        settings.ObserveRuntimeDownloadUrl("https://github.com/vladoshub/xr-gate/releases/download/v-old/runtime.tar.gz", "release", "v-old");
        Check(settings.RuntimeDownloadUrl.Contains("/download/v-next/"), "Status from an older release replaced the selected download URL.");
        settings.RuntimeDownloadUrl = "https://mirror.example/runtime.tar.gz?download=1";
        settings.Save(root);
        settings = LauncherSettings.Load(root, state);
        Check(settings.RuntimeDownloadUrl == "https://mirror.example/runtime.tar.gz?download=1", "A custom source URL did not survive save/reload.");
        settings.ObserveRuntimeDownloadUrl("https://github.com/vladoshub/xr-gate/releases/download/another/runtime.tar.gz");
        Check(settings.RuntimeDownloadUrl.StartsWith("https://mirror.example/"), "Status replaced the user's custom source URL.");
        var changed = new List<string?>();
        settings.PropertyChanged += (_, e) => changed.Add(e.PropertyName);
        settings.RuntimeLocalPath = Path.Combine(root, "O'Brien локальные архивы");
        settings.RuntimeDownloadUrl = "not a URL";
        Check(!settings.RuntimeUrlEnabled && changed.Contains(nameof(settings.RuntimeUrlEnabled)), "A local source did not disable the URL field.");
        settings.Save(root);
        settings = LauncherSettings.Load(root, state);
        Check(!settings.RuntimeUrlEnabled && settings.RuntimeLocalPath.Contains("локальные"), "A local path did not survive save/reload.");
        byte[] before = File.ReadAllBytes(env);
        settings.RuntimeLocalPath = "";
        bool rejected = false;
        try { settings.Save(root); } catch (FormatException) { rejected = true; }
        Check(rejected && before.SequenceEqual(File.ReadAllBytes(env)), "An invalid active URL was accepted or changed saved settings.");
        Check(settings.RuntimeUrlEnabled, "Clearing the local path did not restore URL editing.");
        settings.RuntimeDownloadUrl = "https://mirror.example/downloads/";
        settings.Save(root);

        Check(ActivityMessages.Tone("[Tracking setup] Keep the cameras uncovered.") == "guidance", "Tracking setup instructions lost their prominent style.");
        foreach (string message in new[] { "Attach Request failed - Device busy (exported)", "usbipd attach did not finish within 30 seconds", "Bluetooth is enabled but no controller became ready in 15s", "USB camera is not responding" })
            Check(ActivityMessages.SuggestReconnect(message), "Missing reconnect hint: " + message);
        foreach (string message in new[] { "rfkill: cannot open /dev/rfkill", "Downloading archive: connection timed out", "busctl exited with code 1: no diagnostic output", "Camera permission denied", "Controller calibration completed" })
            Check(!ActivityMessages.SuggestReconnect(message), "Unrelated output suggested reconnecting hardware: " + message);
        Check(new CalibrationNotices().Observe("zeroing")!.Message == "Keep controllers still on a stable surface while the gyroscope is zeroed.", "The controller zeroing instruction was not updated.");
        Console.WriteLine("OK: installation-source settings, observed URLs, local precedence and hardware recovery instructions.");
    }
}
