using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifyUiSmokeAsync()
    {
        if (!smoke) throw new InvalidOperationException("UI fixture is only available in smoke mode.");
        configurationProblem = null;
        UpdateRecoveryUi();
        if (!MainTabs.Items.Cast<TabItem>().Select(tab => tab.Header?.ToString()).SequenceEqual(new[] { "Run", "Controllers", "Audio", "Settings", "Tools" }) || Icon is null)
            throw new InvalidOperationException("The tab order, labels or application icon is missing.");
        MainTabs.SelectedItem = ControllersTab;
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        bool originalGearVr = settings.GearVr;
        ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, "gestures");
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateSource();
        if (settings.GearVr || settings.ControllerMode != "gestures") throw new InvalidOperationException("The Gestures choice did not reach settings.");
        ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, "gearvr");
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateSource();
        if (!settings.GearVr) throw new InvalidOperationException("The Gear VR choice did not reach settings.");
        settings.GearVr = originalGearVr;
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateTarget();
        string originalUsbId = settings.BluetoothUsbId;
        var usbIdBinding = BluetoothUsbIdBox.GetBindingExpression(TextBox.TextProperty)
            ?? throw new InvalidOperationException("The Bluetooth VID:PID field has no binding.");
        usbIdBinding.UpdateTarget();
        if (BluetoothUsbIdBox.Text != originalUsbId)
            throw new InvalidOperationException("The Bluetooth VID:PID field did not load its profile value.");
        BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, "abcd:0123");
        usbIdBinding.UpdateSource();
        if (settings.BluetoothUsbId != "abcd:0123")
            throw new InvalidOperationException("Editing Bluetooth VID:PID did not update settings.");
        BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, originalUsbId);
        usbIdBinding.UpdateSource();
        MainTabs.SelectedIndex = 0;
        ApplyBluetoothDevices(JsonSerializer.SerializeToElement(new
        {
            devices = new[]
            {
                new { address = "AA:BB:CC:DD:EE:01", label = "Same controller [AA:BB:CC:DD:EE:01]" },
                new { address = "AA:BB:CC:DD:EE:02", label = "Same controller [AA:BB:CC:DD:EE:02]" }
            },
            selected = new { left = "AA:BB:CC:DD:EE:01", right = (string?)null }
        }));
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        if (LeftBluetoothController.SelectedValue as string != "AA:BB:CC:DD:EE:01" ||
            RightBluetoothController.SelectedValue as string != "none" || LeftBluetoothController.Items.Count != 3)
            throw new InvalidOperationException("The controller picker lost duplicate-name identity or the omitted side.");
        string originalSource = settings.AudioSourceId, originalOutput = settings.AudioOutputId;
        string audioOutput = "alsa:" + new string('a', 64);
        settings.AudioSourceId = "fixture-source"; settings.AudioOutputId = audioOutput;
        ApplyAudioDevices([new("fixture-source", "Speakers")], [new(audioOutput, "Generic USB output")]);
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        if (WindowsAudioSource.SelectedValue as string != "fixture-source" || WslAudioOutput.SelectedValue as string != audioOutput)
            throw new InvalidOperationException("Audio controls did not restore the saved identities.");
        ApplyAudioDevices([], []);
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        if (WslAudioOutput.SelectedValue as string != audioOutput || ((AudioDeviceChoice)WslAudioOutput.SelectedItem).Available)
            throw new InvalidOperationException("Audio disconnect replaced or forgot the saved choice.");
        ApplyAudioDevices([new("fixture-source", "Speakers")], [new(audioOutput, "Reconnected output")]);
        runtimeMayBeRunning = true; SetBusy("");
        if (!AudioFields.IsEnabled || SettingsFields.IsEnabled || !((AudioDeviceChoice)WslAudioOutput.SelectedItem).Available)
            throw new InvalidOperationException("Audio reconnect or changes during VR are unavailable.");
        runtimeMayBeRunning = false;
        settings.AudioSourceId = originalSource; settings.AudioOutputId = originalOutput; BindAudioChoices(); SetBusy("");
        smokeAction = async (action, cancellation) =>
        {
            if (action is "Doctor" or "Test" or "Usb" or "BluetoothScan" or "Pair") await Task.Delay(Timeout.Infinite, cancellation);
            return new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new { installed = true, running = false }) };
        };
        if (!await ExecuteAsync("Run", saveSettings: false) || !runtimeMayBeRunning)
            throw new InvalidOperationException("A newly opened tracking console was treated as already closed.");
        if (!await ExecuteAsync("Stop", saveSettings: false) || runtimeMayBeRunning)
            throw new InvalidOperationException("Stopping a tracking console did not clear the running state.");
        // Exercise the real async action, routed Cancel click, finally block and
        // subsequent action. No PowerShell, WSL, USB or user files are touched.
        foreach (string action in new[] { "Doctor", "Test", "Usb", "BluetoothScan", "Pair" })
        {
            Task<bool> pending = ExecuteAsync(action, saveSettings: false);
            await Dispatcher.Yield(DispatcherPriority.Background);
            if (CancelDiagnosticsButton.Visibility != Visibility.Visible)
                throw new InvalidOperationException(action + " has no usable Cancel button.");
            CancelDiagnosticsButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            if (await pending.WaitAsync(TimeSpan.FromSeconds(5)))
                throw new InvalidOperationException("Cancelled check reported success.");
            if (!ActionPanel.IsEnabled || !ConnectionPanel.IsEnabled || !SettingsPanel.IsEnabled)
                throw new InvalidOperationException("Cancellation left the UI blocked.");
            if (!await ExecuteAsync("Status", saveSettings: false))
                throw new InvalidOperationException("The UI could not start an action after cancellation.");
        }
        Task<bool> closingUsb = ExecuteAsync("Usb", saveSettings: false);
        await Dispatcher.Yield(DispatcherPriority.Background);
        // The close handler uses this same cancellation-and-wait path.
        if (!IsCancellableOperation(busyAction)) throw new InvalidOperationException("USB attach still blocks window closing.");
        await CancelActiveOperationAsync().WaitAsync(TimeSpan.FromSeconds(5));
        if (await closingUsb || busyAction.Length != 0 || !ActionPanel.IsEnabled || !ConnectionPanel.IsEnabled)
            throw new InvalidOperationException("Closing during USB attach left an active operation.");
        foreach (bool duringStartup in new[] { false, true })
        {
            int stops = 0;
            smokeAction = async (action, cancellation) =>
            {
                if (action == "Run" && duringStartup) await Task.Delay(Timeout.Infinite, cancellation);
                if (action == "Stop") stops++;
                return new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new { installed = true, running = false }) };
            };
            runtimeMayBeRunning = resourcesMayBeOwned = true;
            Task<bool>? startup = duringStartup ? ExecuteAsync("Run", saveSettings: false) : null;
            if (!await ShutdownSessionAsync().WaitAsync(TimeSpan.FromSeconds(5)) || stops != 1 || runtimeMayBeRunning || resourcesMayBeOwned)
                throw new InvalidOperationException("Closing did not stop the active/starting runtime and release its USB ownership.");
            if (startup is not null && await startup) throw new InvalidOperationException("Closing left startup running.");
        }
        smokeAction = (_, _) => Task.FromResult(new ActionResult { ExitCode = 1 });
        resourcesMayBeOwned = true;
        if (await ShutdownSessionAsync() || !resourcesMayBeOwned)
            throw new InvalidOperationException("Incomplete cleanup was reported as a completed close.");
        resourcesMayBeOwned = false;
        int calls = 0;
        smokeAction = (_, _) => { calls++; throw new InvalidOperationException("Recovery invoked a backend action."); };
        configurationProblem = new ConfigurationProblem(Path.Combine(root, ".env"), "fixture configuration error", true);
        UpdateRecoveryUi();
        if (RecoveryPanel.Visibility != Visibility.Visible || !SettingsFields.IsEnabled || ActionPanel.IsEnabled || ConnectionPanel.IsEnabled || AudioFields.IsEnabled ||
            await ExecuteAsync("Doctor", saveSettings: false) || calls != 0)
            throw new InvalidOperationException("Configuration recovery did not keep the repair UI available and actions blocked.");
        configurationProblem = null;
        smokeAction = null;
        UpdateRecoveryUi();
    }
}
