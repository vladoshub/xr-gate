using XrGate.Launcher.Core;

internal static class SetupTests
{
    public static void Run(string folder)
    {
        static void Check(bool value, string message) { if (!value) throw new Exception(message); }
        for (int bits = 0; bits < 32; bits++)
        {
            var state = new LauncherReadiness { Known = (bits & 1) != 0, Installed = (bits & 2) != 0,
                DisplaySelected = (bits & 4) != 0, DisplayConnected = (bits & 8) != 0, DeviceConnected = (bits & 16) != 0 };
            Check(state.CanStart == (bits == 31), "Start VR ignored an incomplete prerequisite: " + bits);
            Check(state.CanInstall == ((bits & 12) == 12), "Install must require a selected, connected display independently of WSL: " + bits);
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
        Console.WriteLine("OK: display-before-install gating, complete Start VR prerequisites, resumed setup and calibration notices.");
    }
}
