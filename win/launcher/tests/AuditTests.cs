using System.Text.Json;
using XrGate.Launcher.Core;

internal static class AuditTests
{
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }

    public static void Run(string source, string temporary)
    {
        string root = Path.Combine(temporary, "audit");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        File.Copy(Path.Combine(source, ".env.example"), Path.Combine(root, ".env.example"));
        string preset = File.ReadAllText(Path.Combine(source, "hardware/xreal_ultra.env"));
        File.WriteAllText(Path.Combine(root, "hardware/xreal_ultra.env"), preset);
        File.WriteAllText(Path.Combine(root, "hardware/custom.env"), preset
            .Replace("XR_DISPLAY_FREQUENCY_HZ=60", "XR_DISPLAY_FREQUENCY_HZ=72")
            .Replace("XR_DISPLAY_MODE=extended_sbs", "XR_DISPLAY_MODE=direct")
            .Replace("XR_DISPLAY_NAME=", "XR_DISPLAY_NAME=DISPLAY8")
            .Replace("XR_CONTROLLER_PROFILE_LABEL=Gear VR controllers", "XR_CONTROLLER_PROFILE_LABEL=Custom controller"));
        string state = Path.Combine(root, "state.json");
        var settings = LauncherSettings.Load(root, state);
        bool bindingNotified = false;
        settings.PropertyChanged += (_, _) => bindingNotified = true;
        settings.HardwareEnv = "hardware/custom.env";
        settings.Save(root);
        var loaded = LauncherSettings.Load(root, state);
        Check(loaded.FrequencyHz == "72" && loaded.DisplayMode == "direct" && loaded.DisplayName == "DISPLAY8", "Changing the hardware preset pinned old defaults.");
        Check(settings.ControllerProfileLabel == "Custom controller" && bindingNotified, "The selected profile did not update live labels/bindings.");
        loaded.Save(root);
        loaded = LauncherSettings.Load(root, state);
        Check(loaded.HardwareEnv == "hardware/custom.env" && loaded.DisplayMode == "direct", "A repeated save forgot the selected hardware preset.");
        loaded.FrequencyHz = "90";
        loaded.HardwareEnv = "hardware/xreal_ultra.env";
        loaded.Save(root);
        Check(LauncherSettings.Load(root, state).FrequencyHz == "90", "A deliberate manual frequency override was lost on a profile change.");
        File.WriteAllText(state, JsonSerializer.Serialize(new { GearVR = true, BluetoothInstanceId = "saved-dongle", FrequencyHz = 90 }));
        loaded = LauncherSettings.Load(root, state);
        loaded.GearVr = loaded.BluetoothEnabled = false;
        loaded.FrequencyHz = "60";
        loaded.AutoDriverFrequency = false;
        loaded.Save(root);
        loaded = LauncherSettings.Load(root, state);
        Check(!loaded.GearVr && !loaded.BluetoothEnabled && loaded.FrequencyHz == "60", "Disabling a saved controller or choosing the profile's default frequency did not override cached choices.");

        byte[] broken = "{broken json"u8.ToArray();
        File.WriteAllBytes(state, broken);
        var session = SettingsRecovery.Open(root, state);
        Check(session.Problem is { CanReset: true } && session.Problem.FilePath == state, "Corrupt saved state did not enter recovery.");
        Check(File.ReadAllBytes(state).SequenceEqual(broken), "Entering recovery overwrote the broken state.");
        string backup = SettingsRecovery.BackUpAndReset(session.Problem!);
        Check(!File.Exists(state) && File.ReadAllBytes(backup).SequenceEqual(broken), "Recovery did not preserve the exact original file.");
        Check(SettingsRecovery.Open(root, state).Problem is null, "Settings could not reload after state recovery.");
        string env = Path.Combine(root, ".env");
        File.WriteAllText(env, "XR_HARDWARE_ENV=hardware/missing.env\n");
        session = SettingsRecovery.Open(root, state);
        Check(session.Problem is { CanReset: false }, "A missing hardware preset was silently reset.");
        Check(SettingsRecovery.Open(root, state, "hardware/custom.env").Problem is null, "Choosing another preset could not recover a missing preset.");
        File.WriteAllText(env, "not an env line\n");
        session = SettingsRecovery.Open(root, state);
        Check(session.Problem is { CanReset: true } && session.Problem.FilePath == env, "Malformed env did not open file-specific recovery.");

        string result = Path.Combine(root, "result.json");
        string valid = "{\"exit_code\":0,\"next_step\":\"\",\"message\":\"ok\",\"warnings\":[],\"data\":{\"installed\":true,\"running\":false}}";
        string?[] invalid = [null, "{}", "null", "{broken", valid.Replace("\"warnings\":[]", "\"warnings\":null"),
            valid.Replace("\"installed\":true", "\"installed\":\"true\"")];
        foreach (string? payload in invalid)
        {
            File.Delete(result);
            if (payload is not null) File.WriteAllText(result, payload);
            bool rejected = false;
            try { ActionResult.Read(result, 0, "Status"); } catch (InvalidDataException) { rejected = true; }
            Check(rejected, "An invalid/missing action result was accepted: " + payload);
        }
        File.WriteAllText(result, valid);
        Check(ActionResult.Read(result, 0, "Status").Success, "A valid action result was rejected.");
    }
}
