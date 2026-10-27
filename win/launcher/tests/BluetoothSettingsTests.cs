using System.Text.Json;
using XrGate.Launcher.Core;

internal static class BluetoothSettingsTests
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    public static void Run(string source, string temporary)
    {
        string root = Path.Combine(temporary, "bluetooth-settings");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        File.Copy(Path.Combine(source, ".env.example"), Path.Combine(root, ".env.example"));
        string preset = File.ReadAllText(Path.Combine(source, "hardware/xreal_ultra.env"));
        File.WriteAllText(Path.Combine(root, "hardware/xreal_ultra.env"), preset);
        File.WriteAllText(Path.Combine(root, "hardware/custom.env"), preset.Replace("XR_BLUETOOTH_USB_ID=0b05:190e", "XR_BLUETOOTH_USB_ID=1234:abcd"));
        string state = Path.Combine(root, "state.json"), env = Path.Combine(root, ".env");
        var settings = LauncherSettings.Load(root, state);
        settings.HardwareEnv = "hardware/xreal_ultra.env";
        settings.SaveHardwareSelection(root);
        Check(settings.BluetoothUsbId == "0b05:190e" && settings.BluetoothBusId == "", "The profile did not supply automatic ASUS selection.");
        Check(!settings.BluetoothEnabled, "The default adapter identity enabled Bluetooth by itself.");
        settings.HardwareEnv = "hardware/custom.env";
        settings.Save(root);
        Check(settings.BluetoothUsbId == "1234:abcd" && LauncherSettings.Load(root, state).BluetoothUsbId == "1234:abcd", "A hardware profile change did not update the inherited Bluetooth identity.");
        settings.BluetoothUsbId = "ABCD:0123";
        settings.BluetoothEnabled = true;
        settings.HardwareEnv = "hardware/xreal_ultra.env";
        settings.Save(root);
        settings = LauncherSettings.Load(root, state);
        Check(settings.BluetoothUsbId == "ABCD:0123" && settings.BluetoothEnabled, "An explicit Bluetooth override was lost on profile change/save/reload.");
        byte[] before = File.ReadAllBytes(env);
        settings.BluetoothUsbId = "abcd:0123*";
        bool rejected = false;
        try { settings.Save(root); } catch (FormatException) { rejected = true; }
        Check(rejected && File.ReadAllBytes(env).SequenceEqual(before), "Invalid Bluetooth identity changed saved settings.");
        settings.BluetoothUsbId = "";
        settings.Save(root);
        Check(LauncherSettings.Load(root, state).BluetoothUsbId == "", "An explicit empty identity did not disable VID:PID selection.");
        File.WriteAllText(state, JsonSerializer.Serialize(new { BluetoothInstanceId = @"USB\VID_0489&PID_E0E2\SYSTEM" }));
        File.WriteAllText(env, "XR_BLUETOOTH_BUS_ID=2-7\nXR_BLUETOOTH_USB_ID=0b05:190e\n");
        settings = LauncherSettings.Load(root, state);
        settings.BluetoothBusId = "";
        settings.Save(root);
        settings = LauncherSettings.Load(root, state);
        Check(settings.BluetoothBusId == "" && settings.BluetoothUsbId == "0b05:190e", "Clearing a manual BUSID did not retain the configured automatic identity.");
        VerifyControllerAutoSave(root, state, env);
    }

    private static void VerifyControllerAutoSave(string root, string state, string env)
    {
        File.WriteAllText(env, "# Preserve advanced choices\nXR_TCP_PORT=45999\nXR_DISTRIBUTION=Ubuntu-24.04\nXR_AUDIO_OUTPUT=none\nXR_BLUETOOTH_BUS_ID=4-16\nXR_CONTROLLER_CONFIG=old.json\n");
        var settings = LauncherSettings.Load(root, state);
        settings.Distribution = "unfinished distribution";
        settings.FrequencyHz = "";
        settings.AudioOutputId = "unfinished audio output";
        settings.SteamVrPath = "unsaved SteamVR path";
        settings.ControllerMode = "gearvr";
        settings.BluetoothEnabled = true;
        settings.BluetoothBusId = "3-13";
        settings.BluetoothUsbId = "ABCD:0123";
        settings.ControllerConfig = @"C:\Новая папка\O'Brien\controllers.json";
        settings.SaveControllerSettings(root);
        var saved = LauncherSettings.Load(root, state);
        Check(saved.GearVr && saved.BluetoothEnabled && saved.BluetoothBusId == "3-13" &&
            saved.BluetoothUsbId == "ABCD:0123" && saved.ControllerConfig == settings.ControllerConfig,
            "Controller autosave did not persist all controller fields.");
        Check(saved.Distribution == "Ubuntu-24.04" && saved.FrequencyHz == "60" && saved.AudioOutputId == "none" && saved.SteamVrPath == "",
            "Controller autosave changed unrelated unfinished settings.");
        Check(File.ReadAllText(env).Contains("# Preserve advanced choices\nXR_TCP_PORT=45999"),
            "Controller autosave lost advanced settings or comments.");
        byte[] before = File.ReadAllBytes(env);
        foreach (string invalid in new[] { "3-", "3-13,4-16", "3-13\nXR_PRIVATE=1" })
        {
            settings.BluetoothBusId = invalid;
            bool rejected = false;
            try { settings.SaveControllerSettings(root); } catch (FormatException) { rejected = true; }
            Check(rejected && before.SequenceEqual(File.ReadAllBytes(env)), "Incomplete or unsafe BUSID overwrote the last valid settings.");
        }
        settings.BluetoothBusId = "3-13";
        settings.BluetoothUsbId = "abcd:";
        bool identityRejected = false;
        try { settings.SaveControllerSettings(root); } catch (FormatException) { identityRejected = true; }
        Check(identityRejected && before.SequenceEqual(File.ReadAllBytes(env)), "Incomplete VID:PID overwrote the last valid settings.");
        settings.BluetoothUsbId = "ABCD:0123";
        settings.ControllerConfig = "controllers.json\nXR_PRIVATE=1";
        bool pathRejected = false;
        try { settings.SaveControllerSettings(root); } catch (FormatException) { pathRejected = true; }
        Check(pathRejected && before.SequenceEqual(File.ReadAllBytes(env)), "Unsafe controller config overwrote saved settings.");
        settings.ControllerMode = "gestures";
        settings.BluetoothEnabled = false;
        settings.BluetoothBusId = settings.BluetoothUsbId = settings.ControllerConfig = "";
        settings.SaveControllerSettings(root);
        saved = LauncherSettings.Load(root, state);
        Check(!saved.GearVr && !saved.BluetoothEnabled && saved.BluetoothBusId == "" &&
            saved.BluetoothUsbId == "" && saved.ControllerConfig == "", "Clearing controller selections was not saved.");
        before = File.ReadAllBytes(env);
        DateTime unchangedTime = new(2020, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(env, unchangedTime);
        settings.SaveControllerSettings(root);
        Check(before.SequenceEqual(File.ReadAllBytes(env)) && File.GetLastWriteTimeUtc(env) == unchangedTime &&
            Directory.GetFiles(root, ".env.controllers-*").Length == 0, "Unchanged controller autosave rewrote the file or left temporary files.");
        File.WriteAllText(env, "# Inherit the adapter from the preset\n");
        settings = LauncherSettings.Load(root, state);
        settings.ControllerMode = "gearvr";
        settings.SaveControllerSettings(root);
        Check(!LauncherSettings.ReadEnv(env).ContainsKey("XR_BLUETOOTH_USB_ID"), "Autosaving the controller mode pinned an unchanged adapter default.");
        settings = LauncherSettings.Load(root, state);
        settings.HardwareEnv = "hardware/custom.env";
        settings.Save(root);
        Check(LauncherSettings.Load(root, state).BluetoothUsbId == "1234:abcd", "Controller autosave blocked adapter inheritance from a new preset.");
    }
}
