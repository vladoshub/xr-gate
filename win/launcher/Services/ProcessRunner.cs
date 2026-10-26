using System.Diagnostics;
using System.Text;
using XrGate.Launcher.Core;

namespace XrGate.Launcher.Services;

public sealed class ProcessRunner(string root, string stateDirectory, Action<string> log, string? powerShellPath = null)
{
    public static TimeSpan DiagnosticTimeout { get; } = TimeSpan.FromSeconds(60);
    public static TimeSpan UsbTimeout { get; } = TimeSpan.FromMinutes(3);

    public async Task<ActionResult> RunAsync(string action, bool steamVr = false, bool console = false, CancellationToken cancellation = default, TimeSpan? timeLimit = null, IReadOnlyDictionary<string, string>? parameters = null, bool quiet = false)
    {
        using var deadline = new CancellationTokenSource(timeLimit ?? (action is "Doctor" or "Status" ? DiagnosticTimeout :
            action is "Usb" or "Detach" or "Stop" or "BluetoothScan" or "Pair" ? UsbTimeout :
            action == "DeviceStatus" ? TimeSpan.FromSeconds(15) : action == "Test" ? TimeSpan.FromSeconds(605) : Timeout.InfiniteTimeSpan));
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, deadline.Token);
        cancellation = linked.Token;
        cancellation.ThrowIfCancellationRequested();
        string script = Path.Combine(root, "XrGate-Wsl.ps1");
        if (!File.Exists(script)) throw new FileNotFoundException("Extract XR-Gate.exe with all scripts from the Windows ZIP.", script);
        Directory.CreateDirectory(stateDirectory);
        string result = Path.Combine(stateDirectory, "launcher-result-" + Guid.NewGuid().ToString("N") + ".json");
        string powerShell = powerShellPath ?? Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
        var info = new ProcessStartInfo(powerShell)
        {
            WorkingDirectory = root, UseShellExecute = console, CreateNoWindow = !console,
            RedirectStandardOutput = !console, RedirectStandardError = !console
        };
        if (!console) { info.StandardOutputEncoding = Encoding.UTF8; info.StandardErrorEncoding = Encoding.UTF8; }
        if (!console) info.ArgumentList.Add("-NonInteractive");
        foreach (string argument in new[] { "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text" }) info.ArgumentList.Add(argument);
        if (console)
        {
            info.ArgumentList.Add("-EncodedCommand");
            info.ArgumentList.Add(ScriptCommand.Encode(script, action, result, steamVr, console));
        }
        else
        {
            // Windows PowerShell serializes host/error records as CLIXML with
            // -EncodedCommand. -File keeps redirected streams as readable text.
            foreach (string argument in new[] { "-File", script, "-Action", action, "-Launcher", "-ResultPath", result })
                info.ArgumentList.Add(argument);
            if (steamVr) info.ArgumentList.Add("-StartSteamVR");
            if (parameters is not null)
                foreach (var parameter in parameters)
                {
                    if (!(action == "Pair" && parameter.Key is "LeftControllerMac" or "RightControllerMac") &&
                        !(action == "Delete" && parameter.Key == "DeleteDistributionName")) throw new ArgumentException("Unsupported launcher parameter.");
                    info.ArgumentList.Add("-" + parameter.Key);
                    info.ArgumentList.Add(parameter.Value);
                }
        }
        if (!quiet && action != "DeviceStatus") log("[Launcher] " + action + (console ? " — complete the prompts in the separate console." : ""));
        try
        {
            using var process = Process.Start(info) ?? throw new IOException("Could not start Windows PowerShell.");
            Task output = console ? Task.CompletedTask : PumpAsync(process.StandardOutput, cancellation, !quiet && action != "DeviceStatus");
            Task error = console ? Task.CompletedTask : PumpAsync(process.StandardError, cancellation, !quiet && action != "DeviceStatus");
            try { await process.WaitForExitAsync(cancellation); }
            catch (OperationCanceledException)
            {
                // USB and read-only operations own only transient children. Stop
                // their usbipd/wsl clients too, so cancellation cannot leave the
                // same attach command running behind the next attempt. Run owns
                // a separate tracking console and still uses the scoped Stop.
                Terminate(process, action is "Usb" or "Detach" or "Doctor" or "Status" or "DeviceStatus" or "Test" or "BluetoothScan" or "Pair" or "Controllers");
                try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2)); } catch (TimeoutException) { }
                try { await Task.WhenAll(output, error).WaitAsync(TimeSpan.FromSeconds(2)); }
                catch (OperationCanceledException) { }
                catch (TimeoutException) { }
                throw;
            }
            await Task.WhenAll(output, error).WaitAsync(cancellation);
            return ActionResult.Read(result, process.ExitCode, action);
        }
        finally
        {
            foreach (string file in new[] { result, result + ".new" }) if (File.Exists(file)) File.Delete(file);
        }
    }

    private async Task PumpAsync(StreamReader reader, CancellationToken cancellation, bool visible = true)
    {
        while (await reader.ReadLineAsync(cancellation) is { } line)
            if (visible) log(line.Replace("\0", ""));
    }

    private static void Terminate(Process process, bool entireProcessTree)
    {
        try { if (!process.HasExited) process.Kill(entireProcessTree); }
        catch (InvalidOperationException) { } // Process exited during cancellation.
    }

    public static async Task<(int Code, string Output)> NativeAsync(string file, IEnumerable<string> arguments, TimeSpan? timeout = null, CancellationToken cancellation = default)
    {
        var info = new ProcessStartInfo(file)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (string argument in arguments) info.ArgumentList.Add(argument);
        using var timer = new CancellationTokenSource(timeout ?? TimeSpan.FromSeconds(20));
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation, timer.Token);
        deadline.Token.ThrowIfCancellationRequested();
        using var process = Process.Start(info) ?? throw new IOException("Could not start " + file);
        Task<string> output = process.StandardOutput.ReadToEndAsync(deadline.Token);
        Task<string> error = process.StandardError.ReadToEndAsync(deadline.Token);
        try
        {
            await process.WaitForExitAsync(deadline.Token);
            await Task.WhenAll(output, error).WaitAsync(deadline.Token);
            return (process.ExitCode, ((await output) + (await error)).Replace("\0", "").Trim());
        }
        catch (OperationCanceledException)
        {
            Terminate(process, entireProcessTree: true);
            cancellation.ThrowIfCancellationRequested();
            throw new TimeoutException(file + " did not respond. Check Diagnostics.");
        }
    }
}
