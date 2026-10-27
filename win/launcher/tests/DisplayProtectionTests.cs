using XrGate.Launcher.Core;

internal static class DisplayProtectionTests
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    public static void Run(string folder)
    {
        var main = new GuardMonitor("DISPLAY1", "main", new(0, 0, 1920, 1080), new(0, 40, 1920, 1080), true);
        var glasses = new GuardMonitor("DISPLAY3", "glasses", new(1920, 0, 5760, 1080), new(1920, 0, 5760, 1080), false);
        var left = new GuardMonitor("DISPLAY2", "left", new(-2560, -200, 0, 1240), new(-2560, -200, 0, 1200), false);
        var monitors = new[] { main, glasses, left };
        var layout = DisplayGuardLayout.Create("display3", "GLASSES", monitors)!;
        Check(layout is not null, "The exact selected secondary display was rejected.");
        foreach (var selection in new[] { ("", ""), ("DISPLAY1", "main"), ("DISPLAY3", "replacement"), ("DISPLAY9", "glasses") })
            Check(DisplayGuardLayout.Create(selection.Item1, selection.Item2, monitors) is null,
                "Protection accepted a missing, primary, replaced or unknown output.");
        Check(DisplayGuardLayout.Create("DISPLAY3", "glasses", [glasses]) is null, "Protection allowed the only desktop monitor.");
        Check(DisplayGuardLayout.Create("DISPLAY3", "glasses", [main, glasses, glasses with { Id = "clone" }]) is null,
            "A duplicated GDI output could be protected.");
        Check(DisplayGuardLayout.Create("DISPLAY3", "glasses", [main, glasses with { Bounds = main.Bounds }]) is null,
            "An overlapping/cloned display could be protected.");
        Check(layout!.RedirectPointer(1920, 200) == (1919, 200), "The pointer crossed into the protected display.");
        Check(layout.RedirectPointer(5759, 200) == (1919, 200), "A pointer teleport stayed inside the protected display.");
        Check(layout.RedirectPointer(-1200, 100) is null && layout.RedirectPointer(400, 500) is null,
            "Pointer movement on ordinary monitors was blocked.");
        Check(layout.RedirectPointer(5760, 200) is null, "The exclusive right edge was treated as inside the screen.");
        Check(layout.RedirectWindow(new(100, 100, 1100, 800), false) is null, "An ordinary desktop window was moved.");
        Check(layout.RedirectWindow(new(0, 40, 1920, 1080), true) is null, "Maximizing on the main monitor triggered protection.");
        Check(layout.RedirectWindow(new(1800, 200, 2400, 900), false) == new ScreenBounds(1320, 200, 1920, 900),
            "A partially dragged window was not returned intact to the desktop.");
        Check(layout.RedirectWindow(new(1920, 0, 5760, 1080), true) == main.WorkArea,
            "A maximized window was not fitted to the real desktop work area.");
        var negative = DisplayGuardLayout.Create("DISPLAY2", "left", monitors)!;
        Check(negative.RedirectPointer(-1, 100) == (0, 100), "Protection failed at a negative-coordinate monitor edge.");
        Check(negative.RedirectWindow(new(-2500, -150, -1000, 850), false) == new ScreenBounds(0, 40, 1500, 1040),
            "A window from a negative-coordinate display was returned under the taskbar or off screen.");
        Check(layout.RedirectWindow(new(1900, -500, 6900, 1500), false) == main.WorkArea,
            "An oversized window was not contained on a safe monitor.");

        string root = Path.Combine(folder, "display-protection");
        Directory.CreateDirectory(root);
        string state = Path.Combine(root, "state.json"), env = Path.Combine(root, ".env");
        File.WriteAllText(Path.Combine(root, ".env.example"), "");
        var settings = LauncherSettings.Load(root, state);
        Check(settings.ProtectDeviceDisplay, "Display protection is not enabled by default for existing installations.");
        File.WriteAllText(env, "# Keep advanced settings\nXR_START_TIMEOUT=210\nXR_PROTECT_DEVICE_DISPLAY=1\n");
        settings = LauncherSettings.Load(root, state);
        settings.ProtectDeviceDisplay = false;
        settings.FrequencyHz = "unfinished";
        settings.SaveDisplayProtection(root);
        settings = LauncherSettings.Load(root, state);
        Check(!settings.ProtectDeviceDisplay && settings.StartTimeoutSeconds == 210 && settings.FrequencyHz == "60" &&
            File.ReadAllText(env).Contains("# Keep advanced settings"), "Toggling protection saved unfinished fields or lost advanced settings.");
        settings.ProtectDeviceDisplay = true;
        settings.SaveDisplayProtection(root);
        Check(LauncherSettings.Load(root, state).ProtectDeviceDisplay, "Re-enabling protection did not survive reload.");
        Console.WriteLine("OK: exact display identity, primary/clone rejection, cursor boundaries, window relocation, work areas and display protection persistence.");
    }
}
