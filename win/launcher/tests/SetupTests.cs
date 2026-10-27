using System.Text.Json;
using XrGate.Launcher.Core;

internal static class SetupTests
{
    public static void Run(string folder)
    {
        static void Check(bool value, string message) { if (!value) throw new Exception(message); }
        for (int bits = 0; bits < 128; bits++)
        {
            var state = new LauncherReadiness { Known = (bits & 1) != 0, Installed = (bits & 2) != 0,
                DisplaySelected = (bits & 4) != 0, DisplayConnected = (bits & 8) != 0, DeviceConnected = (bits & 16) != 0,
                HardwareSelected = (bits & 32) != 0, HardwareConnected = (bits & 64) != 0 };
            Check(state.CanStart == ((bits & 63) == 63), "Start VR ignored an incomplete prerequisite: " + bits);
            Check(state.CanInstall == state.HardwareSelected, "Install must require a device preset, independently of connection/display: " + bits);
            Check(state.CanSelectDisplay == (state.HardwareSelected && (state.DeviceConnected || state.HardwareConnected)), "Display selection ignored device presence: " + bits);
            Check(state.NeedsDisplaySelection == (state.CanSelectDisplay && (!state.DisplaySelected || !state.DisplayConnected)), "Display selection hint ignored a known output or missing hardware: " + bits);
        }
        var pending = new SetupProgress(true, "Ubuntu-24.04", "restart_windows");
        string path = Path.Combine(folder, "setup", "setup-progress.json");
        Check(!SetupProgress.Load(path).Pending, "Fresh setup was marked pending.");
        pending.Save(path);
        Check(SetupProgress.Load(path) == pending, "An unfinished setup did not survive reopening.");
        new SetupProgress().Save(path);
        Check(!SetupProgress.Load(path).Pending && Directory.GetFiles(Path.GetDirectoryName(path)!).Length == 1,
            "Completed setup retained a pending marker or temporary files.");
        var notices = new CalibrationNotices();
        foreach (string stage in new[] { "zeroing", "optical", "complete" })
        {
            var notice = notices.Observe(stage);
            Check(notice is not null && notice.Complete == (stage == "complete"), "Calibration stage or color was lost.");
            Check(notices.Observe(stage) is null, "Calibration instructions spam on every frame.");
        }
        Check(notices.Observe("unrelated") is null, "An unrelated log produced calibration instructions.");
        notices.Reset();
        Check(notices.Observe("zeroing") is not null, "A new tracking run did not reset calibration instructions.");
        var login = FirstLoginSession.Read(JsonSerializer.SerializeToElement(new
        {
            first_login = new { process_id = 1234, start_ticks = "638969000000000000" }
        }));
        Check(login == new FirstLoginSession(1234, 638969000000000000), "Ubuntu first-login process identity was lost.");
        foreach (string json in new[] { "{}", "null", "{\"first_login\":null}",
            "{\"first_login\":{\"process_id\":\"1234\",\"start_ticks\":\"1\"}}",
            "{\"first_login\":{\"process_id\":-1,\"start_ticks\":\"1\"}}",
            "{\"first_login\":{\"process_id\":1234,\"start_ticks\":1}}",
            "{\"first_login\":{\"process_id\":1234,\"start_ticks\":\"invalid\"}}" })
            Check(FirstLoginSession.Read(JsonSerializer.Deserialize<JsonElement>(json)) is null,
                "Invalid first-login metadata should allow manual continuation without throwing.");
        string removalState = Path.Combine(folder, "removal-state.json");
        File.WriteAllText(removalState, JsonSerializer.Serialize(new { Distribution = "Ubuntu-24.04", LinuxUser = "saved-user" }));
        var removal = DistributionRemoval.Load(removalState);
        Check(removal.Distribution == "Ubuntu-24.04" && removal.Confirmation.Contains("ALL files") &&
            removal.Confirmation.Contains("cannot be undone") && removal.Confirmation.Contains("Ubuntu-24.04"),
            "The deletion confirmation lost its saved distribution or permanent-data-loss warning.");
        foreach (string name in new[] { "docker-desktop", "docker-desktop-data", "", "Ubuntu;bad" })
        {
            File.WriteAllText(removalState, JsonSerializer.Serialize(new { Distribution = name, LinuxUser = "saved-user" }));
            bool rejected = false;
            try { DistributionRemoval.Load(removalState); } catch (InvalidDataException) { rejected = true; }
            Check(rejected, "An invalid/system distribution became an XR Gate deletion target.");
        }
        File.WriteAllText(removalState, JsonSerializer.Serialize(new { Distribution = "Ubuntu-24.04", InstallPending = true }));
        Check(DistributionRemoval.Load(removalState).Distribution == "Ubuntu-24.04", "An unfinished Ubuntu installation could not be selected for deletion.");
        File.WriteAllText(removalState, JsonSerializer.Serialize(new { Distribution = "Ubuntu-24.04", LinuxUser = "", InstallPending = false }));
        bool emptyRejected = false;
        try { DistributionRemoval.Load(removalState); } catch (InvalidDataException) { emptyRejected = true; }
        Check(emptyRejected, "A display-only or deleted installation could unregister Ubuntu.");
        File.WriteAllText(removalState, JsonSerializer.Serialize(new { Distribution = "Ubuntu-24.04", LinuxUser = "", InstallPending = false, DriverPath = "saved-driver" }));
        var driversOnly = DistributionRemoval.Load(removalState);
        Check(!driversOnly.RemoveUbuntu && driversOnly.Confirmation.Contains("SteamVR driver copies") && !driversOnly.Confirmation.Contains("ALL files"), "Leftover driver cleanup was unavailable or claimed it would delete an unowned Ubuntu.");
        Console.WriteLine("OK: preset-before-install gating, connected-device display selection, complete Start VR prerequisites, resumed setup and deletion notices.");
    }
}
