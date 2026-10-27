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
        string mode = settings.ControllerMode, hardware = settings.HardwareEnv;
        try
        {
            string selectedHardware = hardwareChoices.First().Path;
            foreach (var state in new[] { (false, false, "gestures", false), (false, false, "gestures", true),
                (true, false, "gestures", true), (true, true, "gestures", true), (true, true, "gearvr", true),
                (true, true, "no", true), (false, false, "gearvr", true), (false, false, "gearvr", false) })
            {
                readiness.DeviceConnected = state.Item1;
                runtimeMayBeRunning = state.Item2;
                settings.ControllerMode = state.Item3;
                settings.HardwareEnv = state.Item4 ? selectedHardware : "";
                UpdateHeaderArtwork();
                var expected = HeaderArtwork.Select(root, settings, state.Item1, state.Item2);
                if ((DeviceArtwork.Visibility == Visibility.Visible) != (expected.Device is not null) ||
                    (ControllerArtwork.Visibility == Visibility.Visible) != (expected.Controller is not null))
                    throw new InvalidOperationException("Header artwork did not follow device/runtime/controller state.");
                if ((UnselectedArtwork.Visibility == Visibility.Visible) != !state.Item4 ||
                    (DisconnectedArtwork.Visibility == Visibility.Visible) != (state.Item4 && !state.Item1))
                    throw new InvalidOperationException("Unselected and disconnected device artwork did not follow hardware selection and connection state.");
            }
        }
        finally
        {
            readiness.DeviceConnected = connected;
            runtimeMayBeRunning = active;
            settings.ControllerMode = mode;
            settings.HardwareEnv = hardware;
            UpdateHeaderArtwork();
        }
    }
}
