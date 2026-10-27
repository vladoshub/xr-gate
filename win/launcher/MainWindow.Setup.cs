using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Media.Animation;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private sealed record ActivityEntry(string Text, string Tone = "normal");
    private readonly LauncherReadiness readiness = new();
    private SetupProgress setupProgress = new();
    private string lastNextStepHint = "";
    private string SetupProgressPath => Path.Combine(stateDirectory, "setup-progress.json");
    private Func<string, bool>? smokeConfirmation;

    private bool Confirm(string message) => smoke ? smokeConfirmation?.Invoke(message) == true :
        MessageBox.Show(this, message, "XR Gate", MessageBoxButton.YesNo, MessageBoxImage.Information, MessageBoxResult.No) == MessageBoxResult.Yes;

    private void SaveSetupProgress(SetupProgress progress)
    {
        if (setupProgress == progress) return;
        setupProgress = progress;
        if (!smoke) progress.Save(SetupProgressPath);
    }

    private static void Pulse(Button button, bool enabled)
    {
        if (Equals(button.Tag, enabled)) return;
        button.Tag = enabled;
        button.BeginAnimation(OpacityProperty, enabled ? new DoubleAnimation(1, 0.45, TimeSpan.FromSeconds(0.9))
        { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever } : null);
    }

    private void UpdateReadinessUi()
    {
        readiness.HardwareSelected = settings.HardwareSelected;
        UpdateHeaderArtwork();
        UpdateDisplayProtection();
        bool available = busyAction.Length == 0 && configurationProblem is null && !closing;
        StartButton.IsEnabled = available && !runtimeMayBeRunning && readiness.CanStart;
        StartButton.ToolTip = readiness.CanStart ? "F5: attach USB, start XR Gate tracking and open SteamVR when head tracking is ready." : readiness.NextStep;
        RestartButton.IsEnabled = available && (runtimeMayBeRunning || installed && resourcesMayBeOwned);
        InstallButton.Content = installed ? "Update" : "Install";
        InstallButton.IsEnabled = available && !runtimeMayBeRunning && readiness.CanInstall;
        InstallButton.ToolTip = readiness.CanInstall ? "Install or update WSL, Ubuntu and XR Gate for the selected device. Uses 60 Hz SBS when available; display selection is needed only to start VR." : "Connect a supported device or choose Hardware preset in Settings first.";
        DisplayButton.IsEnabled = available && !runtimeMayBeRunning && readiness.CanSelectDisplay;
        DisplayButton.ToolTip = readiness.CanSelectDisplay ? "Select the glasses display if automatic selection failed. Needed to start VR; installation does not require a display." : "Connect the selected supported XR device to choose its display.";
        DeleteButton.IsEnabled = available && (canDeleteInstallation || installed || setupProgress.Pending);
        Pulse(InstallButton, InstallButton.IsEnabled && !installed);
        Pulse(DisplayButton, DisplayButton.IsEnabled && readiness.NeedsDisplaySelection);
        if (!available || !readiness.Known) return;
        string hint = runtimeMayBeRunning ? "Click Stop tracking & return USB when finished." : readiness.NextStep;
        if (hint == lastNextStepHint) return;
        lastNextStepHint = hint;
        Log("[Next] " + hint);
    }

    private async Task BeginInstallationAsync(bool confirmed = false, bool firstLoginContinuation = false)
    {
        try { await InstallCoreAsync(confirmed, firstLoginContinuation); }
        catch (Exception error)
        {
            StatusTitle.Text = "Setup needs attention"; StatusDetail.Text = error.Message; Log("ERROR: " + error.Message);
            ShowSetupPrompt("INSTALLATION PAUSED. CHECK THE ACTIVITY LOG, THEN CLICK INSTALL TO CONTINUE.");
        }
    }

    private async Task InstallCoreAsync(bool confirmed, bool firstLoginContinuation)
    {
        if (busyAction.Length > 0 || closing || configurationProblem is not null || runtimeMayBeRunning) return;
        if (!readiness.CanInstall) { UpdateReadinessUi(); ShowSetupPrompt("CONNECT A SUPPORTED XR DEVICE OR CHOOSE HARDWARE PRESET IN SETTINGS. THEN CLICK INSTALL."); return; }
        if (!confirmed && !Confirm("XR Gate will install WSL 2, Ubuntu and the required XR components if they are missing. Administrator permission and a Windows restart may be required. Continue?")) return;
        if (!firstLoginContinuation) CancelFirstLoginWait();
        SetupNoticeText.Visibility = Visibility.Collapsed;
        SaveSetupProgress(new(true, settings.Distribution));
        readiness.Installed = false;
        nextStep = "";
        firstLoginSession = null;
        bool success = await ExecuteAsync("Install");
        if (success && installed) { SaveSetupProgress(new()); SetupNoticeText.Visibility = Visibility.Collapsed; }
        else SaveSetupProgress(new(true, settings.Distribution, nextStep));
        if (closing) return;
        if (nextStep == "restart_windows") await OfferWindowsRestartAsync();
        else if (nextStep == "restart_ubuntu")
        {
            ShowSetupPrompt("CLICK RESTART UBUNTU & CONTINUE TO FINISH INSTALLATION.");
            NextStepButton.Content = "Restart Ubuntu & continue";
            NextStepButton.Visibility = Visibility.Visible;
            Log("[Next] Click Restart Ubuntu & continue after saving work in Ubuntu.");
        }
        else if (nextStep == "ubuntu_first_login")
        {
            if (!firstLoginContinuation && firstLoginSession is not null)
            {
                ShowSetupPrompt("CREATE YOUR UBUNTU USERNAME AND PASSWORD IN THE UBUNTU WINDOW. THEN TYPE exit. INSTALLATION WILL CONTINUE AUTOMATICALLY.");
                StartFirstLoginWait(firstLoginSession);
            }
            else ShowSetupPrompt("FINISH CREATING YOUR UBUNTU USERNAME AND PASSWORD, TYPE exit, THEN CLICK INSTALL TO CONTINUE.");
        }
        else if (!success) ShowSetupPrompt("INSTALLATION PAUSED. CHECK THE ACTIVITY LOG, THEN CLICK INSTALL TO CONTINUE.");
    }

    private async Task OfferResumeAsync()
    {
        if (!setupProgress.Pending || configurationProblem is not null || closing) return;
        if (setupProgress.Distribution != settings.Distribution)
        {
            Log("[Next] Installation is unfinished for " + setupProgress.Distribution + ". Select that Ubuntu distribution in Settings to continue.");
            return;
        }
        nextStep = setupProgress.NextStep;
        pausedDistribution = setupProgress.Distribution;
        Log("[Next] Installation is unfinished. Click Install to continue.");
        if (Confirm("XR Gate installation is unfinished. Restart Windows first if it was requested. Continue installing WSL and XR Gate now?"))
            await BeginInstallationAsync(confirmed: true);
    }

    private async Task OfferWindowsRestartAsync()
    {
        ShowSetupPrompt("SAVE YOUR WORK AND RESTART WINDOWS. OPEN XR GATE AFTERWARD TO CONTINUE INSTALLATION.");
        NextStepButton.Content = "Restart Windows";
        NextStepButton.Visibility = Visibility.Visible;
        Log("[Next] Save your work and restart Windows. Open XR Gate afterward to continue installation.");
        if (!Confirm("Windows must restart to continue setup. Save your work in all applications. Restart Windows now?")) return;
        if (!await ShutdownSessionAsync()) { StatusDetail.Text = "Cleanup is incomplete. Retry Stop before restarting Windows."; return; }
        if (smoke) return;
        var result = await ProcessRunner.NativeAsync(Path.Combine(Environment.SystemDirectory, "shutdown.exe"), ["/r", "/t", "0"]);
        if (result.Code != 0)
        {
            deviceCancellation.Dispose(); deviceCancellation = new CancellationTokenSource(); deviceTimer.Start();
            Log("Restart could not be requested: " + result.Output); return;
        }
        closeApproved = true;
        Close();
    }

    private string ActivityText() => new TextRange(LogBox.Document.ContentStart, LogBox.Document.ContentEnd).Text;
    private void FlushLog()
    {
        while (pendingLog.TryDequeue(out var entry))
        {
            var paragraph = new Paragraph(new Run(entry.Text)) { Margin = new Thickness(0, 0, 0, 5) };
            if (entry.Tone != "normal")
            {
                paragraph.FontSize = 18;
                paragraph.FontWeight = FontWeights.Bold;
                paragraph.Foreground = entry.Tone == "success" ? Brushes.LightGreen :
                    entry.Tone == "guidance" ? new SolidColorBrush(Color.FromRgb(255, 180, 84)) :
                    new SolidColorBrush(Color.FromRgb(255, 102, 102));
            }
            LogBox.Document.Blocks.Add(paragraph);
        }
        while (LogBox.Document.Blocks.Count > 1500) LogBox.Document.Blocks.Remove(LogBox.Document.Blocks.FirstBlock);
        LogBox.ScrollToEnd();
    }

    private void LogCalibration(CalibrationNotice notice)
    {
        string line = DateTime.Now.ToString("HH:mm:ss") + "  " + notice.Message;
        lock (pendingLog)
        {
            if (resourcesReleased) return;
            sessionLog?.WriteLine(line);
            pendingLog.Enqueue(new(line, notice.Complete ? "success" : "attention"));
        }
    }
}
