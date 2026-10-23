using System.Diagnostics;

namespace XrGate.Launcher.Services;

public sealed class WslKeeper(Action<string> log) : IDisposable
{
    private Process? process;
    private string distribution = "";

    public async Task EnsureAsync(string selected, bool allowMissing, CancellationToken cancellation = default)
    {
        cancellation.ThrowIfCancellationRequested();
        if (process is { HasExited: false } && distribution == selected) return;
        Dispose();
        string wsl = Path.Combine(Environment.SystemDirectory, "wsl.exe");
        if (!File.Exists(wsl))
        {
            if (allowMissing) return;
            throw new IOException("WSL is not installed. Click Install first.");
        }
        var (code, output) = await ProcessRunner.NativeAsync(wsl, ["--list", "--quiet"], cancellation: cancellation);
        if (code != 0 || !output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Contains(selected))
        {
            if (allowMissing) return;
            throw new IOException("The selected Ubuntu distribution is not ready. Click Install.");
        }
        var info = new ProcessStartInfo(wsl) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true };
        // Explicit root avoids triggering first-login username prompts in a hidden
        // window. EOF on this private pipe ends the Linux lease as well as its
        // Windows client; killing only wsl.exe could leave sleep infinity alive.
        foreach (string argument in new[] { "--distribution", selected, "--user", "root", "--exec", "cat" }) info.ArgumentList.Add(argument);
        process = Process.Start(info) ?? throw new IOException("Could not keep Ubuntu running.");
        distribution = selected;
        await Task.Delay(350, cancellation);
        if (process.HasExited) throw new IOException("Ubuntu could not start. Complete setup or run Diagnostics.");
        log("[Launcher] Keeping " + selected + " running while this window is open.");
    }

    public void Dispose()
    {
        if (process is null) return;
        try
        {
            process.StandardInput.Close();
            if (!process.WaitForExit(2000)) process.Kill(entireProcessTree: true);
        }
        catch (Exception error) when (error is InvalidOperationException or IOException or System.ComponentModel.Win32Exception) { }
        finally { process.Dispose(); process = null; distribution = ""; }
    }
}
