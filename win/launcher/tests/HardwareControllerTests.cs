using System.Text.Json;
using XrGate.Launcher.Core;

internal static class HardwareControllerTests
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    public static void Run(string source, string folder)
    {
        string root = Path.Combine(folder, "hardware-controller-settings");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        File.Copy(Path.Combine(source, ".env.example"), Path.Combine(root, ".env.example"));
        File.Copy(Path.Combine(source, "hardware/xreal_ultra.env"), Path.Combine(root, "hardware/xreal_ultra.env"));
        string env = Path.Combine(root, ".env"), state = Path.Combine(root, "state.json");
        File.WriteAllText(state, JsonSerializer.Serialize(new { GearVR = true, BluetoothInstanceId = "saved-system-adapter" }));
        var settings = LauncherSettings.Load(root, state);
        Check(settings.ControllerMode == "gestures" && !settings.BluetoothEnabled && !settings.ShowCommandPrompt && settings.GlobalHotkeys, "Gestures, Bluetooth-off, hidden-console or enabled hotkey defaults were lost.");
        settings.ControllerMode = "gestures";
        settings.ShowCommandPrompt = true;
        settings.Save(root);
        settings = LauncherSettings.Load(root, state);
        Check(settings.ControllerMode == "gestures" && !settings.GearVr && settings.ShowCommandPrompt,
            "The selected input/console mode did not survive save/reload.");
        Check(File.ReadAllText(env).Contains("XR_GEARVR=0"), "Gestures did not replace the legacy controller flag.");
        Check(File.ReadAllText(env).Contains("XR_BLUETOOTH=0"), "An inherited adapter could re-enable Bluetooth in the PowerShell backend.");
        settings.ControllerMode = "gearvr"; settings.ShowCommandPrompt = false; settings.Save(root);
        Check(LauncherSettings.Load(root, state).GearVr && !LauncherSettings.Load(root, state).ShowCommandPrompt, "Gear VR mode or console-off could not be restored.");
        settings.ControllerMode = "no";
        settings.SaveControllerSettings(root);
        settings = LauncherSettings.Load(root, state);
        Check(settings.ControllerMode == "no" && !settings.GearVr && File.ReadAllText(env).Contains("XR_GEARVR=0"),
            "No controllers did not persist independently of the legacy GearVR state.");
        File.AppendAllText(env, "# Keep advanced values\nXR_START_TIMEOUT=210\n");
        settings.GlobalHotkeys = false;
        settings.FrequencyHz = "unfinished";
        settings.SaveHotkeySettings(root);
        Check(!LauncherSettings.Load(root, state).GlobalHotkeys && File.ReadAllText(env).Contains("XR_START_TIMEOUT=210") &&
            LauncherSettings.Load(root, state).ControllerMode == "no", "Hotkey settings saved unfinished form values or changed controller/advanced settings.");
        settings.GlobalHotkeys = true;
        settings.SaveHotkeySettings(root);
        Check(LauncherSettings.Load(root, state).GlobalHotkeys, "Hotkeys could not be re-enabled and restored.");
        File.WriteAllText(env, "XR_CONTROLLER_MODE=no\nXR_GEARVR=1\n");
        Check(LauncherSettings.Load(root, state).ControllerMode == "no", "A legacy flag overrode the explicit No mode.");
        File.WriteAllText(env, "XR_CONTROLLER_MODE=gestures\nXR_GEARVR=1\n");
        Check(!LauncherSettings.Load(root, state).GearVr, "A legacy flag overrode the explicit Gestures mode.");
        File.WriteAllText(env, "XR_GEARVR=1\nXR_BLUETOOTH=1\n");
        settings = LauncherSettings.Load(root, state);
        Check(settings.GearVr && settings.BluetoothEnabled, "Explicit existing controller/Bluetooth preferences were lost.");

        File.WriteAllText(Path.Combine(root, "hardware/fixture.env"), "XR_DEVICE_NAME=Fixture XR\nXR_USB_IDS=ABCD:0123\n");
        File.WriteAllText(Path.Combine(root, "hardware/multiple.env"), "XR_DEVICE_NAME=Two USB interfaces\nXR_USB_IDS=ABCD:0123,5678:0001\n");
        File.WriteAllText(Path.Combine(root, "hardware/generic.env"), "XR_DEVICE_NAME=Manual only\nXR_USB_IDS=\n");
        File.WriteAllText(Path.Combine(root, "hardware/broken.env"), "XR_USB_IDS=not-a-usb-id\n");
        int warnings = 0;
        var choices = HardwarePresetChoice.Load(root, "hardware/xreal_ultra.env", _ => warnings++);
        Check(choices.Count == 4 && warnings == 1, "Hardware folder enumeration silently used a malformed profile.");
        var ids = HardwarePresetChoice.UsbIdsFromInstances([@"USB\VID_abcd&PID_0123\serial", @"USB\VID_ABCD&PID_0123&MI_00\port", @"PCI\VID_5678&PID_0001\not-usb"]);
        Check(ids.Count == 1 && ids.Contains("abcd:0123"), "USB identities were case/port-dependent or included a non-USB device.");
        Check(HardwarePresetChoice.Matches(choices, ids).Single().Name == "Fixture XR", "Unique hardware VID:PID did not select its profile.");
        Check(HardwarePresetChoice.Matches(choices, HardwarePresetChoice.UsbIdsFromInstances([@"USB\VID_abcd&PID_0123\new-port"])).Single().Name == "Fixture XR", "Replug changed the matching hardware preset.");
        Check(HardwarePresetChoice.Matches(choices, new HashSet<string>()).Count == 0, "A generic/empty-ID profile matched absent hardware.");
        Check(HardwarePresetChoice.Matches(choices, new HashSet<string> { "abcd:0123", "5678:0001" }).Count == 2, "Ambiguous profiles were silently reduced to one match.");

        File.WriteAllText(env, "XR_HARDWARE_ENV=hardware/fixture.env\n");
        settings = LauncherSettings.Load(root, state);
        Check(!settings.AutoDetectHardware, "An existing manually selected preset became automatic.");
        settings.AutoDetectHardware = true; settings.Save(root);
        Check(LauncherSettings.Load(root, state).AutoDetectHardware, "Explicit automatic hardware selection did not persist.");
        settings.AutoDetectHardware = false; settings.Save(root);
        Check(!LauncherSettings.Load(root, state).AutoDetectHardware && LauncherSettings.Load(root, state).HardwareEnv == "hardware/fixture.env",
            "Manual override did not survive restart.");
        Console.WriteLine("OK: controller mode, console setting, hardware enumeration, matching and manual overrides.");
    }
}
