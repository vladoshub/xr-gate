using System.Diagnostics;
using System.Windows;
using System.Windows.Input;
using System.Windows.Navigation;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class AboutWindow : Window
{
    public AboutWindow(string root, string runtimeVersion, string launcherVersion)
    {
        InitializeComponent();
        UpdateRuntimeVersion(runtimeVersion);
        LauncherVersionText.Text = launcherVersion;
        try
        {
            var devices = HardwarePresetChoice.Load(root, "").Select(choice => choice.Label).ToArray();
            SupportedDevicesList.ItemsSource = devices;
            if (devices.Length == 0) ShowDeviceNotice("No hardware presets found.");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { ShowDeviceNotice("Device list unavailable."); }
    }

    public void UpdateRuntimeVersion(string version) => RuntimeVersionText.Text = version;

    private void ShowDeviceNotice(string message)
    {
        SupportedDevicesNotice.Text = message;
        SupportedDevicesNotice.Visibility = Visibility.Visible;
    }

    private void Email_RequestNavigate(object sender, RequestNavigateEventArgs e)
    {
        e.Handled = true;
        try { Process.Start(new ProcessStartInfo("mailto:vladosyarovenko@gmail.com") { UseShellExecute = true }); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            ContactErrorText.Text = "Could not open a mail app. Email: vladosyarovenko@gmail.com";
            ContactErrorText.Visibility = Visibility.Visible;
        }
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
    private void Close_Executed(object sender, ExecutedRoutedEventArgs e) => Close();
}
