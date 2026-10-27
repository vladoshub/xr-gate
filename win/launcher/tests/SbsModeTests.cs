using XrGate.Launcher.Core;

internal static class SbsModeTests
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    public static void Run(string folder)
    {
        var expected = new DisplayResolution(3840, 1080);
        Check(DisplayResolution.Parse(" 3840X1080 ") == expected, "SBS resolution was not parsed consistently.");
        Check(DisplayResolution.Parse("") is null && DisplayResolution.Parse(null) is null, "An optional SBS check became mandatory.");
        foreach (string invalid in new[] { "1920", "0x1080", "3840x0", "-3840x1080", "3840*1080", "3840x1080x2", "999999999999x1080", "3840 x 1080" })
        {
            bool rejected = false;
            try { DisplayResolution.Parse(invalid); } catch (FormatException) { rejected = true; }
            Check(rejected, "Invalid SBS resolution accepted: " + invalid);
        }
        var state = new LauncherReadiness { Known = true, Installed = true, HardwareSelected = true,
            DeviceConnected = true, DisplayConnected = true, DisplaySelected = true, SbsResolution = expected };
        Check(!state.CanStart && state.SbsEnabled is null && !state.SbsOff, "An unknown display mode allowed VR or was falsely shown as SBS OFF.");
        state.DisplayWidth = 1920; state.DisplayHeight = 1080;
        Check(state.SbsOff && !state.CanStart && state.NextStep.Contains("3840x1080"), "A monoscopic output allowed VR or gave no SBS instructions.");
        state.DisplayWidth = 3840;
        Check(state.SbsEnabled == true && !state.SbsOff && state.CanStart, "The required SBS output did not enable VR.");
        state.DisplayHeight = 1200;
        Check(state.SbsOff && !state.CanStart, "Only the width was checked for SBS.");
        state.DisplayConnected = false;
        Check(state.SbsEnabled is null && !state.SbsOff && !state.CanStart, "An unplugged display retained an SBS OFF label or allowed VR.");
        state.DisplayConnected = true; state.DisplayWidth = 1920; state.DisplayHeight = 1080; state.SbsResolution = null;
        Check(state.SbsEnabled == true && !state.SbsOff && state.CanStart, "A device without SBS_RESOLUTION was subjected to a mode check.");

        string root = Path.Combine(folder, "sbs-modes"), hardware = Path.Combine(root, "hardware");
        Directory.CreateDirectory(hardware);
        File.WriteAllText(Path.Combine(root, ".env.example"), "");
        File.WriteAllText(Path.Combine(hardware, "stereo.env"), "XR_DEVICE_NAME=Stereo\nSBS_RESOLUTION=3840x1080\n");
        File.WriteAllText(Path.Combine(hardware, "single.env"), "XR_DEVICE_NAME=Single\n");
        string stateFile = Path.Combine(root, "state.json");
        var settings = LauncherSettings.Load(root, stateFile, hardwareOverride: "hardware/stereo.env");
        Check(settings.SbsResolution == expected, "The device's SBS_RESOLUTION did not reach the launcher.");
        settings.Save(root);
        Check(!File.ReadAllText(Path.Combine(root, ".env")).Contains("SBS_RESOLUTION"), "Saving a device choice pinned its SBS mode globally.");
        settings.HardwareEnv = "hardware/single.env"; settings.ApplyHardwarePreset(root);
        Check(settings.SbsResolution is null, "Switching to a single-mode device retained the previous SBS requirement.");
        settings.HardwareEnv = "hardware/stereo.env"; settings.ApplyHardwarePreset(root);
        Check(settings.SbsResolution == expected, "Switching back lost the device's SBS requirement.");
        File.AppendAllText(Path.Combine(root, ".env"), "SBS_RESOLUTION=2560x1440\n");
        settings = LauncherSettings.Load(root, stateFile, hardwareOverride: "hardware/stereo.env");
        Check(settings.SbsResolution == new DisplayResolution(2560, 1440), "An explicit SBS override was not read.");
        File.WriteAllText(Path.Combine(hardware, "broken.env"), "SBS_RESOLUTION=oops\n");
        bool invalidPreset = false;
        try { LauncherSettings.ReadEnv(Path.Combine(hardware, "broken.env")); } catch (SettingsReadException) { invalidPreset = true; }
        Check(invalidPreset, "A malformed SBS requirement silently disabled the launch check.");
        Console.WriteLine("OK: optional SBS profile, complete resolution checks, unknown/disconnected modes, launch gating and preset changes.");
    }
}
