using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private string recoveryHardware = "";
    private void UpdateRecoveryUi()
    {
        RecoveryPanel.Visibility = configurationProblem is null ? Visibility.Collapsed : Visibility.Visible;
        if (configurationProblem is not null)
        {
            recoveryHardware = settings.HardwareEnv;
            RecoveryDetail.Text = configurationProblem.Message + "\nFile: " + configurationProblem.FilePath;
            ResetConfigurationButton.Visibility = configurationProblem.CanReset ? Visibility.Visible : Visibility.Collapsed;
            StatusTitle.Text = "Configuration needs attention";
            StatusDetail.Text = "Repair the file, choose a valid preset or back up and reset the damaged settings. Then reload. Tracking state is unknown; an existing console can be stopped with Ctrl+C.";
            MainTabs.SelectedItem = SettingsTab;
            Log("[Launcher] Configuration recovery: " + configurationProblem.Message);
        }
        SetBusy(busyAction);
    }

    private void ReloadConfiguration(string? hardware = null)
    {
        FlushControllerSettings();
        ResetControllerAutoSave();
        var session = SettingsRecovery.Open(root, hardwareOverride: hardware);
        settings = session.Settings;
        if (hardware is not null) settings.AutoDetectHardware = false;
        configurationProblem = session.Problem;
        DataContext = settings;
        InitializeHardwareUi();
        BindAudioChoices();
        UpdateRecoveryUi();
        ConfigureHotkeys();
        if (configurationProblem is null)
        {
            StatusTitle.Text = "Settings loaded";
            StatusDetail.Text = "Review the settings and save any changes. Refresh status to check the installation.";
        }
    }

    private void HardwarePreset_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (bindingHardwareChoices || busyAction.Length > 0 || HardwarePresetBox.SelectedValue is not string selected) return;
        try
        {
            settings.AutoDetectHardware = false;
            settings.HardwareEnv = selected;
            if (configurationProblem is null) settings.ApplyHardwarePreset(root);
            else if (!string.IsNullOrWhiteSpace(settings.HardwareEnv) && settings.HardwareEnv != recoveryHardware) ReloadConfiguration(settings.HardwareEnv);
            UpdateHeaderArtwork();
            AutoDetectHardwareBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateTarget();
            HardwareDetectionStatus.Text = "Manual preset selected. Automatic detection will not replace it.";
        }
        catch (Exception error) { StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); }
    }

    private void BrowseHardware_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "Hardware configuration (*.env;*.example)|*.env;*.example|All files|*.*", InitialDirectory = root };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            string relative = Path.GetRelativePath(root, dialog.FileName);
            string hardware = relative.StartsWith(".." + Path.DirectorySeparatorChar) ? dialog.FileName : relative;
            if (configurationProblem is not null) ReloadConfiguration(hardware);
            else { settings.AutoDetectHardware = false; settings.HardwareEnv = hardware; settings.ApplyHardwarePreset(root); InitializeHardwareUi(); }
        }
        catch (Exception error) { StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); }
    }

    private void OpenProblem_Click(object sender, RoutedEventArgs e)
    {
        if (configurationProblem is null) return;
        try
        {
            var info = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "notepad.exe")) { UseShellExecute = true };
            info.ArgumentList.Add(configurationProblem.FilePath);
            Process.Start(info);
        }
        catch (Exception error) { StatusDetail.Text = error.Message; }
    }

    private void ReloadSettings_Click(object sender, RoutedEventArgs e) => ReloadConfiguration();

    private void ResetConfiguration_Click(object sender, RoutedEventArgs e)
    {
        if (configurationProblem is null) return;
        try
        {
            string backup = SettingsRecovery.BackUpAndReset(configurationProblem);
            Log("[Launcher] Original settings preserved: " + backup);
            ReloadConfiguration();
            if (configurationProblem is null)
            {
                StatusDetail.Text = "Original file backed up to " + backup + ". Existing tracking and USB were left unchanged; stop any tracking console with Ctrl+C before reinstalling.";
            }
        }
        catch (Exception error) { StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); }
    }
}
