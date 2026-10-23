using System.Text.Json;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private readonly CalibrationNotices calibrationNotices = new();
    private CancellationTokenSource? calibrationCancellation;
    private Task? calibrationTask;

    private string RuntimeWindowToken()
    {
        try
        {
            using var data = JsonDocument.Parse(File.ReadAllText(Path.Combine(stateDirectory, "runtime-window.json")));
            return data.RootElement.GetProperty("Token").GetString() ?? "";
        }
        catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException) { return ""; }
    }

    private void BeginCalibrationProgress()
    {
        if (smoke || !settings.GearVr || calibrationTask is { IsCompleted: false }) return;
        calibrationNotices.Reset();
        calibrationCancellation?.Dispose();
        calibrationCancellation = CancellationTokenSource.CreateLinkedTokenSource(deviceCancellation.Token);
        string previousToken = RuntimeWindowToken();
        calibrationTask = ObserveCalibrationAsync(previousToken, calibrationCancellation.Token);
    }

    private async Task ObserveCalibrationAsync(string previousToken, CancellationToken cancellation)
    {
        try
        {
            string token;
            while ((token = RuntimeWindowToken()).Length == 0 || token == previousToken)
                await Task.Delay(200, cancellation);
            var saved = LauncherSettings.Load(root);
            await RuntimeProgressClient.FollowAsync(root, saved.Distribution, saved.LinuxUser, saved.LinuxRoot, token, stage =>
            {
                if (calibrationNotices.Observe(stage) is { } notice) LogCalibration(notice);
            }, cancellation);
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { Log("[Controllers] Calibration notifications unavailable: " + error.Message); }
    }

    private async Task StopCalibrationProgressAsync()
    {
        calibrationCancellation?.Cancel();
        if (calibrationTask is not null) await calibrationTask;
        calibrationTask = null;
        calibrationCancellation?.Dispose();
        calibrationCancellation = null;
    }
}
