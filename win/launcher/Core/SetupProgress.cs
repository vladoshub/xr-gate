using System.Text.Json;

namespace XrGate.Launcher.Core;

public sealed record SetupProgress(bool Pending = false, string Distribution = "", string NextStep = "")
{
    public static SetupProgress Load(string path) => File.Exists(path)
        ? JsonSerializer.Deserialize<SetupProgress>(File.ReadAllText(path)) ?? new() : new();
    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + "." + Guid.NewGuid().ToString("N");
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(this)); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}

public sealed class LauncherReadiness
{
    public bool Known { get; set; }
    public bool Installed { get; set; }
    public bool DisplaySelected { get; set; }
    public bool DisplayConnected { get; set; }
    public bool DeviceConnected { get; set; }
    public bool CanInstall => DisplaySelected && DisplayConnected;
    public string NextStep => !Known ? "Click Refresh status to check setup." :
        !CanInstall ? "Click Select glasses display and choose the glasses display number." : !Installed ? "Click Install to finish setup." :
        !DeviceConnected ? "Connect the configured XR device." : "Click Start VR. USB devices connect automatically.";
    public bool CanStart => Known && Installed && DisplaySelected && DisplayConnected && DeviceConnected;
}

public sealed record CalibrationNotice(string Message, bool Complete = false);
public sealed class CalibrationNotices
{
    private readonly HashSet<string> shown = [];
    public void Reset() => shown.Clear();
    public CalibrationNotice? Observe(string stage)
    {
        if (stage is not ("zeroing" or "optical" or "complete") || !shown.Add(stage)) return null;
        return stage switch
        {
            "zeroing" => new("Keep it still on a stable surface while the gyroscope is zeroed."),
            "optical" => new("Calibrating controller: keep the hand and controller visible to the cameras and keep your head steady. Gently move the controller 10-20 cm forward/back or side to side, with clear starts and stops. Avoid fast wrist rotations. Continue until calibration is confirmed."),
            _ => new("Controller calibration completed. IMU prediction is active.", true)
        };
    }
}
