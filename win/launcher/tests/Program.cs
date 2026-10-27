using System.Text;
using System.Text.Json;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

static void Check(bool value, string message) { if (!value) throw new Exception(message); }
static void MustReject(Action action, string message)
{
    try { action(); } catch (FormatException) { return; }
    throw new Exception(message);
}

if (args.Length > 0 && args[0] == "--audio-receiver-fixture")
{
    Console.OutputEncoding = new UTF8Encoding(false);
    Console.WriteLine("ready");
    if (args[1] == "hang") await Task.Delay(Timeout.Infinite);
    using var input = Console.OpenStandardInput();
    if (args[1] is "ack" or "badack")
    {
        using var output = Console.OpenStandardOutput();
        var prefix = new byte[4];
        while (await input.ReadAsync(prefix.AsMemory(0, 1)) > 0)
        {
            await input.ReadExactlyAsync(prefix.AsMemory(1));
            int length = System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(prefix);
            await input.ReadExactlyAsync(new byte[length]);
            if (args[1] == "badack") Array.Clear(prefix);
            await output.WriteAsync(prefix); await output.FlushAsync();
        }
        return;
    }
    var block = new byte[4096]; int size, total = 0;
    while ((size = await input.ReadAsync(block)) > 0) total += size;
    Console.WriteLine(total);
    return;
}
string source = args.Length > 0 ? Path.GetFullPath(args[0]) : throw new ArgumentException("Pass the win source directory.");
string folder = Path.Combine(Path.GetTempPath(), "xr-gate-launcher-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(folder);
try
{
    SetupTests.Run(folder);
    Directory.CreateDirectory(Path.Combine(folder, "hardware"));
    File.Copy(Path.Combine(source, ".env.example"), Path.Combine(folder, ".env.example"));
    File.Copy(Path.Combine(source, "hardware/xreal_ultra.env"), Path.Combine(folder, "hardware/xreal_ultra.env"));
    string env = Path.Combine(folder, ".env"), state = Path.Combine(folder, "state.json");
    File.WriteAllText(state, JsonSerializer.Serialize(new { FrequencyHz = 90, GearVR = true, BluetoothInstanceId = "saved-dongle", Distribution = "Ubuntu-24.04" }));
    File.WriteAllText(env, "# My advanced configuration\nXR_TCP_PORT=45999\nXR_GEARVR=1\nXR_BLUETOOTH=1\nXR_STEAMVR_PATH=\"D:\\Games O'Brien\\SteamVR\"\n");
    var settings = LauncherSettings.Load(folder, state);
    Check(settings.FrequencyHz == "90" && settings.GearVr && settings.BluetoothEnabled, "Saved hardware/controller selections were lost.");
    Check(settings.SteamVrPath == @"D:\Games O'Brien\SteamVR", "Quoted Windows path did not survive parsing.");
    settings.BluetoothBusId = "4-16";
    settings.Save(folder);
    string content = File.ReadAllText(env);
    Check(content.Contains("# My advanced configuration") && content.Contains("XR_TCP_PORT=45999"), "GUI save overwrote advanced settings/comments.");
    settings = LauncherSettings.Load(folder, state);
    Check(settings.SteamVrPath == @"D:\Games O'Brien\SteamVR" && settings.BluetoothBusId == "4-16", "Settings failed the save/reload round trip.");
    byte[] before = File.ReadAllBytes(env);
    settings.UsbBusIds = "2-2\nXR_GEARVR=0";
    MustReject(() => settings.Save(folder), "Multiline settings injection was accepted.");
    Check(before.SequenceEqual(File.ReadAllBytes(env)), "Validation failure modified the existing settings file.");
    settings.UsbBusIds = "2-2,4-16";
    settings.FrequencyHz = "0";
    MustReject(() => settings.Save(folder), "Zero Hz was accepted.");
    var updated = LauncherSettings.UpdateEnv("XR_GEARVR=0\n# keep\nXR_GEARVR=1\nXR_PRIVATE=value\n",
        new Dictionary<string, string> { ["XR_GEARVR"] = "0" });
    Check(updated.Split("XR_GEARVR=").Length == 2 && updated.Contains("XR_PRIVATE=value"), "Duplicate overrides or advanced settings were handled incorrectly.");
    File.AppendAllText(env, "XR_DISPLAY_FREQUENCY_HZ=72\n");
    Check(LauncherSettings.Load(folder, state).FrequencyHz == "72", "Explicit env selection did not override saved frequency.");
    string result = Path.Combine(folder, "result.json");
    File.WriteAllText(result, "{\"exit_code\":20,\"next_step\":\"restart_windows\",\"message\":\"\",\"warnings\":[],\"data\":{}}");
    Check(ActionResult.Read(result, 20).Instruction.Contains("Restart Windows"), "Setup pause was presented as an unexplained error.");
    bool staleRejected = false;
    try { ActionResult.Read(result, 1); } catch (InvalidDataException) { staleRejected = true; }
    Check(staleRejected, "Stale success/setup metadata masked a crashed process.");
    AuditTests.Run(source, folder);
    BluetoothSettingsTests.Run(source, folder);
    HardwareControllerTests.Run(source, folder);
    InstallSourceTests.Run(source, folder);
    ArtworkTests.Run(folder);
    DisplayProtectionTests.Run(folder);
    await AudioTests.RunAsync(source, folder);
    string encoded = ScriptCommand.Encode(@"C:\O'Brien\XR Gate\XrGate-Wsl.ps1", "Run", @"C:\result.json", true, false);
    string command = Encoding.Unicode.GetString(Convert.FromBase64String(encoded));
    Check(command.Contains("O''Brien") && command.Contains("-StartSteamVR") && command.Contains("-ResultPath 'C:\\result.json'"), "Paths or action arguments were not passed literally.");
    bool rejected = false;
    try { ScriptCommand.Encode("test", "Run; exit 0", result, false, false); } catch (ArgumentException) { rejected = true; }
    Check(rejected, "An unknown executable action was accepted.");
    string? testPowerShell = Environment.GetEnvironmentVariable("XR_GATE_TEST_POWERSHELL");
    if (OperatingSystem.IsWindows() || !string.IsNullOrWhiteSpace(testPowerShell))
    {
        string bridge = Path.Combine(folder, "O'Brien новая папка");
        Directory.CreateDirectory(bridge);
        string script = "param([string]$Action,[switch]$Launcher,[string]$ResultPath)\n" +
            "$OutputEncoding = [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)\n" +
            "Write-Output 'Привет из PowerShell'\n" +
            "Write-Host 'Сообщение установщика'\n" +
            "Write-Warning 'Проверка предупреждения'\n" +
            "Write-Error 'Проверка сообщения об ошибке' -ErrorAction Continue\n" +
            "[IO.File]::WriteAllText($ResultPath, '{\"exit_code\":0,\"next_step\":\"\",\"message\":\"bridge OK\",\"warnings\":[],\"data\":{\"installed\":false,\"running\":false}}', (New-Object Text.UTF8Encoding($false)))\nexit 0\n";
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), script, new UTF8Encoding(true));
        var logs = new List<string>();
        var runner = new ProcessRunner(bridge, folder, line => { lock (logs) logs.Add(line); }, testPowerShell);
        var bridged = await runner.RunAsync("Doctor");
        Check(bridged.Success && bridged.Message == "bridge OK", "GUI-to-PowerShell process/result bridge failed.");
        Check(logs.Any(line => line.Contains("Привет из PowerShell")), "Redirected PowerShell UTF-8 output was corrupted.");
        Check(logs.Any(line => line.Contains("Сообщение установщика")) &&
              logs.Any(line => line.Contains("Проверка предупреждения")) &&
              logs.Any(line => line.Contains("Проверка сообщения об ошибке")), "A PowerShell host/warning/error message was lost or its Cyrillic text was corrupted.");
        Check(!logs.Any(line => line.Contains("CLIXML") || line.Contains("<Objs") || line.Contains("<ToString>")), "Raw PowerShell CLIXML was displayed in the activity log.");
        int beforeQuiet = logs.Count;
        Check((await runner.RunAsync("Status", quiet: true)).Success && logs.Count == beforeQuiet,
            "Read-only background status flooded the activity log or lost its structured result.");
        string deletionFixture = "param([string]$Action,[switch]$Launcher,[string]$ResultPath,[string]$DeleteDistributionName)\n" +
            "if ($Action -ne 'Delete' -or $DeleteDistributionName -cne 'Ubuntu-24.04') { throw 'Wrong confirmed distribution' }\n" +
            "[IO.File]::WriteAllText($ResultPath, '{\"exit_code\":0,\"next_step\":\"\",\"message\":\"fixture only\",\"warnings\":[],\"data\":{\"deleted\":true}}')\nexit 0\n";
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), deletionFixture, new UTF8Encoding(true));
        Check((await runner.RunAsync("Delete", parameters: new Dictionary<string, string> { ["DeleteDistributionName"] = "Ubuntu-24.04" })).Data.GetProperty("deleted").GetBoolean(),
            "The process bridge lost the exact confirmed Ubuntu deletion target.");
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), script.Replace("exit 0", "Start-Sleep -Seconds 30\nexit 0"), new UTF8Encoding(true));
        bool timedOut = false;
        try { await runner.RunAsync("Doctor", timeLimit: TimeSpan.FromSeconds(2)).WaitAsync(TimeSpan.FromSeconds(8)); }
        catch (OperationCanceledException) { timedOut = true; }
        Check(timedOut, "A hung Doctor did not reach its time limit.");
        using (var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
        {
            bool cancelled = false;
            try { await runner.RunAsync("Doctor", cancellation: cancel.Token).WaitAsync(TimeSpan.FromSeconds(8)); }
            catch (OperationCanceledException) { cancelled = true; }
            Check(cancelled, "A hung Doctor ignored cancellation.");
        }
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), script, new UTF8Encoding(true));
        Check((await runner.RunAsync("Status")).Success, "The launcher could not run another action after cancelling Doctor.");
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), "param([string]$Action,[switch]$Launcher,[string]$ResultPath)\nexit 0\n");
        bool emptyRejected = false;
        try { await runner.RunAsync("Doctor"); } catch (InvalidDataException) { emptyRejected = true; }
        Check(emptyRejected, "A process that returned no JSON was reported as successful.");
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), script.Replace("exit 0", "Start-Sleep -Seconds 30\nexit 0"), new UTF8Encoding(true));
        bool testCancelled = false;
        using (var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
        {
            try { await runner.RunAsync("Test", cancellation: cancel.Token).WaitAsync(TimeSpan.FromSeconds(8)); }
            catch (OperationCanceledException) { testCancelled = true; }
        }
        Check(testCancelled, "A hung live-tracking check ignored cancellation.");
        File.WriteAllText(Path.Combine(bridge, "XrGate-Wsl.ps1"), script, new UTF8Encoding(true));
        Check((await runner.RunAsync("Status")).Success, "A cancelled tracking check prevented subsequent actions.");
        Check(Directory.GetFiles(folder, "launcher-result-*").Length == 0, "Cancelled diagnostic results were left behind.");
        string shell = testPowerShell ?? Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
        await UsbCancellationTests.RunAsync(folder, shell);
        string[] sleepingCommand = ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 30"];
        bool nativeTimedOut = false;
        try { await ProcessRunner.NativeAsync(shell, sleepingCommand, TimeSpan.FromSeconds(2)).WaitAsync(TimeSpan.FromSeconds(8)); }
        catch (TimeoutException error) when (error.Message.Contains("did not respond")) { nativeTimedOut = true; }
        Check(nativeTimedOut, "The WSL prerequisite probe did not time out.");
        using (var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
        {
            bool nativeCancelled = false;
            try { await ProcessRunner.NativeAsync(shell, sleepingCommand, cancellation: cancel.Token).WaitAsync(TimeSpan.FromSeconds(8)); }
            catch (OperationCanceledException) { nativeCancelled = true; }
            Check(nativeCancelled, "The WSL prerequisite probe ignored cancellation.");
        }
    }
    Console.WriteLine("OK: settings, literal arguments, process results, diagnostic timeout/cancellation and subsequent actions.");
}
finally { Directory.Delete(folder, true); }
