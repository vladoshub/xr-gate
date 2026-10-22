using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace XrGate.Launcher.Core;

public sealed record ActionResult
{
    [JsonRequired, JsonPropertyName("exit_code")] public int ExitCode { get; init; }
    [JsonRequired, JsonPropertyName("next_step")] public string NextStep { get; init; } = "";
    [JsonRequired, JsonPropertyName("message")] public string Message { get; init; } = "";
    [JsonRequired, JsonPropertyName("warnings")] public string[] Warnings { get; init; } = [];
    [JsonRequired, JsonPropertyName("data")] public JsonElement Data { get; init; }
    public bool Success => ExitCode == 0;
    public string Instruction => NextStep switch
    {
        "restart_windows" => "Restart Windows, reopen XR Gate, then click Install / continue.",
        "ubuntu_first_login" => "In the Ubuntu window, create your Linux username and password, type exit, then click Install / continue.",
        "restart_ubuntu" => "Save your work in the selected Ubuntu distribution, then click Restart Ubuntu & continue.",
        _ when ExitCode == 20 => "Installation is paused. Follow the last instructions in the activity log, then click Install / continue.",
        _ when !Success => Message.Length > 0 ? Message : "The action failed. Check the activity log or run Diagnostics.",
        _ => Message
    };

    public static ActionResult Read(string path, int actualExitCode, string? action = null)
    {
        const string hint = " Extract the complete updated Windows archive and check the activity log.";
        if (!File.Exists(path))
            throw new InvalidDataException("PowerShell did not return an action result (exit " + actualExitCode + ")." + hint);
        ActionResult? result;
        try { result = JsonSerializer.Deserialize<ActionResult>(File.ReadAllText(path, Encoding.UTF8)); }
        catch (JsonException error) { throw new InvalidDataException("PowerShell returned an invalid action result." + hint, error); }
        if (result is null || result.NextStep is null || result.Message is null || result.Warnings is null ||
            result.Warnings.Any(warning => warning is null) || result.Data.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("PowerShell returned an incomplete action result." + hint);
        if (result.ExitCode != actualExitCode)
            throw new InvalidDataException("PowerShell result does not match its process exit code." + hint);
        if (action == "Status" && result.Success &&
            (!HasBoolean(result.Data, "installed") || !HasBoolean(result.Data, "running")))
            throw new InvalidDataException("PowerShell returned an invalid runtime status." + hint);
        return result;
    }

    private static bool HasBoolean(JsonElement data, string key) =>
        data.TryGetProperty(key, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False;
}

public static class ScriptCommand
{
    private static readonly HashSet<string> Actions = ["Install", "Run", "Stop", "Status", "DeviceStatus", "BluetoothScan", "Usb", "Detach", "Doctor", "Test", "Driver", "Display", "Pair", "Controllers"];
    public static string Literal(string value) => "'" + value.Replace("'", "''") + "'";
    public static string Encode(string script, string action, string resultPath, bool steamVr, bool console)
    {
        if (!Actions.Contains(action)) throw new ArgumentException("Unknown launcher action.", nameof(action));
        string command = "$OutputEncoding = [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false); " +
            "$env:PYTHONIOENCODING = 'utf-8'; & " + Literal(script) + " -Action " + Literal(action) +
            " -Launcher -ResultPath " + Literal(resultPath) + (steamVr ? " -StartSteamVR" : "") + "; $code = $LASTEXITCODE; ";
        if (console) command += "if ($code -ne 0 -and $code -ne 20) { Read-Host 'Press Enter to return to XR Gate' | Out-Null }; ";
        return Convert.ToBase64String(Encoding.Unicode.GetBytes(command + "exit $code"));
    }
}
