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
        VerifyArtworkUi();
        await VerifyDisplayProtectionUiAsync();
        await VerifyInstallSourceUiAsync();
        // Model a fresh Windows installation where wsl.exe exists but its
        // distribution inventory hangs. Install and first-login continuation
        // must reach the installer without requiring a ready WSL lease.
        smokeWslLease = (_, _) => throw new TimeoutException("fixture WSL inventory did not respond");
        try
        {
            await VerifySetupUiAsync();
            await VerifyFirstLoginUiAsync();
        }
        finally { smokeWslLease = null; }
        await VerifyHotkeysUiAsync();
        await VerifyRuntimeObservationAsync();
        if (!MainTabs.Items.Cast<TabItem>().Select(tab => tab.Header?.ToString()).SequenceEqual(new[] { "Run", "Controllers", "Audio", "Settings", "Tools" }) || Icon is null)
            throw new InvalidOperationException("The tab order, labels or application icon is missing.");
        MainTabs.SelectedItem = ControllersTab;
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        string originalMode = settings.ControllerMode;
        ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, "no");
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateSource();
        if (settings.GearVr || settings.ControllerMode != "no") throw new InvalidOperationException("The No controllers choice did not reach settings.");
        ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, "gestures");
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateSource();
        if (settings.GearVr || settings.ControllerMode != "gestures") throw new InvalidOperationException("The Gestures choice did not reach settings.");
        ControllerModeBox.SetCurrentValue(ComboBox.SelectedValueProperty, "gearvr");
        ControllerModeBox.GetBindingExpression(ComboBox.SelectedValueProperty)?.UpdateSource();
        if (!settings.GearVr) throw new InvalidOperationException("The Gear VR choice did not reach settings.");
        settings.ControllerMode = originalMode;
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
        await VerifyDeleteUiAsync();
        await VerifyAudioStartUiAsync();
        await VerifyControllerAutoSaveAsync();
    }

    private async Task VerifyDeleteUiAsync()
    {
        int deletes = 0;
        installed = readiness.Installed = true; readiness.Known = true;
        readiness.DisplaySelected = readiness.DisplayConnected = readiness.DeviceConnected = true;
        UpdateReadinessUi();
        smokeConfirmation = message =>
        {
            if (!message.Contains("Ubuntu-24.04") || !message.Contains("ALL files") || !message.Contains("cannot be undone"))
                throw new InvalidOperationException("Delete confirmation omitted the distribution or full data-loss explanation.");
            return false;
        };
        bool fail = true;
        smokeAction = (action, _) =>
        {
            if (action != "Delete" || deleteDistribution != "Ubuntu-24.04")
                throw new InvalidOperationException("Delete targeted another action or an unconfirmed distribution.");
            deletes++;
            return Task.FromResult(new ActionResult { ExitCode = fail ? 1 : 0, Data = JsonSerializer.SerializeToElement(new { deleted = !fail }) });
        };
        if (await DeleteInstallationAsync() || deletes != 0)
            throw new InvalidOperationException("Canceling deletion invoked the backend.");
        smokeConfirmation = _ => true;
        if (await DeleteInstallationAsync() || deletes != 1 || !installed)
            throw new InvalidOperationException("A failed deletion erased installation readiness.");
        fail = false;
        if (!await DeleteInstallationAsync() || deletes != 2 || installed || StartButton.IsEnabled || DeleteButton.IsEnabled ||
            InstallButton.Content as string != "Install" || setupProgress.Pending)
            throw new InvalidOperationException("Deletion did not reset installation controls and continuation state.");
        smokeAction = null; smokeConfirmation = null;
    }

    private async Task VerifyAudioStartUiAsync()
    {
        bool enabled = settings.AudioEnabled;
        string output = settings.AudioOutputId;
        try
        {
            MainTabs.SelectedItem = AudioTab;
            string first = "alsa:" + new string('a', 64), second = "alsa:" + new string('b', 64);
            settings.AudioEnabled = true; settings.AudioOutputId = "none";
            ApplyAudioDevices([], [new(first, "First output"), new(second, "Second output")]);
            await SelectAudioOutputOnStartAsync(CancellationToken.None);
            if (settings.AudioOutputId != first || WslAudioOutput.SelectedValue as string != first)
                throw new InvalidOperationException("Start VR did not select the first available Ubuntu output in the UI.");
            settings.AudioOutputId = second; BindAudioChoices();
            await SelectAudioOutputOnStartAsync(CancellationToken.None);
            if (settings.AudioOutputId != second) throw new InvalidOperationException("Start VR replaced an available saved output.");
            AudioEnabledBox.SetCurrentValue(CheckBox.IsCheckedProperty, false);
            AudioEnabledBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateSource();
            AudioEnabledBox.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            await SelectAudioOutputOnStartAsync(CancellationToken.None);
            if (settings.AudioEnabled || AudioDeviceFields.IsEnabled || settings.AudioOutputId != second)
                throw new InvalidOperationException("Disabling audio lost its selection, kept controls enabled or auto-selected another device.");
        }
        finally { settings.AudioEnabled = enabled; settings.AudioOutputId = output; BindAudioChoices(); }
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
        settings.HardwareEnv = ""; settings.ApplyHardwarePreset(root); InitializeHardwareUi();
        readiness.Known = true; readiness.Installed = installed = false;
        readiness.DisplaySelected = readiness.DisplayConnected = readiness.DeviceConnected = false;
        readiness.HardwareConnected = false;
        UpdateReadinessUi();
        if (InstallButton.IsEnabled || Equals(InstallButton.Tag, true) || StartButton.IsEnabled || DisplayButton.IsEnabled || Equals(DisplayButton.Tag, true))
            throw new InvalidOperationException("Fresh setup must have no selected device and block Install/Display/Start VR.");
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
        if (installs != 0 || prompts != 0) throw new InvalidOperationException("Install ignored the missing device preset.");
        settings.HardwareEnv = hardwareChoices.First().Path; settings.ApplyHardwarePreset(root); InitializeHardwareUi();
        UpdateReadinessUi();
        if (!InstallButton.IsEnabled || StartButton.IsEnabled || !Equals(InstallButton.Tag, true) || DisplayButton.IsEnabled)
            throw new InvalidOperationException("A chosen preset must enable Install without hardware or display, but keep display selection unavailable.");
        ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { device_status = "CONNECTED", display_selected = false, display_connected = false }));
        if (!DisplayButton.IsEnabled || !Equals(DisplayButton.Tag, true)) throw new InvalidOperationException("A connected device with no automatic display did not enable and pulse manual selection.");
        ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { device_status = "CONNECTED", display_selected = true, display_connected = true }));
        if (!DisplayButton.IsEnabled || Equals(DisplayButton.Tag, true)) throw new InvalidOperationException("An automatically selected display still requested manual selection.");
        SetBusy("Install");
        if (InstallButton.IsEnabled || Equals(InstallButton.Tag, true))
            throw new InvalidOperationException("Install must stop blinking while unavailable during setup.");
        SetBusy("");
        if (!Equals(InstallButton.Tag, true)) throw new InvalidOperationException("An available unfinished Install did not resume blinking.");
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
        if (!StartButton.IsEnabled || InstallButton.Content as string != "Update" || Equals(InstallButton.Tag, true) || setupProgress.Pending)
            throw new InvalidOperationException("Complete setup did not enable Start VR and change Install to Update.");
        if (await ExecuteAsync("Run", steamVr: true) || StatusDetail.Text != "fixture WSL inventory did not respond" || runtimeMayBeRunning)
            throw new InvalidOperationException("Start VR bypassed the WSL lease or reported tracking after a failed WSL probe.");
        ApplyDeviceObservation(JsonSerializer.SerializeToElement(new { device_status = "NOT CONNECTED", display_selected = true, display_connected = false }));
        if (StartButton.IsEnabled || !InstallButton.IsEnabled || DisplayButton.IsEnabled || Equals(DisplayButton.Tag, true))
            throw new InvalidOperationException("Disconnect must block Start VR/display selection while keeping preset-based Update available.");
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

    private async Task VerifyFirstLoginUiAsync()
    {
        try
        {
            foreach (string outcome in new[] { "ready", "incomplete", "aborted", "closing", "busy", "restart" })
            {
                int installs = 0, confirmations = 0;
                var releaseLogin = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                readiness.Known = true;
                installed = readiness.Installed = runtimeMayBeRunning = resourcesMayBeOwned = false;
                readiness.DisplaySelected = readiness.DisplayConnected = readiness.DeviceConnected = true;
                UpdateReadinessUi();
                smokeConfirmation = _ => { confirmations++; return true; };
                smokeFirstLoginWait = (_, cancellation) => releaseLogin.Task.WaitAsync(cancellation);
                smokeAction = (action, _) =>
                {
                    if (action == "Install")
                    {
                        installs++;
                        if (installs == 1 || outcome == "incomplete")
                            return Task.FromResult(new ActionResult
                            {
                                ExitCode = 20, NextStep = "ubuntu_first_login",
                                Data = JsonSerializer.SerializeToElement(new { first_login = new { process_id = 1234, start_ticks = "638969000000000000" } })
                            });
                        return Task.FromResult(outcome == "restart"
                            ? new ActionResult { ExitCode = 20, NextStep = "restart_windows" }
                            : new ActionResult { ExitCode = 0 });
                    }
                    bool complete = installs > 1 && outcome == "ready";
                    return Task.FromResult(new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new
                    {
                        installed = complete, setup_complete = complete, running = false,
                        display_selected = true, display_connected = true, device_status = "CONNECTED"
                    }) });
                };
                await BeginInstallationAsync();
                Task waiting = firstLoginWaitTask ?? throw new InvalidOperationException("First Ubuntu login did not start automatic continuation.");
                if (installs != 1 || confirmations != 1 || SetupNoticeText.Visibility != Visibility.Visible ||
                    SetupNoticeText.FontSize < 20 || !SetupNoticeText.Text.Contains("USERNAME AND PASSWORD") || !SetupNoticeText.Text.Contains("exit"))
                    throw new InvalidOperationException("First login did not show prominent account/password/exit instructions.");
                if (outcome == "closing") { closing = true; CancelFirstLoginWait(); }
                else
                {
                    if (outcome == "busy") SetBusy("Doctor");
                    releaseLogin.SetResult(outcome != "aborted");
                }
                await waiting.WaitAsync(TimeSpan.FromSeconds(5));
                closing = false;
                if (outcome == "ready")
                {
                    if (installs != 2 || confirmations != 1 || !installed || setupProgress.Pending || SetupNoticeText.Visibility != Visibility.Collapsed)
                        throw new InvalidOperationException("Ubuntu account creation did not resume installation once without another confirmation or restart.");
                }
                else if (outcome == "incomplete" || outcome == "aborted")
                {
                    if (installs != (outcome == "incomplete" ? 2 : 1) || confirmations != 1 ||
                        firstLoginCancellation is not null || !SetupNoticeText.Text.Contains("CLICK INSTALL") || !SetupNoticeText.Text.Contains("exit"))
                        throw new InvalidOperationException("An incomplete Ubuntu login did not stop retrying and explain manual continuation.");
                }
                else if (outcome == "closing" && installs != 1)
                    throw new InvalidOperationException("Closing while creating a Linux account started another installation.");
                else if (outcome == "busy")
                {
                    if (installs != 1 || !SetupNoticeText.Text.Contains("CLICK INSTALL"))
                        throw new InvalidOperationException("A concurrent action silently lost Ubuntu setup continuation.");
                    SetBusy("");
                }
                else if (outcome == "restart")
                {
                    if (installs != 2 || confirmations != 2 || !SetupNoticeText.Text.Contains("RESTART WINDOWS"))
                        throw new InvalidOperationException("A required restart after automatic continuation failed or awaited its own login task.");
                    deviceCancellation.Dispose();
                    deviceCancellation = new CancellationTokenSource();
                }
            }
        }
        finally
        {
            closing = false;
            CancelFirstLoginWait();
            firstLoginWaitTask = null;
            smokeFirstLoginWait = null; smokeConfirmation = null; smokeAction = null;
            SetupNoticeText.Visibility = Visibility.Collapsed;
        }
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
