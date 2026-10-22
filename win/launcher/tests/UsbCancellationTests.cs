using System.Diagnostics;
using System.Text;
using XrGate.Launcher.Services;

internal static class UsbCancellationTests
{
    public static async Task RunAsync(string folder, string shell)
    {
        // Process.Kill(tree) enumerates /proc on Unix. Some container runners
        // expose host /proc inside a different PID namespace; that cannot model
        // a Windows process tree. Windows CI always runs all cases below.
        if (OperatingSystem.IsLinux() && File.ReadAllText("/proc/self/stat").Split(' ', 2)[0] != Environment.ProcessId.ToString())
        {
            Console.WriteLine("SKIP USB child-process tests: /proc does not match the PID namespace; run on Windows CI.");
            return;
        }
        string root = Path.Combine(folder, "USB O'Brien fixture");
        Directory.CreateDirectory(root);
        string pidFile = Path.Combine(root, "child.pid");
        string childScript = Path.Combine(root, "child.ps1");
        File.WriteAllText(childScript, "Start-Sleep -Seconds 60\n", new UTF8Encoding(true));
        static string Literal(string value) => "'" + value.Replace("'", "''") + "'";
        string fixture = "param([string]$Action,[switch]$Launcher,[string]$ResultPath)\n" +
            "$p = Start-Process -FilePath " + Literal(shell) + " -NoNewWindow -PassThru -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive','-File'," +
            Literal('"' + childScript + '"') + ")\n" +
            "[IO.File]::WriteAllText(" + Literal(pidFile) + ", [string]$p.Id)\n" +
            "Write-Output 'fixture child started'\n$p.WaitForExit()\n";
        File.WriteAllText(Path.Combine(root, "XrGate-Wsl.ps1"), fixture, new UTF8Encoding(true));
        foreach (string mode in new[] { "cancel", "timeout", "runtime" })
        {
            File.Delete(pidFile);
            Process? child = null;
            try
            {
                using var cancel = new CancellationTokenSource();
                var runner = new ProcessRunner(root, folder, line =>
                {
                    if (mode != "timeout" && line.Contains("fixture child started")) cancel.Cancel();
                }, shell);
                bool interrupted = false;
                var watch = Stopwatch.StartNew();
                try
                {
                    await runner.RunAsync(mode == "runtime" ? "Run" : "Usb", cancellation: cancel.Token,
                        timeLimit: TimeSpan.FromSeconds(6)).WaitAsync(TimeSpan.FromSeconds(12));
                }
                catch (OperationCanceledException) { interrupted = true; }
                if (!interrupted || watch.Elapsed > TimeSpan.FromSeconds(11) || !File.Exists(pidFile))
                    throw new Exception("USB process cancellation/deadline failed: " + mode);
                int pid = int.Parse(File.ReadAllText(pidFile));
                try { child = Process.GetProcessById(pid); } catch (ArgumentException) { }
                if (mode == "runtime")
                {
                    if (child is null || child.HasExited)
                        throw new Exception("Cancelling Run killed the separately managed tracking console.");
                }
                else if (child is not null && !child.HasExited && !child.WaitForExit(3000))
                    throw new Exception("The USB child process survived cancellation: " + mode);
            }
            finally
            {
                if (child is null && File.Exists(pidFile))
                    try { child = Process.GetProcessById(int.Parse(File.ReadAllText(pidFile))); } catch (ArgumentException) { }
                if (child is not null)
                {
                    using (child) { if (!child.HasExited) { child.Kill(entireProcessTree: true); child.WaitForExit(3000); } }
                }
            }
        }
    }
}
