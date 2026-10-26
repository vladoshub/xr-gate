using System.Windows;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private string deleteDistribution = "";
    private bool canDeleteInstallation;

    private async void Delete_Click(object sender, RoutedEventArgs e) => await DeleteInstallationAsync();

    private async Task<bool> DeleteInstallationAsync()
    {
        if (busyAction.Length > 0 || activeTask is { IsCompleted: false } || closing) return false;
        try
        {
            var removal = smoke ? new DistributionRemoval("Ubuntu-24.04") :
                DistributionRemoval.Load(Path.Combine(stateDirectory, "state.json"));
            if (!Confirm(removal.Confirmation)) return false;
            CancelFirstLoginWait();
            deleteDistribution = removal.Distribution;
            return await ExecuteAsync("Delete", saveSettings: false);
        }
        catch (Exception error) { StatusTitle.Text = "Deletion needs attention"; StatusDetail.Text = error.Message; Log("ERROR: " + error.Message); return false; }
        finally { deleteDistribution = ""; }
    }

    private void ApplyDeletedInstallation()
    {
        installed = readiness.Installed = runtimeMayBeRunning = resourcesMayBeOwned = canDeleteInstallation = false;
        readiness.Known = true;
        nextStep = "";
        SetupNoticeText.Visibility = Visibility.Collapsed;
        SaveSetupProgress(new());
        audioCatalog.Outputs = audioCatalog.Outputs.Select(item => item with { Available = false }).ToList();
        BindAudioChoices();
        UpdateReadinessUi();
    }
}
