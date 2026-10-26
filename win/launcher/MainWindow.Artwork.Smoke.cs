using System.Windows;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private void VerifyArtworkUi()
    {
        if (ReadmeButton.Content?.ToString() != "Readme" || GesturesButton.Content?.ToString() != "Gestures")
            throw new InvalidOperationException("The documentation buttons are missing.");
        bool connected = readiness.DeviceConnected, active = runtimeMayBeRunning;
        string mode = settings.ControllerMode;
        try
        {
            foreach (var state in new[] { (false, false, "gestures"), (true, false, "gestures"),
                (true, true, "gestures"), (true, true, "gearvr"), (true, true, "no"), (false, false, "gearvr") })
            {
                readiness.DeviceConnected = state.Item1;
                runtimeMayBeRunning = state.Item2;
                settings.ControllerMode = state.Item3;
                UpdateHeaderArtwork();
                var expected = HeaderArtwork.Select(root, settings, state.Item1, state.Item2);
                if ((DeviceArtwork.Visibility == Visibility.Visible) != (expected.Device is not null) ||
                    (ControllerArtwork.Visibility == Visibility.Visible) != (expected.Controller is not null))
                    throw new InvalidOperationException("Header artwork did not follow device/runtime/controller state.");
            }
        }
        finally
        {
            readiness.DeviceConnected = connected;
            runtimeMayBeRunning = active;
            settings.ControllerMode = mode;
            UpdateHeaderArtwork();
        }
    }
}
