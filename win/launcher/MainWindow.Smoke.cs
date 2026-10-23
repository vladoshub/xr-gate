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
        await VerifySetupUiAsync();
        await VerifyRuntimeObservationAsync();
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
            if (!ActionPanel.IsEnabled || !SettingsPanel.IsEnabled)
                throw new InvalidOperationException("Cancellation left the UI blocked.");
            if (!await ExecuteAsync("Status", saveSettings: false))
                throw new InvalidOperationException("The UI could not start an action after cancellation.");
        }
        Task<bool> closingUsb = ExecuteAsync("Usb", saveSettings: false);
        await Dispatcher.Yield(DispatcherPriority.Background);
        // The close handler uses this same cancellation-and-wait path.
        if (!IsCancellableOperation(busyAction)) throw new InvalidOperationException("USB attach still blocks window closing.");
        await CancelActiveOperationAsync().WaitAsync(TimeSpan.FromSeconds(5));
        if (await closingUsb || busyAction.Length != 0 || !ActionPanel.IsEnabled)
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
        if (RecoveryPanel.Visibility != Visibility.Visible || !SettingsFields.IsEnabled || ActionPanel.IsEnabled || AudioFields.IsEnabled ||
            await ExecuteAsync("Doctor", saveSettings: false) || calls != 0)
            throw new InvalidOperationException("Configuration recovery did not keep the repair UI available and actions blocked.");
        configurationProblem = null;
        smokeAction = null;
        UpdateRecoveryUi();
        await VerifyControllerAutoSaveAsync();
    }

    private async Task VerifyControllerAutoSaveAsync()
    {
        string temporary = Path.Combine(Path.GetTempPath(), "xr-gate-controller-ui-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        string originalMode = settings.ControllerMode, originalBusId = settings.BluetoothBusId,
            originalUsbId = settings.BluetoothUsbId, originalConfig = settings.ControllerConfig;
        bool originalBluetooth = settings.BluetoothEnabled;
        int saves = 0;
        try
        {
            ResetControllerAutoSave();
            MainTabs.SelectedItem = ControllersTab;
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            smokeControllerSave = () => { settings.SaveControllerSettings(temporary); saves++; };
            ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, originalMode == "gestures" ? "gearvr" : "gestures");
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (saves != 1 || controllerSettingsDirty)
                throw new InvalidOperationException("Changing controller mode did not save immediately.");
            BluetoothEnabledBox.SetCurrentValue(CheckBox.IsCheckedProperty, !originalBluetooth);
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (saves != 2 || controllerSettingsDirty)
                throw new InvalidOperationException("Changing the Bluetooth checkbox did not save immediately.");
            BluetoothBusIdBox.SetCurrentValue(TextBox.TextProperty, "9-");
            BluetoothBusIdBox.SetCurrentValue(TextBox.TextProperty, "9-10");
            BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, "abcd:0123");
            ControllerConfigBox.SetCurrentValue(TextBox.TextProperty, @"C:\Новая папка\controllers.json");
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (!controllerSettingsDirty || !controllerSaveTimer.IsEnabled || saves != 2)
                throw new InvalidOperationException("Typing did not defer and combine controller autosaves.");
            await Task.Delay(750);
            await Dispatcher.Yield(DispatcherPriority.Background);
            var values = LauncherSettings.ReadEnv(Path.Combine(temporary, ".env"), true);
            if (saves != 3 || controllerSettingsDirty || values["XR_BLUETOOTH_BUS_ID"] != "9-10" ||
                values["XR_BLUETOOTH_USB_ID"] != "abcd:0123" || values["XR_CONTROLLER_CONFIG"] != settings.ControllerConfig)
                throw new InvalidOperationException("The debounced autosave did not persist the final text values.");
            byte[] before = File.ReadAllBytes(Path.Combine(temporary, ".env"));
            BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, "abcd:");
            await Task.Delay(750);
            await Dispatcher.Yield(DispatcherPriority.Background);
            if (!controllerSettingsDirty || !ControllerSaveStatus.Text.StartsWith("Not saved:") ||
                !before.SequenceEqual(File.ReadAllBytes(Path.Combine(temporary, ".env"))))
                throw new InvalidOperationException("An incomplete controller identifier was saved or had no inline explanation.");
            BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, "abcd:0124");
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (!await ShutdownSessionAsync() || controllerSettingsDirty || saves != 4 ||
                LauncherSettings.ReadEnv(Path.Combine(temporary, ".env"))["XR_BLUETOOTH_USB_ID"] != "abcd:0124")
                throw new InvalidOperationException("Closing did not flush the last valid edit before its debounce elapsed.");
        }
        finally
        {
            smokeControllerSave = null;
            ResetControllerAutoSave();
            settings.ControllerMode = originalMode; settings.BluetoothEnabled = originalBluetooth;
            settings.BluetoothBusId = originalBusId; settings.BluetoothUsbId = originalUsbId; settings.ControllerConfig = originalConfig;
            ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateTarget();
            BluetoothEnabledBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateTarget();
            BluetoothBusIdBox.GetBindingExpression(TextBox.TextProperty)?.UpdateTarget();
            BluetoothUsbIdBox.GetBindingExpression(TextBox.TextProperty)?.UpdateTarget();
            ControllerConfigBox.GetBindingExpression(TextBox.TextProperty)?.UpdateTarget();
            Directory.Delete(temporary, true);
        }
    }

    private async Task VerifySetupUiAsync()
    {
        int installs = 0, prompts = 0;
        bool setupComplete = false;
        readiness.Known = true; readiness.Installed = installed = false;
        readiness.DisplaySelected = readiness.DisplayConnected = readiness.DeviceConnected = false;
        UpdateReadinessUi();
        if (InstallButton.IsEnabled || StartButton.IsEnabled || !DisplayButton.IsEnabled || !Equals(DisplayButton.Tag, true))
            throw new InvalidOperationException("Fresh setup must allow display selection and block Install/Start VR.");
        smokeConfirmation = _ => { prompts++; return true; };
        smokeAction = (action, _) =>
        {
            if (action == "Install")
            {
                installs++;
                return Task.FromResult(new ActionResult { ExitCode = 20, NextStep = "restart_windows" });
            }
            return Task.FromResult(new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new
            {
                installed = setupComplete, setup_complete = setupComplete, running = false,
                install_pending = !setupComplete, install_next_step = "restart_windows",
                display_selected = true, display_connected = true, device_status = "CONNECTED"
            }) });
        };
        await BeginInstallationAsync();
        if (installs != 0 || prompts != 0) throw new InvalidOperationException("Install ignored the missing display.");
        readiness.DisplaySelected = readiness.DisplayConnected = true;
        UpdateReadinessUi();
        if (!InstallButton.IsEnabled || StartButton.IsEnabled || !Equals(InstallButton.Tag, true) || Equals(DisplayButton.Tag, true))
            throw new InvalidOperationException("Choosing a display must enable Install before WSL is installed.");
        smokeConfirmation = _ => { prompts++; return false; };
        await BeginInstallationAsync();
        if (installs != 0 || prompts != 1) throw new InvalidOperationException("Install bypassed the WSL confirmation.");
        smokeConfirmation = message => { prompts++; return message.StartsWith("XR Gate will install"); };
        await BeginInstallationAsync();
        if (installs != 1 || !setupProgress.Pending || setupProgress.NextStep != "restart_windows" ||
            NextStepButton.Content as string != "Restart Windows" || StartButton.IsEnabled)
            throw new InvalidOperationException("A required reboot lost continuation state or enabled Start VR.");
        smokeConfirmation = _ => { prompts++; return false; };
        int previousPrompts = prompts;
        await OfferResumeAsync();
        if (prompts != previousPrompts + 1 || installs != 1)
            throw new InvalidOperationException("Reopening unfinished setup did not offer continuation with confirmation.");
        setupComplete = true;
        await ExecuteAsync("Status", saveSettings: false);
        if (!StartButton.IsEnabled || InstallButton.Content as string != "Update" || setupProgress.Pending)
            throw new InvalidOperationException("Complete setup did not enable Start VR and change Install to Update.");
        ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { device_status = "NOT CONNECTED", display_selected = true, display_connected = false }));
        if (StartButton.IsEnabled || InstallButton.IsEnabled || !DisplayButton.IsEnabled)
            throw new InvalidOperationException("Disconnect left Start VR or Install enabled.");
        FlushLog();
        var before = LogBox.Document.Blocks.Count;
        calibrationNotices.Reset();
        foreach (string stage in new[] { "zeroing", "zeroing", "optical", "optical", "complete", "complete" })
            if (calibrationNotices.Observe(stage) is { } notice) LogCalibration(notice);
        FlushLog();
        var notices = LogBox.Document.Blocks.Skip(before).Cast<System.Windows.Documents.Paragraph>().ToArray();
        if (notices.Length != 3 || notices.Any(item => item.FontSize < 18 || item.FontWeight != FontWeights.Bold) ||
            notices[0].Foreground.ToString() != "#FFFF6666" || notices[2].Foreground != System.Windows.Media.Brushes.LightGreen)
            throw new InvalidOperationException("Calibration log must show two red stages and one green confirmation without repeats.");
        smokeConfirmation = null; smokeAction = null;
    }

    private async Task VerifyRuntimeObservationAsync()
    {
        var releaseStartup = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int statusCalls = 0;
        JsonElement stopped = JsonSerializer.SerializeToElement(new
        {
            installed = true, setup_complete = true, running = false, starting = false,
            display_selected = true, display_connected = true, device_status = "SHARED",
            runtime_exit_message = "fixture camera unavailable", runtime_log_tail = new[] { "capture failed" }
        });
        smokeAction = async (action, cancellation) =>
        {
            if (action == "Run")
            {
                await releaseStartup.Task.WaitAsync(cancellation);
                return new ActionResult { ExitCode = 1, Message = "fixture camera unavailable" };
            }
            if (action == "Status") statusCalls++;
            return new ActionResult { ExitCode = 0, Data = stopped };
        };
        Task<bool> pending = ExecuteAsync("Run", steamVr: true, saveSettings: false);
        await Dispatcher.Yield(DispatcherPriority.Background);
        await ObserveDeviceAsync();
        if (DeviceConnectionStatus.Text != "TRACKING" || statusCalls != 0 || StartButton.IsEnabled)
            throw new InvalidOperationException("Startup must refresh USB status without racing the Linux startup lock.");
        releaseStartup.SetResult();
        if (await pending || busyAction.Length != 0 || StatusTitle.Text != "Tracking stopped during startup")
            throw new InvalidOperationException("Runtime failure left startup busy or failed to show a stopped state.");
        runtimeMayBeRunning = resourcesMayBeOwned = true;
        await ObserveDeviceAsync();
        if (runtimeMayBeRunning || StatusTitle.Text != "Tracking stopped" || StatusDetail.Text != "fixture camera unavailable" || !StopButton.IsEnabled)
            throw new InvalidOperationException("Unexpected runtime exit did not update the banner and retain USB cleanup.");
        int previousCalls = statusCalls;
        await ObserveDeviceAsync();
        if (statusCalls != previousCalls) throw new InvalidOperationException("A stopped runtime kept repeating its exit notifications.");
        resourcesMayBeOwned = false;
        smokeAction = null; SetBusy("");
    }
}
