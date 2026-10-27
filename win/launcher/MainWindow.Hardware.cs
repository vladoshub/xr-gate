using System.Windows;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private bool bindingHardwareChoices;
    private List<HardwarePresetChoice> hardwareChoices = [];
    private IReadOnlySet<string> connectedHardwareIds = new HashSet<string>();
    private void InitializeHardwareUi()
    {
        bindingHardwareChoices = true;
        try
        {
            hardwareChoices = HardwarePresetChoice.Load(root, settings.HardwareEnv, message => pendingLog.Enqueue(new("[Hardware] " + message)));
            HardwarePresetBox.ItemsSource = new[] { HardwarePresetChoice.None }.Concat(hardwareChoices).ToList();
            HardwarePresetBox.SelectedValue = hardwareChoices.FirstOrDefault(choice => HardwarePresetChoice.SamePath(root, choice.Path, settings.HardwareEnv))?.Path ?? "";
        }
        finally { bindingHardwareChoices = false; }
        UpdateHardwarePresence();
        UpdateHeaderArtwork();
    }

    private void UpdateHardwarePresence()
    {
        var selected = hardwareChoices.FirstOrDefault(choice => HardwarePresetChoice.SamePath(root, choice.Path, settings.HardwareEnv));
        readiness.HardwareConnected = selected is not null && selected.UsbIds.Count > 0 && selected.UsbIds.All(connectedHardwareIds.Contains);
    }

    private async void AutoDetectHardware_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!smoke) settings.SaveHardwareSelection(root);
            if (settings.AutoDetectHardware) await DetectHardwareAsync();
            else HardwareDetectionStatus.Text = "Manual selection: choose a preset from the list.";
        }
        catch (Exception error) { StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); }
    }

    private async Task DetectHardwareAsync()
    {
        if (smoke || closing || runtimeMayBeRunning || resourcesMayBeOwned || configurationProblem is not null) return;
        try
        {
            var ids = await Task.Run(HardwareDiscovery.ConnectedUsbIds, deviceCancellation.Token);
            if (closing || runtimeMayBeRunning || resourcesMayBeOwned || busyAction.Length > 0) return;
            connectedHardwareIds = ids;
            ApplyHardwareDetection(ids);
            UpdateHardwarePresence();
            UpdateReadinessUi();
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { HardwareDetectionStatus.Text = error.Message + " You can choose a preset manually."; }
    }

    private void ApplyHardwareDetection(IReadOnlySet<string> ids)
    {
        if (!settings.AutoDetectHardware) return;
        var matches = HardwarePresetChoice.Matches(hardwareChoices, ids);
        if (matches.Count != 1)
        {
            if (matches.Count > 1 && settings.HardwareSelected)
            {
                settings.HardwareEnv = ""; settings.ApplyHardwarePreset(root);
                if (!smoke) settings.SaveHardwareSelection(root);
                installed = readiness.Installed = readiness.Known = readiness.DeviceConnected = readiness.DisplaySelected = readiness.DisplayConnected = false;
                InitializeHardwareUi(); UpdateReadinessUi();
            }
            HardwareDetectionStatus.Text = matches.Count == 0 ? "No matching connected device. Choose any preset manually, or connect the device."
                : "Multiple presets match: " + string.Join(", ", matches.Select(choice => choice.Label)) + ". Choose one manually.";
            return;
        }
        var selected = matches[0];
        if (!HardwarePresetChoice.SamePath(root, selected.Path, settings.HardwareEnv))
        {
            settings.HardwareEnv = selected.Path;
            settings.ApplyHardwarePreset(root);
            if (!smoke) settings.SaveHardwareSelection(root);
            installed = readiness.Installed = readiness.Known = readiness.DeviceConnected = readiness.DisplaySelected = readiness.DisplayConnected = false;
            InitializeHardwareUi();
            Log("[Hardware] Detected " + selected.Label + " from XR_USB_IDS.");
        }
        HardwareDetectionStatus.Text = "Detected: " + selected.Label + ". Choose another preset to keep a manual selection.";
        UpdateReadinessUi();
    }
}
