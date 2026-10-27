using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private DeviceDisplayGuard? displayGuard;
    private string protectedDisplayName = "", protectedDisplayId = "", protectedHardware = "";
    private bool displayProtectionRequested;

    private void InitializeDisplayProtection()
    {
        if (!smoke)
            displayGuard = new(message =>
            {
                Log("[Display protection] " + message);
                if (!Dispatcher.HasShutdownStarted)
                    Dispatcher.BeginInvoke(new Action(() =>
                    {
                        if (!resourcesReleased && displayProtectionRequested) DisplayProtectionStatus.Text = message;
                    }));
            });
        UpdateDisplayProtection();
    }

    private void ObserveDisplayProtection(JsonElement data)
    {
        if (data.ValueKind != JsonValueKind.Object || !data.TryGetProperty("display_connected", out var connected)) return;
        protectedDisplayName = protectedDisplayId = protectedHardware = "";
        if (connected.ValueKind != JsonValueKind.True || !data.TryGetProperty("display_selected", out var selected) || selected.ValueKind != JsonValueKind.True) return;
        if (!data.TryGetProperty("display_name", out var name) || name.ValueKind != JsonValueKind.String ||
            !data.TryGetProperty("display_device_id", out var id) || id.ValueKind != JsonValueKind.String) return;
        protectedDisplayName = name.GetString() ?? "";
        protectedDisplayId = id.GetString() ?? "";
        protectedHardware = settings.HardwareEnv;
    }

    private void UpdateDisplayProtection()
    {
        if (!HardwarePresetChoice.SamePath(root, protectedHardware, settings.HardwareEnv))
            protectedDisplayName = protectedDisplayId = protectedHardware = "";
        string reason = !settings.ProtectDeviceDisplay ? "Device display protection is disabled." :
            !runtimeMayBeRunning ? "Protection is idle until tracking starts." :
            settings.DisplayMode != "extended_sbs" ? "Protection applies to extended SBS displays only." :
            configurationProblem is not null || !settings.HardwareSelected || !readiness.DisplaySelected || !readiness.DisplayConnected ||
                protectedDisplayName.Length == 0 || protectedDisplayId.Length == 0
                ? "Waiting for a selected, connected secondary XR display." : "";
        bool wasRequested = displayProtectionRequested;
        displayProtectionRequested = reason.Length == 0;
        displayGuard?.Configure(displayProtectionRequested ? protectedDisplayName : "", displayProtectionRequested ? protectedDisplayId : "");
        if (!displayProtectionRequested) DisplayProtectionStatus.Text = reason;
        else if (!wasRequested) DisplayProtectionStatus.Text = "Enabling protection for " + protectedDisplayName + "…";
    }

    private void ProtectDeviceDisplay_Click(object sender, RoutedEventArgs e)
    {
        ProtectDeviceDisplayBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateSource();
        UpdateDisplayProtection();
        try { if (!smoke) settings.SaveDisplayProtection(root); }
        catch (Exception error)
        {
            DisplayProtectionStatus.Text += " Could not save this setting: " + error.Message;
            Log("[Display protection] Could not save setting: " + error.Message);
        }
    }
}
