using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifyDisplayProtectionUiAsync()
    {
        bool enabled = settings.ProtectDeviceDisplay, active = runtimeMayBeRunning;
        int? originalWidth = readiness.DisplayWidth, originalHeight = readiness.DisplayHeight;
        string hardware = settings.HardwareEnv, mode = settings.DisplayMode;
        string name = protectedDisplayName, id = protectedDisplayId, observedHardware = protectedHardware;
        bool selected = readiness.DisplaySelected, connected = readiness.DisplayConnected, device = readiness.DeviceConnected;
        bool known = readiness.Known, ready = readiness.Installed, wasInstalled = installed, canDelete = canDeleteInstallation;
        var previousAction = smokeAction;
        JsonElement Observation(bool running = false, bool starting = false) => JsonSerializer.SerializeToElement(new
        {
            display_selected = true, display_connected = true, display_width = 3840, display_height = 1080, display_name = @"\\.\DISPLAY7", display_device_id = "fixture-monitor",
            device_status = "TRACKING", running, starting
        });
        try
        {
            settings.HardwareEnv = hardwareChoices.First().Path;
            settings.DisplayMode = "extended_sbs";
            settings.ProtectDeviceDisplay = true;
            runtimeMayBeRunning = false;
            ApplyDeviceObservation(Observation());
            if (displayProtectionRequested) throw new InvalidOperationException("Opening the launcher protected a display before tracking started.");
            ApplyStatus(Observation(running: true), diagnosticLog: false);
            SetBusy("");
            if (!displayProtectionRequested || !ProtectDeviceDisplayBox.IsEnabled)
                throw new InvalidOperationException("Tracking did not enable display protection or its live toggle is disabled.");
            ProtectDeviceDisplayBox.SetCurrentValue(CheckBox.IsCheckedProperty, false);
            ProtectDeviceDisplayBox.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            if (settings.ProtectDeviceDisplay || displayProtectionRequested)
                throw new InvalidOperationException("Turning protection off during tracking left it active.");
            ProtectDeviceDisplayBox.SetCurrentValue(CheckBox.IsCheckedProperty, true);
            ProtectDeviceDisplayBox.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            if (!displayProtectionRequested) throw new InvalidOperationException("Re-enabling protection during tracking did not take effect.");
            ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { display_selected = true, display_connected = false }));
            if (displayProtectionRequested) throw new InvalidOperationException("Unplugging the selected display retained protection.");
            ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { display_selected = true, display_connected = true, display_width = 3840, display_height = 1080, display_name = @"\\.\DISPLAY7" }));
            if (displayProtectionRequested) throw new InvalidOperationException("A helper without an exact monitor identity enabled protection.");
            ApplyDeviceObservation(Observation());
            if (!displayProtectionRequested) throw new InvalidOperationException("Reconnecting the exact display did not restore protection.");
            ApplyStatus(Observation(), diagnosticLog: false);
            if (displayProtectionRequested || runtimeMayBeRunning)
                throw new InvalidOperationException("A stopped/exited tracking process left display protection enabled.");
            ApplyStatus(Observation(starting: true), diagnosticLog: false);
            if (!displayProtectionRequested) throw new InvalidOperationException("Protection was not enabled during tracking startup.");
            var statusStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var pendingStatus = new TaskCompletionSource<ActionResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            smokeAction = (action, _) =>
            {
                if (action == "Status") { statusStarted.TrySetResult(); return pendingStatus.Task; }
                return Task.FromResult(new ActionResult { ExitCode = 0 });
            };
            Task<bool> stopping = ExecuteAsync("Stop", saveSettings: false);
            try
            {
                await statusStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
                if (displayProtectionRequested)
                    throw new InvalidOperationException("A slow status refresh kept the screen blocked after Stop succeeded.");
            }
            finally
            {
                pendingStatus.TrySetResult(new ActionResult { ExitCode = 0, Data = Observation() });
                await stopping;
                smokeAction = previousAction;
            }
            ApplyStatus(Observation(running: true), diagnosticLog: false);
            settings.DisplayMode = "direct"; UpdateDisplayProtection();
            if (displayProtectionRequested) throw new InvalidOperationException("Direct Mode protected an unrelated desktop output.");
            settings.DisplayMode = "extended_sbs"; settings.HardwareEnv = ""; UpdateDisplayProtection();
            if (displayProtectionRequested) throw new InvalidOperationException("Clearing the hardware selection retained display protection.");
        }
        finally
        {
            settings.ProtectDeviceDisplay = enabled; settings.HardwareEnv = hardware; settings.DisplayMode = mode;
            smokeAction = previousAction;
            runtimeMayBeRunning = active;
            protectedDisplayName = name; protectedDisplayId = id; protectedHardware = observedHardware;
            readiness.DisplaySelected = selected; readiness.DisplayConnected = connected; readiness.DeviceConnected = device;
            readiness.DisplayWidth = originalWidth; readiness.DisplayHeight = originalHeight;
            readiness.Known = known; readiness.Installed = ready; installed = wasInstalled; canDeleteInstallation = canDelete;
            ProtectDeviceDisplayBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateTarget();
            SetBusy("");
            UpdateDisplayProtection();
        }
    }
}
