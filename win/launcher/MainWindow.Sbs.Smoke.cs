using System.Text.Json;
using System.Windows;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifySbsUiAsync()
    {
        var originalSettings = settings;
        var originalAction = smokeAction;
        bool known = readiness.Known, ready = readiness.Installed, wasInstalled = installed;
        bool selected = readiness.DisplaySelected, connected = readiness.DisplayConnected, device = readiness.DeviceConnected;
        bool active = runtimeMayBeRunning;
        int? width = readiness.DisplayWidth, height = readiness.DisplayHeight;
        JsonElement Observation(int? w, int? h, bool present = true) => JsonSerializer.SerializeToElement(new
        {
            device_status = "CONNECTED", display_selected = true, display_connected = present,
            display_width = w, display_height = h
        });
        try
        {
            settings = LauncherSettings.Load(root, hardwareOverride: "hardware/xreal_ultra.env", defaultsOnly: true);
            DataContext = settings;
            installed = readiness.Installed = readiness.Known = true;
            runtimeMayBeRunning = false;
            int launches = 0;
            smokeAction = (_, _) => { launches++; return Task.FromResult(new ActionResult { ExitCode = 0 }); };
            ApplyDeviceObservation(Observation(1920, 1080));
            SetBusy("");
            if (StartButton.IsEnabled || !readiness.SbsOff || DeviceSbsStatus.Visibility != Visibility.Visible || DeviceSbsStatus.Text != " (SBS OFF)")
                throw new InvalidOperationException("SBS OFF did not disable Start VR and append its device label.");
            await StartVrAsync();
            await HandleHotkeyAsync(StartHotkey);
            if (launches != 0) throw new InvalidOperationException("Click/F5 bypassed the SBS mode requirement.");
            ApplyDeviceObservation(Observation(3840, 1080));
            if (!StartButton.IsEnabled || DeviceSbsStatus.Visibility != Visibility.Collapsed || DeviceSbsStatus.Text != "")
                throw new InvalidOperationException("Enabling SBS did not restore Start VR or retained an unwanted SBS ON/OFF label.");
            ApplyDeviceObservation(Observation(null, null));
            if (StartButton.IsEnabled || DeviceSbsStatus.Visibility != Visibility.Collapsed)
                throw new InvalidOperationException("An unknown display mode allowed VR or was falsely labeled SBS OFF.");
            ApplyDeviceObservation(Observation(null, null, present: false));
            if (DeviceSbsStatus.Visibility != Visibility.Collapsed) throw new InvalidOperationException("An unplugged display was labeled SBS OFF.");
            settings = LauncherSettings.Load(root, hardwareOverride: "hardware/generic.env.example", defaultsOnly: true);
            DataContext = settings;
            ApplyDeviceObservation(Observation(1920, 1080));
            if (!StartButton.IsEnabled || DeviceSbsStatus.Visibility != Visibility.Collapsed)
                throw new InvalidOperationException("A device without SBS_RESOLUTION acquired an SBS restriction or label.");
        }
        finally
        {
            settings = originalSettings; DataContext = settings; smokeAction = originalAction;
            readiness.Known = known; readiness.Installed = ready; installed = wasInstalled;
            readiness.DisplaySelected = selected; readiness.DisplayConnected = connected; readiness.DeviceConnected = device;
            readiness.DisplayWidth = width; readiness.DisplayHeight = height;
            runtimeMayBeRunning = active;
            SetBusy("");
        }
    }
}
