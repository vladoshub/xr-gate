namespace XrGate.Launcher.Core;

public sealed record ConfigurationProblem(string FilePath, string Message, bool CanReset);
public sealed record SettingsSession(LauncherSettings Settings, ConfigurationProblem? Problem);

public static class SettingsRecovery
{
    public static string DefaultStateFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XR-Gate-WSL", "state.json");

    public static SettingsSession Open(string root, string? stateFile = null, string? hardwareOverride = null)
    {
        stateFile ??= DefaultStateFile;
        try { return new(LauncherSettings.Load(root, stateFile, hardwareOverride), null); }
        catch (SettingsReadException error)
        {
            LauncherSettings fallback;
            try { fallback = LauncherSettings.Load(root, defaultsOnly: true); }
            catch (SettingsReadException) { fallback = new LauncherSettings(); }
            bool reset = string.Equals(error.FilePath, Path.GetFullPath(stateFile), StringComparison.OrdinalIgnoreCase) ||
                         string.Equals(error.FilePath, Path.GetFullPath(Path.Combine(root, ".env")), StringComparison.OrdinalIgnoreCase);
            return new(fallback, new(error.FilePath, error.Message, reset));
        }
    }

    public static string BackUpAndReset(ConfigurationProblem problem)
    {
        if (!problem.CanReset) throw new InvalidOperationException("Repair the file or choose another hardware preset, then reload settings.");
        string backup = problem.FilePath + ".before-recovery-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N");
        File.Move(problem.FilePath, backup);
        return backup;
    }
}
