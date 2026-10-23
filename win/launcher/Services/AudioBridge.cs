using NAudio.CoreAudioApi;
using NAudio.Wave;
using XrGate.Launcher.Core;

namespace XrGate.Launcher.Services;

public sealed class AudioBridge
{
    private readonly string root;
    private readonly Action<string> log;
    private readonly AudioSessionController controller;
    public bool Enabled => controller.Enabled;
    public AudioBridge(string root, Action<string> log)
    { this.root = root; this.log = log; controller = new AudioSessionController(SuperviseAsync); }
    public Task ConfigureAsync(AudioRoute? route, CancellationToken lifetime = default) => controller.ConfigureAsync(route, lifetime);
    public void Cancel() => controller.Cancel();
    public static List<AudioDeviceChoice> Sources()
    {
        using var enumerator = new MMDeviceEnumerator();
        var result = new List<AudioDeviceChoice>();
        foreach (var device in enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
            using (device) result.Add(new AudioDeviceChoice(device.ID, device.FriendlyName));
        return result;
    }
    private static MMDevice Source(MMDeviceEnumerator enumerator, string identity)
    {
        var device = identity == "default" ? enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia) : enumerator.GetDevice(identity);
        if (device.State == DeviceState.Active) return device;
        device.Dispose(); throw new IOException("The selected Windows audio source is not connected.");
    }
    private async Task SuperviseAsync(AudioRoute route, CancellationToken cancellation)
    {
        string lastError = "";
        var wsl = new WslAudioClient(root, route.Distribution, route.LinuxUser);
        while (!cancellation.IsCancellationRequested)
        {
            try { await RunSessionAsync(route, wsl, () => lastError = "", cancellation); }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { return; }
            catch (Exception error)
            {
                string message = error is OperationCanceledException ? "WSL audio did not respond in time." : error.Message;
                if (message != lastError) { log("[Audio] Waiting for the selected devices: " + message); lastError = message; }
            }
            try { await Task.Delay(TimeSpan.FromSeconds(2), cancellation); } catch (OperationCanceledException) { return; }
        }
    }
    private async Task RunSessionAsync(AudioRoute route, WslAudioClient wsl, Action ready, CancellationToken cancellation)
    {
        using var enumerator = new MMDeviceEnumerator();
        using var source = Source(enumerator, route.SourceId);
        var outputs = await wsl.ListAsync(cancellation);
        var output = outputs.SingleOrDefault(item => item.Id == route.OutputId && item.Available)
            ?? throw new IOException("The saved WSL output is disconnected or ambiguous. Its selection is preserved.");
        await using var receiver = await wsl.OpenAsync(route.OutputId, log, cancellation);
        using var session = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        using var abort = session.Token.Register(receiver.Abort);
        using var capture = new LoopbackCapture(source)
        { WaveFormat = new WaveFormat(AudioPacketQueue.SampleRate, 16, AudioPacketQueue.Channels) };
        var packets = new AudioPacketQueue();
        var stopped = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        capture.DataAvailable += (_, args) => packets.Add(args.Buffer, args.BytesRecorded);
        capture.RecordingStopped += (_, args) =>
        { if (args.Exception is not null) stopped.TrySetException(args.Exception); else stopped.TrySetResult(); };
        Task? send = null, watch = null, exited = null;
        try
        {
            capture.StartRecording(); ready();
            log("[Audio] " + source.FriendlyName + " → " + output.Name);
            send = SendAsync(packets, receiver, session.Token);
            watch = WatchSourceAsync(route.SourceId, source.ID, session.Token);
            exited = receiver.Process.WaitForExitAsync(session.Token);
            var completed = await Task.WhenAny(send, watch, exited, stopped.Task);
            await completed;
            cancellation.ThrowIfCancellationRequested();
            throw new IOException("Audio playback disconnected; reconnecting the saved devices.");
        }
        finally
        {
            session.Cancel(); capture.StopRecording();
            foreach (var task in new[] { send, watch, exited, stopped.Task })
                if (task is not null) _ = task.ContinueWith(t => _ = t.Exception, CancellationToken.None,
                    TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }
    private static async Task SendAsync(AudioPacketQueue packets, WslAudioReceiver receiver, CancellationToken cancellation)
    {
        while (true)
        {
            var packet = await packets.ReadAsync(cancellation);
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            deadline.CancelAfter(TimeSpan.FromSeconds(1));
            await receiver.SendAsync(packet.FramedData, deadline.Token);
        }
    }
    private static async Task WatchSourceAsync(string selected, string opened, CancellationToken cancellation)
    {
        while (true)
        {
            await Task.Delay(TimeSpan.FromSeconds(1), cancellation);
            using var enumerator = new MMDeviceEnumerator();
            using var current = Source(enumerator, selected);
            if (current.ID != opened) throw new IOException("The default Windows audio output changed.");
        }
    }
    private sealed class LoopbackCapture(MMDevice device) : WasapiCapture(device, false, 20)
    {
        protected override AudioClientStreamFlags GetAudioClientStreamFlags() => AudioClientStreamFlags.Loopback | base.GetAudioClientStreamFlags();
    }
}
