using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace XrGate.Launcher.Services;

public static class RuntimeProgressClient
{
    public static async Task FollowAsync(string root, string distribution, string user, string linuxRoot, string token,
        Action<string> stage, CancellationToken cancellation)
    {
        string wsl = Path.Combine(Environment.SystemDirectory, "wsl.exe");
        string helper = Path.Combine(root, "linux", "runtime_progress.py");
        var converted = await ProcessRunner.NativeAsync(wsl, ["-d", distribution, "-u", user, "--exec", "wslpath", "-a", "-u", helper], cancellation: cancellation);
        if (converted.Code != 0 || !converted.Output.StartsWith('/')) throw new IOException("Could not locate controller progress helper.");
        var info = new ProcessStartInfo(wsl)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (string argument in new[] { "-d", distribution, "-u", user, "--exec", "python3", converted.Output,
            "--base", linuxRoot, "--launch-token", token }) info.ArgumentList.Add(argument);
        using var process = Process.Start(info) ?? throw new IOException("Could not observe controller calibration.");
        Task<string> errors = process.StandardError.ReadToEndAsync();
        try
        {
            while (await process.StandardOutput.ReadLineAsync(cancellation) is { } line)
            {
                using var data = JsonDocument.Parse(line);
                if (data.RootElement.TryGetProperty("stage", out var value) && value.GetString() is { } message) stage(message);
            }
            await process.WaitForExitAsync(cancellation);
            if (process.ExitCode != 0) throw new IOException(await errors);
        }
        finally
        {
            // EOF ends the read-only Linux worker even after cancellation.
            try { process.StandardInput.Close(); }
            catch (IOException) { }
            if (!process.HasExited)
            {
                try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(1)); }
                catch (TimeoutException)
                {
                    try { process.Kill(entireProcessTree: true); }
                    catch (InvalidOperationException) when (process.HasExited) { }
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2));
                }
            }
            await errors;
        }
    }
}
