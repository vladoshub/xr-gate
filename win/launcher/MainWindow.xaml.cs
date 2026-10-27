using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow : Window
{
    private readonly string root;
    private readonly string stateDirectory;
    private readonly bool smoke;
    private readonly WslKeeper keeper;
    private readonly ProcessRunner runner;
    private readonly ConcurrentQueue<ActivityEntry> pendingLog = new();
    private readonly DispatcherTimer logTimer;
    private readonly DispatcherTimer deviceTimer;
    private CancellationTokenSource deviceCancellation = new();
    private Task? deviceObservation;
    private readonly StreamWriter? sessionLog;
    private LauncherSettings settings;
    private ConfigurationProblem? configurationProblem;
    private Task<bool>? activeTask;
    private Task<bool>? stopTask;
    private CancellationTokenSource? operationCancellation;
    private string busyAction = "";
    private bool reconnectHintShown;
    private string nextStep = "";
    private string pausedDistribution = "";
    private bool installed;
    private bool runtimeMayBeRunning;
    private bool closeApproved;
    private bool closing;
    private bool resourcesReleased;
    private bool resourcesMayBeOwned;
    private Func<string, CancellationToken, Task<ActionResult>>? smokeAction;
    private Func<string, CancellationToken, Task>? smokeWslLease;

    public MainWindow(string bundleRoot, bool smokeTest = false)
    {
        root = bundleRoot;
        smoke = smokeTest;
        InitializeComponent();
        var configuration = SettingsRecovery.Open(root);
        settings = configuration.Settings;
        configurationProblem = configuration.Problem;
        ControllerModeBox.ItemsSource = ControllerModeChoice.All;
        DataContext = settings;
        InitializeHardwareUi();
        stateDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "XR-Gate-WSL");
        if (!smoke)
        {
            string logs = Path.Combine(stateDirectory, "launcher-logs");
            try
            {
                Directory.CreateDirectory(logs);
                string path = Path.Combine(logs, DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Environment.ProcessId + ".log");
                sessionLog = new StreamWriter(path, false, new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { pendingLog.Enqueue(new("Session log is unavailable: " + error.Message)); }
        }
        keeper = new WslKeeper(Log);
        runner = new ProcessRunner(root, stateDirectory, Log);
        audio = new AudioBridge(root, Log);
        InitializeAudioUi();
        InitializeControllerAutoSave();
        InitializeDisplayProtection();
        logTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        logTimer.Tick += (_, _) => FlushLog();
        logTimer.Start();
        deviceTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        deviceTimer.Tick += (_, _) => StartDeviceObservation();
        StopButton.IsEnabled = false;
        Log("XR Gate Launcher " + typeof(MainWindow).Assembly.GetName().Version);
        Log("Bundle: " + root);
        Log("Windows: " + Environment.OSVersion);
        if (!smoke)
        {
            try { setupProgress = SetupProgress.Load(SetupProgressPath); }
            catch (Exception error) { Log("Could not read unfinished setup: " + error.Message); }
        }
        UpdateRecoveryUi();
        SourceInitialized += (_, _) => InitializeHotkeys();
        Loaded += async (_, _) =>
        {
            if (smoke)
            {
                // Exercise the actual WPF resource dictionary, window, bindings
                // and settings loader without installing WSL or touching devices.
                ApplyTemplate();
                UpdateLayout();
                if (LogBox is null || StartButton is null || !IsLoaded) { Application.Current.Shutdown(1); return; }
                try { await VerifyUiSmokeAsync(); }
                catch (Exception error) { Console.Error.WriteLine(error); Application.Current.Shutdown(1); return; }
                closeApproved = true;
                Close();
                return;
            }
            await DetectHardwareAsync();
            if (configurationProblem is null) await ExecuteAsync("Status", saveSettings: false);
            await OfferResumeAsync();
            StartDeviceObservation();
            StartAudioDeviceRefresh();
            deviceTimer.Start();
        };
    }

    private void Log(string message)
    {
        string line = DateTime.Now.ToString("HH:mm:ss") + "  " + message;
        lock (pendingLog)
        {
            if (resourcesReleased) return;
            sessionLog?.WriteLine(line);
            pendingLog.Enqueue(new(line, ActivityMessages.Tone(message)));
            if (!reconnectHintShown && ActivityMessages.SuggestReconnect(message))
            {
                reconnectHintShown = true;
                string hint = DateTime.Now.ToString("HH:mm:ss") + "  [Hint] " + ActivityMessages.ReconnectHint;
                sessionLog?.WriteLine(hint);
                pendingLog.Enqueue(new(hint, "guidance"));
            }
        }
    }

    private void SetBusy(string action)
    {
        if (action.Length > 0 && action != busyAction) { lock (pendingLog) reconnectHintShown = false; }
        busyAction = action;
        bool busy = action.Length > 0;
        bool recovery = configurationProblem is not null;
        ActionPanel.IsEnabled = ToolsPanel.IsEnabled = !busy && !recovery;
        SettingsPanel.IsEnabled = !busy;
        SettingsFields.IsEnabled = !busy && (!runtimeMayBeRunning || recovery);
        ControllersPanel.IsEnabled = !busy && !runtimeMayBeRunning && !recovery;
        AudioFields.IsEnabled = !busy && !recovery;
        RefreshAudioDevicesButton.IsEnabled = !busy && !recovery && audioRefreshTask is not { IsCompleted: false };
        SaveSettingsButton.IsEnabled = AdvancedSettingsButton.IsEnabled = !busy && !runtimeMayBeRunning && !recovery;
        RecoveryPanel.IsEnabled = !busy;
        Progress.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        StopButton.IsEnabled = busy ? action is "Run" or "Restart" : installed || runtimeMayBeRunning || resourcesMayBeOwned ||
            (!smoke && File.Exists(Path.Combine(stateDirectory, "state.json")));
        NextStepButton.IsEnabled = !busy && !recovery;
        CancelDiagnosticsButton.Visibility = IsCancellableOperation(action) ? Visibility.Visible : Visibility.Collapsed;
        CancelDiagnosticsButton.Content = action is "Usb" or "Detach" ? "Cancel USB operation" :
            action is "BluetoothScan" ? "Cancel scan" : action is "Pair" or "Controllers" ? "Cancel controller setup" :
            action is "Doctor" or "Status" ? "Cancel diagnostics" : "Cancel check";
        CancelDiagnosticsButton.IsEnabled = true;
        UpdateReadinessUi();
    }

    private async Task<bool> ExecuteAsync(string action, bool steamVr = false, bool console = false, bool saveSettings = true)
    {
        if (configurationProblem is not null && action != "Stop") { UpdateRecoveryUi(); return false; }
        if (busyAction.Length > 0 || activeTask is { IsCompleted: false }) return false;
        activeTask = PerformAsync(action, steamVr, console, saveSettings);
        return await activeTask;
    }

    private Task KeepUbuntuForActionAsync(string action, string distribution, CancellationToken cancellation)
    {
        // Install owns WSL setup and Ubuntu's first login. A readiness probe
        // before it can time out on the Windows WSL stub and prevent setup.
        if (action is "Install" or "Stop" or "Delete" or "Status" or "Doctor" or "Display") return Task.CompletedTask;
        if (smokeWslLease is not null) return smokeWslLease(distribution, cancellation);
        return smokeAction is null ? keeper.EnsureAsync(distribution, allowMissing: false, cancellation: cancellation) : Task.CompletedTask;
    }

    private async Task<bool> PerformAsync(string action, bool steamVr, bool console, bool saveSettings)
    {
        bool trackingAction = action is "Run" or "Restart";
        SetBusy(action);
        StatusTitle.Text = trackingAction ? action == "Restart" ? "Restarting tracking…" : "Starting tracking…" : action + "…";
        StatusDetail.Text = console ? "Complete the prompts in the separate console, then return here." : "You can follow progress in the activity log.";
        WarningText.Visibility = NextStepButton.Visibility = Visibility.Collapsed;
        operationCancellation = new CancellationTokenSource();
        if (action is "Doctor" or "Status") operationCancellation.CancelAfter(ProcessRunner.DiagnosticTimeout);
        if (action is "Usb" or "Detach") operationCancellation.CancelAfter(ProcessRunner.UsbTimeout);
        if (action is "BluetoothScan" or "Pair" or "Stop") operationCancellation.CancelAfter(ProcessRunner.UsbTimeout);
        if (action == "Test") operationCancellation.CancelAfter(TimeSpan.FromSeconds(settings.StartTimeoutSeconds + 5));
        try
        {
            if (action is "Stop" or "Detach" or "Delete" or "Restart") { await StopCalibrationProgressAsync(); await StopAudioAsync(required: true); }
            if (action == "Delete")
            {
                if (audioRefreshTask is not null) await audioRefreshTask;
                if (deviceObservation is not null) await deviceObservation;
                keeper.Dispose();
            }
            bool writesSettings = saveSettings && action is not ("Doctor" or "Status" or "Test" or "Stop" or "Delete");
            if (action == "Install") { settings.FrequencyHz = "60"; settings.DisplayMode = "extended_sbs"; }
            if (writesSettings && !smoke) { settings.Save(root); ControllerSettingsSaved(); }
            var actionSettings = writesSettings || smokeAction is not null || action is "Stop" or "Delete" ? settings : LauncherSettings.Load(root);
            await KeepUbuntuForActionAsync(action, actionSettings.Distribution, operationCancellation.Token);
            if (trackingAction) { runtimeMayBeRunning = true; UpdateDisplayProtection(); BeginCalibrationProgress(); }
            if (action is "Run" or "Restart" or "Usb" or "BluetoothScan" or "Pair" or "Controllers") resourcesMayBeOwned = true;
            ActionResult result = await RunActionAsync(action, steamVr, console, operationCancellation.Token,
                timeLimit: action == "Test" ? TimeSpan.FromSeconds(actionSettings.StartTimeoutSeconds + 5) : null);
            nextStep = result.NextStep;
            if (action == "Install" && result.Success && result.Data.ValueKind == JsonValueKind.Object &&
                result.Data.TryGetProperty("frequency_hz", out var installedFrequency) && installedFrequency.TryGetInt32(out int hz))
            {
                settings.FrequencyHz = hz.ToString(System.Globalization.CultureInfo.InvariantCulture);
                settings.DisplayMode = "extended_sbs";
                if (!smoke) settings.Save(root);
            }
            if (action == "Install") firstLoginSession = FirstLoginSession.Read(result.Data);
            if (action == "Install") SaveSetupProgress(new(!result.Success, settings.Distribution, nextStep));
            pausedDistribution = result.Data.ValueKind == JsonValueKind.Object && result.Data.TryGetProperty("distribution", out var distro)
                ? distro.GetString() ?? settings.Distribution : settings.Distribution;
            if (nextStep == "restart_ubuntu") keeper.Dispose();

            if (action == "Delete" && result.Success) ApplyDeletedInstallation();
            if (result.Success && action == "Stop") { runtimeMayBeRunning = false; UpdateDisplayProtection(); }
            if (result.Success && action is "Stop" or "Detach") resourcesMayBeOwned = false;
            if (result.Success && action == "BluetoothScan") ApplyBluetoothDevices(result.Data);
            if (action == "Status" && result.Success) ApplyStatus(result.Data);
            else if (trackingAction || action == "Install" || ((action is "Stop" or "Display" or "Driver") && result.Success))
            {
                try
                {
                    var status = await RunActionAsync("Status", cancellation: operationCancellation.Token);
                    if (status.Success) ApplyStatus(status.Data);
                    else Log("Status: " + status.Instruction);
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception error) { Log("Status: " + error.Message); }
            }
            if (trackingAction)
            {
                // A console-only start can return before the Linux supervisor
                // acquires its lock. Keep observing it until Status confirms exit.
                if (result.Success) runtimeMayBeRunning = true;
                if (result.Success && steamVr) await SelectAudioOutputOnStartAsync(operationCancellation.Token);
                await UpdateAudioAsync(result.Success);
                if (!runtimeMayBeRunning) await StopCalibrationProgressAsync();
            }
            else if (action == "Status" && result.Success) await UpdateAudioAsync(runtimeMayBeRunning);
            StatusTitle.Text = result.Success ? action switch
            {
                "Run" => steamVr ? "VR is ready" : "Tracking started",
                "Restart" => "Tracking restarted",
                "Stop" => "Tracking stopped",
                "Delete" => "XR Gate installation deleted",
                "Install" => installed ? "Installation complete" : "Finish setup",
                "Status" => runtimeMayBeRunning ? "Tracking is running" : readiness.CanStart ? "Ready to start" : "Next setup step",
                _ => action + " complete"
            } : result.ExitCode == 20 ? "One more setup step" : trackingAction && !runtimeMayBeRunning ? "Tracking stopped during startup" : "Action needs attention";
            StatusDetail.Text = result.Instruction.Length > 0 ? result.Instruction : action == "Status"
                ? readiness.NextStep
                : action == "Stop" ? "The managed runtime stopped and its USB devices were returned to Windows."
                : settings.ShowCommandPrompt ? "Complete any startup or calibration prompts in the tracking console."
                : "Tracking runs in the background. For console commands, stop tracking, enable Show command prompt in Settings, then start again.";
            if (!result.Success && reconnectHintShown) StatusDetail.Text += "\n" + ActivityMessages.ReconnectHint;
            if (result.Warnings.Length > 0)
            {
                WarningText.Text = string.Join("\n", result.Warnings);
                WarningText.Visibility = Visibility.Visible;
            }
            if (nextStep is "ubuntu_first_login" or "restart_ubuntu" or "restart_windows")
            {
                NextStepButton.Content = nextStep == "restart_windows" ? "Restart Windows" : nextStep == "restart_ubuntu" ? "Restart Ubuntu & continue" : "Open Ubuntu";
                NextStepButton.Visibility = Visibility.Visible;
            }
            Log(result.Success ? "[Launcher] " + action + " complete." : "[Launcher] " + result.Instruction);
            return result.Success;
        }
        catch (OperationCanceledException)
        {
            if (action is "BluetoothScan" or "Pair" or "Controllers")
            {
                StatusTitle.Text = "Stopping controller setup…";
                try
                {
                    var stopped = await RunActionAsync("Stop", timeLimit: ProcessRunner.UsbTimeout);
                    if (stopped.Success) { resourcesMayBeOwned = false; runtimeMayBeRunning = false; }
                    StatusDetail.Text = stopped.Success ? "Controller setup cancelled and selected USB devices returned to Windows." : stopped.Instruction;
                }
                catch (Exception error) { StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); }
                StatusTitle.Text = "Controller setup interrupted";
            }
            else if (action is "Usb" or "Detach")
            {
                StatusTitle.Text = "USB operation interrupted";
                StatusDetail.Text = "The USB operation was cancelled or reached its time limit. Some selected devices may already be attached; use List USB devices before retrying.";
                Log("[Launcher] " + action + " interrupted. Check the current USB device state before retrying.");
            }
            else if (IsCancellableOperation(action))
            {
                StatusTitle.Text = "Check interrupted";
                StatusDetail.Text = "The check was cancelled or reached its time limit. You can start another action.";
                Log("[Launcher] " + action + " interrupted.");
            }
            else Log("[Launcher] Startup wait cancelled; requesting a clean runtime stop.");
            return false;
        }
        catch (Exception exception)
        {
            StatusTitle.Text = "Action needs attention";
            StatusDetail.Text = exception.Message;
            Log("ERROR: " + exception.Message);
            return false;
        }
        finally
        {
            operationCancellation.Dispose();
            operationCancellation = null;
            SetBusy("");
            if (action is "Usb" or "Install" or "Run" or "Restart") StartAudioDeviceRefresh();
        }
    }

    private void ApplyStatus(JsonElement data, bool diagnosticLog = true)
    {
        if (data.ValueKind != JsonValueKind.Object) return;
        ObserveAboutVersion(data);
        if (data.TryGetProperty("runtime_download_url", out var runtimeUrl) && runtimeUrl.ValueKind == JsonValueKind.String)
            settings.ObserveRuntimeDownloadUrl(runtimeUrl.GetString() ?? "",
                data.TryGetProperty("runtime_source", out var runtimeSource) && runtimeSource.ValueKind == JsonValueKind.String ? runtimeSource.GetString() ?? "" : "release",
                data.TryGetProperty("release", out var runtimeTag) && runtimeTag.ValueKind == JsonValueKind.String ? runtimeTag.GetString() ?? "" : "");
        ApplyDeviceObservation(data);
        installed = data.TryGetProperty("setup_complete", out var complete) && complete.ValueKind == JsonValueKind.True;
        readiness.Known = true;
        readiness.Installed = installed;
        canDeleteInstallation = data.TryGetProperty("can_delete", out var deletable) ? deletable.ValueKind == JsonValueKind.True : installed;
        if (data.TryGetProperty("install_pending", out var pending) && pending.ValueKind == JsonValueKind.True)
        {
            string step = data.TryGetProperty("install_next_step", out var selectedStep) ? selectedStep.GetString() ?? "" : "";
            SaveSetupProgress(new(true, settings.Distribution, step));
        }
        else if (installed) SaveSetupProgress(new());
        var value = default(JsonElement);
        runtimeMayBeRunning = data.TryGetProperty("running", out value) && value.GetBoolean();
        runtimeMayBeRunning |= data.TryGetProperty("starting", out var starting) && starting.ValueKind == JsonValueKind.True;
        UpdateDisplayProtection();
        UpdateHeaderArtwork();
        if (!diagnosticLog) return;
        if (data.TryGetProperty("frequency_hz", out value)) Log("[Launcher] Configured driver: " + value + " Hz.");
        if (data.TryGetProperty("display_frequency_hz", out value) && value.ValueKind == JsonValueKind.Number)
            Log("[Launcher] Glasses display current mode: " + value + " Hz.");
        else Log("[Launcher] Glasses display not identified; current refresh rate is unknown.");
        if (data.TryGetProperty("display_detection_message", out value) && value.ValueKind == JsonValueKind.String && value.GetString() is { Length: > 0 } message)
            Log("[Launcher] Display detection: " + message);
    }

    private static bool IsCancellableOperation(string action) => action is "Doctor" or "Status" or "Test" or "List USB devices" or "Usb" or "Detach" or "BluetoothScan" or "Pair" or "Controllers";

    private async Task CancelActiveOperationAsync()
    {
        operationCancellation?.Cancel();
        if (activeTask is not null) await activeTask;
    }

    private Task<ActionResult> RunActionAsync(string action, bool steamVr = false, bool console = false, CancellationToken cancellation = default, TimeSpan? timeLimit = null, bool quiet = false)
    {
        IReadOnlyDictionary<string, string>? parameters = action switch
        {
            "Pair" => new Dictionary<string, string>
            {
                ["LeftControllerMac"] = LeftBluetoothController.SelectedValue as string ?? "none",
                ["RightControllerMac"] = RightBluetoothController.SelectedValue as string ?? "none"
            },
            "Delete" => new Dictionary<string, string> { ["DeleteDistributionName"] = deleteDistribution },
            _ => null
        };
        return smokeAction is not null ? smokeAction(action, cancellation) : runner.RunAsync(action, steamVr, console, cancellation, timeLimit, parameters, quiet);
    }

    private Task<bool> StopSessionAsync() => stopTask is { IsCompleted: false } ? stopTask : stopTask = StopSessionCoreAsync();

    private async Task<bool> StopSessionCoreAsync()
    {
        if (busyAction is "Run" or "Restart")
        {
            operationCancellation?.Cancel();
            if (activeTask is not null) await activeTask;
        }
        if (busyAction.Length > 0) return false;
        return await ExecuteAsync("Stop", saveSettings: false);
    }

    private async Task<bool> StartVrAsync()
    {
        if (closing || resourcesReleased || !StartButton.IsEnabled || !readiness.CanStart) { UpdateReadinessUi(); return false; }
        return await ExecuteAsync("Run", steamVr: true);
    }
    private async void Start_Click(object sender, RoutedEventArgs e) => await StartVrAsync();
    private async Task<bool> RestartTrackingAsync()
    {
        if (closing || resourcesReleased || !RestartButton.IsEnabled) return false;
        return await ExecuteAsync("Restart");
    }
    private async void Restart_Click(object sender, RoutedEventArgs e) => await RestartTrackingAsync();
    private async void Tracking_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Run");
    private async void Install_Click(object sender, RoutedEventArgs e) => await BeginInstallationAsync();
    private async void Display_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Display", console: true);
    private async void Doctor_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Doctor");
    private void CancelDiagnostics_Click(object sender, RoutedEventArgs e)
    {
        if (!IsCancellableOperation(busyAction)) return;
        CancelDiagnosticsButton.IsEnabled = false;
        StatusDetail.Text = busyAction is "Usb" or "Detach" ? "Cancelling USB operation…" : "Cancelling check…";
        operationCancellation?.Cancel();
    }
    private async void Refresh_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Status", saveSettings: false);
    private async void Pair_Click(object sender, RoutedEventArgs e)
    {
        if (runtimeMayBeRunning) { StatusDetail.Text = "Stop tracking before changing the controller selection."; return; }
        if (!settings.BluetoothEnabled) { StatusDetail.Text = "Enable Bluetooth in Controllers first."; return; }
        BluetoothControllersPanel.Visibility = Visibility.Visible;
        await ExecuteAsync("BluetoothScan");
    }
    private async void SaveBluetoothControllers_Click(object sender, RoutedEventArgs e)
    {
        string left = LeftBluetoothController.SelectedValue as string ?? "none";
        string right = RightBluetoothController.SelectedValue as string ?? "none";
        if (left == "none" && right == "none") { StatusDetail.Text = "Select at least one controller."; return; }
        if (left == right) { StatusDetail.Text = "Left and right must use different controllers."; return; }
        await ExecuteAsync("Pair");
    }

    private void ApplyBluetoothDevices(JsonElement data)
    {
        if (!data.TryGetProperty("devices", out var devices) || devices.ValueKind != JsonValueKind.Array)
            throw new InvalidDataException("Bluetooth discovery returned no device list.");
        var choices = new List<BluetoothControllerChoice> { BluetoothControllerChoice.None };
        foreach (var device in devices.EnumerateArray())
            choices.Add(new BluetoothControllerChoice(device.GetProperty("address").GetString()!, device.GetProperty("label").GetString()!));
        LeftBluetoothController.ItemsSource = choices;
        RightBluetoothController.ItemsSource = choices;
        string Selected(string side) => data.TryGetProperty("selected", out var selected) && selected.TryGetProperty(side, out var address)
            && address.ValueKind == JsonValueKind.String && choices.Any(x => x.Address == address.GetString()) ? address.GetString()! : "none";
        LeftBluetoothController.SelectedValue = Selected("left");
        RightBluetoothController.SelectedValue = Selected("right");
        Log("[Launcher] Found " + (choices.Count - 1) + " Bluetooth devices. Choose the controller names, then Connect & save.");
    }

    private void Project_RequestNavigate(object sender, System.Windows.Navigation.RequestNavigateEventArgs e)
    {
        try { Process.Start(new ProcessStartInfo("https://github.com/vladoshub/xr-gate") { UseShellExecute = true }); }
        catch (Exception error) { StatusDetail.Text = error.Message; }
        e.Handled = true;
    }

    private void StartDeviceObservation()
    {
        if (smoke || closing || resourcesReleased || busyAction is not ("" or "Run" or "Restart") || configurationProblem is not null || deviceObservation is { IsCompleted: false }) return;
        deviceObservation = ObserveDeviceAsync();
    }

    private async Task ObserveDeviceAsync()
    {
        try
        {
            await DetectHardwareAsync();
            if (closing) return;
            var result = await RunActionAsync("DeviceStatus", cancellation: deviceCancellation.Token, quiet: true);
            if (result.Success && !closing) ApplyDeviceObservation(result.Data);
            else if (!closing) { readiness.DeviceConnected = false; UpdateReadinessUi(); DeviceConnectionStatus.Text = "—"; DeviceConnectionStatus.ToolTip = result.Instruction; }
            // Startup owns its process watch. Do not race a just-created console
            // with a Status poll before it can acquire the Linux runtime lock.
            if (!closing && busyAction.Length == 0 && (runtimeMayBeRunning || !readiness.Known))
            {
                bool wasRunning = runtimeMayBeRunning;
                var runtime = await RunActionAsync("Status", cancellation: deviceCancellation.Token, timeLimit: TimeSpan.FromSeconds(15), quiet: true);
                if (closing || busyAction.Length > 0) return;
                if (runtime.Success) { ApplyStatus(runtime.Data, diagnosticLog: false); UpdateReadinessUi(); }
                if (runtime.Success && wasRunning && !runtimeMayBeRunning)
                {
                    await StopCalibrationProgressAsync(); await StopAudioAsync(); SetBusy(busyAction);
                    StatusTitle.Text = "Tracking stopped";
                    StatusDetail.Text = runtime.Data.TryGetProperty("runtime_exit_message", out var reason) && reason.ValueKind == JsonValueKind.String
                        ? reason.GetString() : "XR Gate exited in Ubuntu. See the activity log; use Stop tracking & return USB before retrying.";
                    if (runtime.Data.TryGetProperty("runtime_log_tail", out var lines) && lines.ValueKind == JsonValueKind.Array)
                        foreach (var line in lines.EnumerateArray()) Log("[Runtime] " + line.GetString());
                    Log("[Launcher] Tracking exited; audio stopped. Use Stop tracking & return USB to return devices.");
                }
            }
            if (MainTabs.SelectedItem == AudioTab && !closing) StartAudioDeviceRefresh();
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (!closing) { readiness.DeviceConnected = false; UpdateReadinessUi(); DeviceConnectionStatus.Text = "—"; DeviceConnectionStatus.ToolTip = error.Message; } }
    }

    private void ApplyDeviceObservation(JsonElement data)
    {
        ObserveDisplayProtection(data);
        if (data.ValueKind == JsonValueKind.Object && data.TryGetProperty("device_status", out var status))
        {
            // Keep compatibility with helpers that report USB attachment as SHARED.
            string? value = status.GetString();
            readiness.DeviceConnected = value is "CONNECTED" or "SHARED" or "TRACKING";
            DeviceConnectionStatus.Text = value == "SHARED" ? "TRACKING" : value;
            DeviceConnectionStatus.ToolTip = data.TryGetProperty("device_status_message", out var message)
                ? message.GetString()?.Replace("SHARED:", "TRACKING:") : null;
        }
        if (data.ValueKind == JsonValueKind.Object)
        {
            if (data.TryGetProperty("display_selected", out var selected)) readiness.DisplaySelected = selected.ValueKind == JsonValueKind.True;
            if (data.TryGetProperty("display_connected", out var connected)) readiness.DisplayConnected = connected.ValueKind == JsonValueKind.True;
            if (data.TryGetProperty("display_connected", out _))
            {
                readiness.DisplayWidth = readiness.DisplayConnected && data.TryGetProperty("display_width", out var width) && width.ValueKind == JsonValueKind.Number && width.TryGetInt32(out int w) && w > 0 ? w : null;
                readiness.DisplayHeight = readiness.DisplayConnected && data.TryGetProperty("display_height", out var height) && height.ValueKind == JsonValueKind.Number && height.TryGetInt32(out int h) && h > 0 ? h : null;
            }
        }
        UpdateReadinessUi();
    }
    private async void Controllers_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Controllers", console: true);
    private async void Driver_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Driver");
    private async void Test_Click(object sender, RoutedEventArgs e) => await ExecuteAsync("Test");
    private async void Stop_Click(object sender, RoutedEventArgs e) => await StopSessionAsync();

    private void SaveSettings_Click(object sender, RoutedEventArgs e)
    {
        if (configurationProblem is not null) { UpdateRecoveryUi(); return; }
        try { settings.Save(root); ControllerSettingsSaved(); StatusDetail.Text = "Settings saved. Advanced settings and device selections are preserved."; Log("Settings saved."); }
        catch (Exception exception) { StatusDetail.Text = exception.Message; Log("ERROR: " + exception.Message); }
    }

    private async void ListUsb_Click(object sender, RoutedEventArgs e)
    {
        if (busyAction.Length > 0 || activeTask is { IsCompleted: false }) return;
        activeTask = ListUsbAsync();
        await activeTask;
    }

    private async Task<bool> ListUsbAsync()
    {
        SetBusy("List USB devices");
        operationCancellation = new CancellationTokenSource();
        StatusTitle.Text = "Listing USB devices…";
        StatusDetail.Text = "Reading connected devices from usbipd. This can take up to 20 seconds.";
        UsbDevicesPanel.Visibility = Visibility.Visible;
        UsbDevicesStatus.Text = StatusDetail.Text;
        UsbDevicesOutput.Clear();
        UsbDevicesPanel.UpdateLayout();
        UsbDevicesPanel.BringIntoView();
        Log("[Launcher] Listing USB devices…");
        try
        {
            string installedTool = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "usbipd-win", "usbipd.exe");
            var result = await ProcessRunner.NativeAsync(File.Exists(installedTool) ? installedTool : "usbipd.exe", ["list"], cancellation: operationCancellation.Token);
            UsbDevicesOutput.Text = result.Output;
            Log(result.Output);
            StatusTitle.Text = result.Code == 0 ? "USB device list ready" : "USB device list needs attention";
            StatusDetail.Text = result.Code == 0
                ? result.Output.Length > 0 ? "Copy a BUSID into the matching USB setting, or use the Bluetooth VID:PID and leave its BUSID empty."
                    : "usbipd returned an empty device list. Reconnect the device and click List USB devices again."
                : "usbipd list failed (exit code " + result.Code + "). See its output below. Run Install if usbipd needs setup.";
            return result.Code == 0;
        }
        catch (OperationCanceledException) { StatusTitle.Text = "USB listing cancelled"; StatusDetail.Text = "You can start another action."; return false; }
        catch (Exception exception)
        {
            StatusTitle.Text = "Could not list USB devices";
            StatusDetail.Text = exception is Win32Exception { NativeErrorCode: 2 or 3 }
                ? "usbipd was not found. Run Install, then click List USB devices again."
                : exception.Message;
            Log("ERROR: " + StatusDetail.Text);
            return false;
        }
        finally
        {
            UsbDevicesStatus.Text = StatusDetail.Text;
            operationCancellation.Dispose();
            operationCancellation = null;
            SetBusy("");
        }
    }

    private void OpenUbuntu(string? distribution = null)
    {
        var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "wsl.exe")) { UseShellExecute = true };
        info.ArgumentList.Add("--distribution");
        info.ArgumentList.Add(distribution ?? settings.Distribution);
        Process.Start(info);
    }
    private void OpenUbuntu_Click(object sender, RoutedEventArgs e)
    {
        try { OpenUbuntu(); } catch (Exception exception) { StatusDetail.Text = exception.Message; }
    }

    private async void NextStep_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (nextStep == "restart_windows") { await OfferWindowsRestartAsync(); return; }
            if (nextStep == "ubuntu_first_login") { OpenUbuntu(pausedDistribution); return; }
            if (nextStep != "restart_ubuntu" || busyAction.Length > 0) return;
            if (!Confirm("Restart the selected Ubuntu distribution? Save any work in it before continuing.")) return;
            SetBusy("Restart Ubuntu");
            keeper.Dispose();
            var result = await ProcessRunner.NativeAsync(Path.Combine(Environment.SystemDirectory, "wsl.exe"), ["--terminate", pausedDistribution]);
            Log(result.Output);
            SetBusy("");
            if (result.Code != 0) throw new IOException("Ubuntu could not restart. Check the activity log.");
            await BeginInstallationAsync(confirmed: true);
        }
        catch (Exception exception) { SetBusy(""); StatusDetail.Text = exception.Message; Log(exception.Message); }
    }

    private void Guide_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string language = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" ? "START-HERE.ru.html" : "START-HERE.html";
            OpenDocumentation(language);
        }
        catch (Exception exception) { StatusDetail.Text = exception.Message; }
    }
    private void Gestures_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string language = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ru" ? "GESTURES.ru.html" : "GESTURES.html";
            OpenDocumentation(language);
        }
        catch (Exception exception) { StatusDetail.Text = exception.Message; }
    }
    private void OpenDocumentation(string name)
    {
        string path = Path.Combine(root, "docs", name);
        // Source checkouts retain the editable documentation at the bundle root.
        if (!File.Exists(path)) path = Path.Combine(root, name);
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    }

    private void Advanced_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            settings.Save(root);
            var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "notepad.exe")) { UseShellExecute = true };
            info.ArgumentList.Add(Path.Combine(root, ".env"));
            Process.Start(info);
            Log("[Launcher] Advanced settings opened. Reopen the launcher after editing the file.");
        }
        catch (Exception exception) { StatusDetail.Text = exception.Message; }
    }
    private void SaveLog_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            FlushLog();
            var dialog = new SaveFileDialog { FileName = "xr-gate-diagnostics-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".txt", Filter = "Text files (*.txt)|*.txt" };
            if (dialog.ShowDialog(this) == true) File.WriteAllText(dialog.FileName, ActivityText(), new UTF8Encoding(false));
        }
        catch (Exception exception) { StatusDetail.Text = exception.Message; }
    }

    private async void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (closeApproved || smoke) { ReleaseResources(); return; }
        if (closing) { e.Cancel = true; return; }
        e.Cancel = true;
        closing = true;
        try
        {
            bool stopped = await ShutdownSessionAsync();
            if (!stopped)
            {
                StatusTitle.Text = "Shutdown needs attention";
                StatusDetail.Text = "Some runtime services or selected USB devices could not be released. The window remains open; check the log and retry Stop tracking & return USB.";
                return;
            }
            closeApproved = true;
            Close();
        }
        catch (Exception error)
        {
            StatusTitle.Text = "Shutdown needs attention";
            StatusDetail.Text = error.Message + " Retry Stop tracking & return USB.";
            Log("[Launcher] Shutdown: " + error.Message);
        }
        finally
        {
            closing = false;
            if (!closeApproved)
            {
                deviceCancellation.Dispose();
                deviceCancellation = new CancellationTokenSource();
                deviceTimer.Start();
            }
        }
    }

    private async Task<bool> ShutdownSessionAsync()
    {
        CancelFirstLoginWait();
        FlushControllerSettings();
        if (controllerSettingsDirty) Log("[Launcher] " + ControllerSaveStatus.Text);
        deviceTimer.Stop();
        deviceCancellation.Cancel();
        if (firstLoginWaitTask is not null) await firstLoginWaitTask;
        await StopCalibrationProgressAsync();
        await StopAudioAsync(required: true);
        if (deviceObservation is not null) await deviceObservation;
        if (audioRefreshTask is not null) await audioRefreshTask;
        if (IsCancellableOperation(busyAction)) await CancelActiveOperationAsync();
        if (busyAction == "Stop" && activeTask is not null) return await activeTask;
        // Let installation/driver setup finish before cleanup; interrupting apt
        // halfway through would leave the next installation unable to continue.
        if (busyAction.Length > 0 && busyAction is not ("Run" or "Restart") && activeTask is not null)
        {
            StatusDetail.Text = "Finishing the current setup step, then stopping tracking and returning USB…";
            await activeTask;
        }
        if (runtimeMayBeRunning || resourcesMayBeOwned || (!smoke && File.Exists(Path.Combine(stateDirectory, "state.json"))))
            return await StopSessionAsync();
        return true;
    }

    private void ReleaseResources()
    {
        if (resourcesReleased) return;
        ReleaseHotkeys();
        displayGuard?.Dispose();
        CancelFirstLoginWait();
        controllerSaveTimer.Stop();
        deviceTimer.Stop();
        deviceCancellation.Cancel();
        audio.Cancel();
        calibrationCancellation?.Cancel();
        keeper.Dispose();
        logTimer.Stop();
        lock (pendingLog) { resourcesReleased = true; sessionLog?.Dispose(); }
        deviceCancellation.Dispose();
    }
}
