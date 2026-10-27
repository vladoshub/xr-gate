using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifyHotkeysUiAsync()
    {
        bool enabled = settings.GlobalHotkeys, audioEnabled = settings.AudioEnabled;
        bool fixtureRunning = false;
        int runs = 0, restarts = 0, stops = 0;
        try
        {
            settings.GlobalHotkeys = true; settings.AudioEnabled = false;
            readiness.Known = true;
            installed = readiness.Installed = false;
            readiness.DisplaySelected = readiness.DisplayConnected = readiness.DeviceConnected = true;
            readiness.DisplayWidth = 3840; readiness.DisplayHeight = 1080;
            runtimeMayBeRunning = resourcesMayBeOwned = false;
            smokeAction = (action, _) =>
            {
                if (action == "Run") { runs++; fixtureRunning = true; }
                if (action == "Restart") { restarts++; fixtureRunning = true; }
                if (action == "Stop") { stops++; fixtureRunning = false; }
                return Task.FromResult(new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new
                {
                    installed = true, setup_complete = true, running = fixtureRunning,
                    display_selected = true, display_connected = true, display_width = 3840, display_height = 1080, device_status = "SHARED"
                }) });
            };
            SetBusy("");
            await HandleHotkeyAsync(StartHotkey);
            await HandleHotkeyAsync(RestartHotkey);
            if (runs != 0 || restarts != 0) throw new InvalidOperationException("Hotkeys bypassed installation/start prerequisites.");
            installed = readiness.Installed = true; readiness.DeviceConnected = false; SetBusy("");
            await HandleHotkeyAsync(StartHotkey);
            if (runs != 0) throw new InvalidOperationException("F5 started VR without the selected device.");
            readiness.DeviceConnected = true; SetBusy("");
            await HandleHotkeyAsync(StartHotkey);
            await HandleHotkeyAsync(StartHotkey);
            if (runs != 1 || !runtimeMayBeRunning || !RestartButton.IsEnabled) throw new InvalidOperationException("F5 duplicated an active session or left Restart unavailable.");
            await HandleHotkeyAsync(RestartHotkey);
            if (restarts != 1 || stops != 0 || !runtimeMayBeRunning || !resourcesMayBeOwned)
                throw new InvalidOperationException("F7 did not use the separate restart action or lost USB ownership.");
            await HandleHotkeyAsync(StopHotkey);
            await HandleHotkeyAsync(RestartHotkey);
            if (stops != 1 || restarts != 1 || runtimeMayBeRunning || resourcesMayBeOwned || RestartButton.IsEnabled)
                throw new InvalidOperationException("F6 failed to clear tracking/USB ownership or F7 restarted an absent session.");
            GlobalHotkeysBox.SetCurrentValue(CheckBox.IsCheckedProperty, false);
            GlobalHotkeysBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateSource();
            GlobalHotkeysBox.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            if (settings.GlobalHotkeys || !HotkeysStatus.Text.Contains("disabled"))
                throw new InvalidOperationException("The Hotkeys checkbox did not disable shortcuts immediately.");
            await HandleHotkeyAsync(StartHotkey); await HandleHotkeyAsync(StopHotkey); await HandleHotkeyAsync(RestartHotkey);
            if (runs != 1 || stops != 1 || restarts != 1) throw new InvalidOperationException("Disabled hotkeys still executed actions.");
            settings.GlobalHotkeys = true;
            foreach (string launch in new[] { "Run", "Restart" })
            {
                int launches = 0, cleanup = 0;
                var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                smokeAction = async (action, cancellation) =>
                {
                    if (action == launch) { launches++; await release.Task.WaitAsync(cancellation); }
                    if (action == "Stop") cleanup++;
                    return new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new
                    {
                        installed = true, setup_complete = true, running = false,
                        display_selected = true, display_connected = true, display_width = 3840, display_height = 1080, device_status = "SHARED"
                    }) };
                };
                runtimeMayBeRunning = resourcesMayBeOwned = launch == "Restart"; SetBusy("");
                int key = launch == "Run" ? StartHotkey : RestartHotkey;
                Task pending = HandleHotkeyAsync(key);
                await Dispatcher.Yield(DispatcherPriority.Background);
                await HandleHotkeyAsync(key);
                await HandleHotkeyAsync(StopHotkey).WaitAsync(TimeSpan.FromSeconds(5));
                await pending.WaitAsync(TimeSpan.FromSeconds(5));
                if (launches != 1 || cleanup != 1 || busyAction.Length != 0 || runtimeMayBeRunning)
                    throw new InvalidOperationException("F6 could not cancel startup/restart, or repeated keys started concurrent actions.");
            }
            foreach (Button button in new[] { DisplayButton, InstallButton, StartButton, RestartButton, RefreshStatusButton, StopButton, DeleteButton, SaveLogButton })
                if (string.IsNullOrWhiteSpace(button.ToolTip?.ToString()) || !ToolTipService.GetShowOnDisabled(button))
                    throw new InvalidOperationException("Run button has no tooltip when disabled: " + button.Content);
            if (!RestartButton.ToolTip.ToString()!.Contains("without restarting SteamVR")) throw new InvalidOperationException("Restart tooltip does not explain the SteamVR behavior.");
        }
        finally
        {
            settings.GlobalHotkeys = enabled; settings.AudioEnabled = audioEnabled;
            GlobalHotkeysBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateTarget();
            smokeAction = null; runtimeMayBeRunning = resourcesMayBeOwned = false; SetBusy("");
        }
    }
}
