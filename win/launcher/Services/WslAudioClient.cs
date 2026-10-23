using System.Diagnostics;
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using XrGate.Launcher.Core;

namespace XrGate.Launcher.Services;

public sealed class WslAudioClient(string root, string distribution, string linuxUser)
{
    private string? helper;
    private static string Wsl => Path.Combine(Environment.SystemDirectory, "wsl.exe");
    private List<string> Arguments(params string[] command)
    {
        var arguments = new List<string> { "--distribution", distribution };
        if (linuxUser.Length > 0) arguments.AddRange(["--user", linuxUser]);
        arguments.Add("--exec"); arguments.AddRange(command);
        return arguments;
    }
    private async Task<string> HelperAsync(CancellationToken cancellation)
    {
        if (helper is not null) return helper;
        string path = Path.Combine(root, "linux", "xr_gate_audio.py");
        if (!File.Exists(path)) throw new FileNotFoundException("Extract the complete Windows package, including linux/xr_gate_audio.py.", path);
        var result = await ProcessRunner.NativeAsync(Wsl, Arguments("wslpath", "-a", "-u", path), TimeSpan.FromSeconds(12), cancellation);
        if (result.Code != 0 || !result.Output.StartsWith('/') || result.Output.Contains('\n'))
            throw new IOException("Could not locate the audio helper in Ubuntu: " + result.Output);
        return helper = result.Output;
    }
    public async Task<List<AudioDeviceChoice>> ListAsync(CancellationToken cancellation)
    {
        string path = await HelperAsync(cancellation);
        var result = await ProcessRunner.NativeAsync(Wsl, Arguments("python3", path, "list"), TimeSpan.FromSeconds(12), cancellation);
        if (result.Code != 0) throw new IOException("WSL audio discovery: " + result.Output);
        using var json = JsonDocument.Parse(result.Output);
        if (json.RootElement.GetProperty("protocol").GetString() != "xr-gate-audio/1")
            throw new InvalidDataException("Unexpected WSL audio discovery response.");
        return json.RootElement.GetProperty("devices").EnumerateArray().Select(item => new AudioDeviceChoice(
            item.GetProperty("id").GetString()!, item.GetProperty("name").GetString()!, item.GetProperty("available").GetBoolean())).ToList();
    }
    public async Task<WslAudioReceiver> OpenAsync(string device, Action<string> log, CancellationToken cancellation)
    {
        string path = await HelperAsync(cancellation);
        var info = new ProcessStartInfo(Wsl)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, StandardInputEncoding = new UTF8Encoding(false)
        };
        foreach (string argument in Arguments("python3", path, "play", "--device", device)) info.ArgumentList.Add(argument);
        info.Environment["PYTHONIOENCODING"] = "utf-8";
        var receiver = new WslAudioReceiver(Process.Start(info) ?? throw new IOException("Could not start WSL audio playback."), log);
        try
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            deadline.CancelAfter(TimeSpan.FromSeconds(12));
            byte[] header = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new
            { protocol = "xr-gate-audio/1", format = "s16le", rate = AudioPacketQueue.SampleRate, channels = AudioPacketQueue.Channels }) + "\n");
            await receiver.Input.WriteAsync(header, deadline.Token);
            await receiver.Input.FlushAsync(deadline.Token);
            string response = await receiver.Process.StandardOutput.ReadLineAsync(deadline.Token)
                ?? throw new IOException("The WSL audio output did not open. See the audio log.");
            using var json = JsonDocument.Parse(response);
            if (json.RootElement.GetProperty("protocol").GetString() != "xr-gate-audio/1" || !json.RootElement.GetProperty("ready").GetBoolean())
                throw new IOException("The WSL audio output is not ready.");
            return receiver;
        }
        catch { await receiver.DisposeAsync(); throw; }
    }
}

public sealed class WslAudioReceiver : IAsyncDisposable
{
    public Process Process { get; }
    public Stream Input => Process.StandardInput.BaseStream;
    private readonly Task errors;
    private readonly CancellationTokenSource errorCancellation = new();
    public WslAudioReceiver(Process process, Action<string> log) { Process = process; errors = PumpErrorsAsync(log); }
    public async Task SendAsync(byte[] packet, CancellationToken cancellation)
    {
        await Input.WriteAsync(packet, cancellation);
        // Only one packet in flight: wsl.exe buffering cannot hide a blocked
        // device and turn a bounded capture queue into seconds of stale audio.
        var acknowledgement = new byte[4];
        await Process.StandardOutput.BaseStream.ReadExactlyAsync(acknowledgement, cancellation);
        if (BinaryPrimitives.ReadInt32LittleEndian(acknowledgement) != packet.Length - 4)
            throw new InvalidDataException("Invalid audio playback acknowledgement.");
    }
    private async Task PumpErrorsAsync(Action<string> log)
    {
        try
        {
            while (await Process.StandardError.ReadLineAsync(errorCancellation.Token) is { } line)
                if (line.Length > 0) log("[Audio] " + line);
        }
        catch (OperationCanceledException) { }
        catch (IOException) { }
    }
    public void Abort()
    {
        try { Process.StandardInput.Close(); } catch (Exception error) when (error is IOException or InvalidOperationException) { }
        try { if (!Process.HasExited) Process.Kill(entireProcessTree: true); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { }
    }
    public async ValueTask DisposeAsync()
    {
        try
        {
            Process.StandardInput.Close(); // EOF ends the Linux helper too.
            try { await Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(1)); }
            catch (TimeoutException)
            {
                Abort();
                await Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(1));
            }
        }
        catch (Exception error) when (error is IOException or InvalidOperationException) { Abort(); }
        finally
        {
            errorCancellation.Cancel();
            try { await errors; } catch (ObjectDisposedException) { }
            Process.Dispose(); errorCancellation.Dispose();
        }
    }
}
