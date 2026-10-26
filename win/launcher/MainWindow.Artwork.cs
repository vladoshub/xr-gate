using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Imaging;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private void UpdateHeaderArtwork()
    {
        var paths = HeaderArtwork.Select(root, settings, readiness.DeviceConnected, runtimeMayBeRunning);
        SetHeaderArtwork(DeviceArtwork, paths.Device, settings.DeviceName);
        SetHeaderArtwork(ControllerArtwork, paths.Controller,
            settings.ControllerMode == "gestures" ? "Hand gestures" : settings.ControllerProfileLabel);
    }

    private static void SetHeaderArtwork(Image image, string? path, string label)
    {
        if (image.Tag as string != path)
        {
            image.Source = null;
            image.Tag = path;
            if (path is not null)
            {
                try
                {
                    var bitmap = new BitmapImage();
                    bitmap.BeginInit();
                    bitmap.CacheOption = BitmapCacheOption.OnLoad;
                    bitmap.DecodePixelWidth = 256;
                    bitmap.UriSource = new Uri(path, UriKind.Absolute);
                    bitmap.EndInit();
                    bitmap.Freeze();
                    image.Source = bitmap;
                }
                catch (Exception error) when (error is IOException or FileFormatException or UnauthorizedAccessException or NotSupportedException or ArgumentException)
                { /* An optional image must not prevent tracking or settings changes. */ }
            }
        }
        image.ToolTip = label;
        System.Windows.Automation.AutomationProperties.SetName(image, label);
        image.Visibility = image.Source is null ? Visibility.Collapsed : Visibility.Visible;
    }
}
