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
    }
}
