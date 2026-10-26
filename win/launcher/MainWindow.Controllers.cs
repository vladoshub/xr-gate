using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using System.Windows.Threading;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private readonly DispatcherTimer controllerSaveTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private bool controllerSettingsDirty;
    private Action? smokeControllerSave;

    private void InitializeControllerAutoSave()
    {
        controllerSaveTimer.Tick += (_, _) => FlushControllerSettings();
    }

    private void ControllerSetting_SourceUpdated(object sender, DataTransferEventArgs e)
    {
        if (!IsLoaded || closing || resourcesReleased || configurationProblem is not null) return;
        controllerSettingsDirty = true;
        controllerSaveTimer.Stop();
        ControllerSaveStatus.Text = "Saving changes…";
        ControllerSaveStatus.Foreground = Brushes.LightSteelBlue;
        if (e.TargetObject is TextBox) controllerSaveTimer.Start();
        else FlushControllerSettings();
    }

    private void FlushControllerSettings()
    {
        controllerSaveTimer.Stop();
        if (!controllerSettingsDirty || resourcesReleased || configurationProblem is not null) return;
        try
        {
            if (smoke) smokeControllerSave?.Invoke();
            else settings.SaveControllerSettings(root);
            ControllerSettingsSaved();
        }
        catch (Exception error)
        {
            // Keep the last valid settings on disk while an identifier is incomplete.
            ControllerSaveStatus.Text = "Not saved: " + error.Message;
            ControllerSaveStatus.Foreground = Brushes.LightSalmon;
        }
    }

    private void ControllerSettingsSaved()
    {
        UpdateHeaderArtwork();
        controllerSaveTimer.Stop();
        controllerSettingsDirty = false;
        ControllerSaveStatus.Text = "Changes saved automatically.";
        ControllerSaveStatus.Foreground = Brushes.LightSteelBlue;
    }

    private void ResetControllerAutoSave()
    {
        controllerSaveTimer.Stop();
        controllerSettingsDirty = false;
        ControllerSaveStatus.Text = "Changes are saved automatically.";
        ControllerSaveStatus.Foreground = Brushes.LightSteelBlue;
    }
}
