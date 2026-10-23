using System.Windows;
using System.Windows.Controls;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private readonly AudioBridge audio;
    private AudioDeviceCatalog audioCatalog = new();
    private Task? audioRefreshTask;
    private bool bindingAudioChoices;
    private string AudioCatalogFile => Path.Combine(stateDirectory, "audio-devices.json");
    private void InitializeAudioUi()
    {
        if (!smoke)
        {
            try
            {
                audioCatalog = AudioDeviceCatalog.Load(AudioCatalogFile);
                audioCatalog.Sources = audioCatalog.Sources.Select(item => item with { Available = false }).ToList();
                audioCatalog.Outputs = audioCatalog.Outputs.Select(item => item with { Available = false }).ToList();
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            { Log("[Audio] Could not read remembered device names: " + error.Message); audioCatalog = new(); }
        }
        BindAudioChoices();
    }
    private void BindAudioChoices()
    {
        bindingAudioChoices = true;
        try
        {
            audioCatalog.Sources = AudioDeviceCatalog.Merge(audioCatalog.Sources, audioCatalog.Sources, settings.AudioSourceId, source: true);
            audioCatalog.Outputs = AudioDeviceCatalog.Merge(audioCatalog.Outputs, audioCatalog.Outputs, settings.AudioOutputId, source: false);
            WindowsAudioSource.ItemsSource = audioCatalog.Sources; WslAudioOutput.ItemsSource = audioCatalog.Outputs;
            WindowsAudioSource.SelectedValue = string.IsNullOrEmpty(settings.AudioSourceId) ? "none" : settings.AudioSourceId;
            WslAudioOutput.SelectedValue = string.IsNullOrEmpty(settings.AudioOutputId) ? "none" : settings.AudioOutputId;
        }
        finally { bindingAudioChoices = false; }
    }
    private void ApplyAudioDevices(IEnumerable<AudioDeviceChoice> sources, IEnumerable<AudioDeviceChoice> outputs)
    {
        audioCatalog.Sources = AudioDeviceCatalog.Merge(audioCatalog.Sources, sources, settings.AudioSourceId, source: true);
        audioCatalog.Outputs = AudioDeviceCatalog.Merge(audioCatalog.Outputs, outputs, settings.AudioOutputId, source: false);
        BindAudioChoices();
    }
    private void StartAudioDeviceRefresh()
    {
        if (smoke || closing || resourcesReleased || configurationProblem is not null || audioRefreshTask is { IsCompleted: false }) return;
        audioRefreshTask = RefreshAudioDevicesAsync();
    }
    private async Task RefreshAudioDevicesAsync()
    {
        RefreshAudioDevicesButton.IsEnabled = false;
        string distribution = settings.Distribution;
        try
        {
            var sources = new List<AudioDeviceChoice>();
            var outputs = new List<AudioDeviceChoice>();
            string detail = "";
            try { sources = await Task.Run(AudioBridge.Sources, deviceCancellation.Token); }
            catch (Exception error) when (error is not OperationCanceledException) { detail = "Windows audio: " + error.Message; }
            if (installed)
            {
                try
                {
                    var loaded = LauncherSettings.Load(root);
                    string user = loaded.Distribution == distribution ? loaded.LinuxUser : "";
                    outputs = await new WslAudioClient(root, distribution, user).ListAsync(deviceCancellation.Token);
                }
                catch (Exception error) when (error is not OperationCanceledException) { detail = "WSL audio: " + error.Message; }
            }
            else detail = "Install XR Gate, start tracking, then refresh this list.";
            if (closing || distribution != settings.Distribution) return;
            ApplyAudioDevices(sources, outputs);
            try { audioCatalog.Save(AudioCatalogFile); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Log("[Audio] Could not save device names: " + error.Message); }
            AudioStatus.Text = detail.Length > 0 ? detail : outputs.Count == 0
                ? "No ALSA playback outputs found. Attach a USB device with audio, then refresh."
                : "Device list refreshed. Selections are saved automatically; disconnected selections are kept.";
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (!closing) AudioStatus.Text = error.Message; Log("[Audio] " + error.Message); }
        finally { if (!closing) RefreshAudioDevicesButton.IsEnabled = busyAction.Length == 0 && configurationProblem is null; }
    }
    private void RefreshAudioDevices_Click(object sender, RoutedEventArgs e) => StartAudioDeviceRefresh();
    private async void AudioSelection_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (bindingAudioChoices || smoke || closing || configurationProblem is not null) return;
        string source = settings.AudioSourceId, output = settings.AudioOutputId;
        if (sender == WindowsAudioSource && WindowsAudioSource.SelectedValue is string sourceId) settings.AudioSourceId = sourceId;
        else if (sender == WslAudioOutput && WslAudioOutput.SelectedValue is string outputId) settings.AudioOutputId = outputId;
        else return;
        try
        {
            settings.SaveAudioSelection(root);
            await UpdateAudioAsync(runtimeMayBeRunning);
            AudioStatus.Text = settings.AudioOutputId == "none" || settings.AudioSourceId == "none" ? "Audio is off. Selection saved."
                : runtimeMayBeRunning ? "Selection saved. Audio connects to these devices; see the activity log."
                : "Selection saved. Audio starts with Start VR or Start tracking.";
        }
        catch (Exception error)
        {
            settings.AudioSourceId = source; settings.AudioOutputId = output; BindAudioChoices();
            AudioStatus.Text = error.Message; Log("[Audio] " + error.Message);
        }
    }
    private async Task UpdateAudioAsync(bool running)
    {
        if (smoke) return;
        if (!running || closing) { await StopAudioAsync(); return; }
        try
        {
            var loaded = LauncherSettings.Load(root);
            await audio.ConfigureAsync(new AudioRoute(loaded.Distribution, loaded.LinuxUser,
                loaded.AudioSourceId, loaded.AudioOutputId), deviceCancellation.Token);
        }
        catch (OperationCanceledException) when (closing) { }
        catch (Exception error) { Log("[Audio] " + error.Message); AudioStatus.Text = error.Message; }
    }
    private async Task StopAudioAsync(bool required = false)
    {
        audio.Cancel();
        try { await audio.ConfigureAsync(null); }
        catch (Exception error) when (!required) { Log("[Audio] Stop: " + error.Message); }
    }
}
