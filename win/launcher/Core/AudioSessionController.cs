namespace XrGate.Launcher.Core;

public sealed record AudioRoute(string Distribution, string LinuxUser, string SourceId, string OutputId, bool PlaybackEnabled = true)
{
    public bool Enabled => PlaybackEnabled && SourceId.Length > 0 && OutputId.Length > 0 && SourceId != "none" && OutputId != "none";
}

public sealed class AudioSessionController(Func<AudioRoute, CancellationToken, Task> run)
{
    private readonly SemaphoreSlim gate = new(1, 1);
    private CancellationTokenSource? cancellation;
    private Task? worker;
    private AudioRoute? current;
    public bool Enabled => current is not null && worker is { IsCompleted: false };
    public async Task ConfigureAsync(AudioRoute? route, CancellationToken lifetime = default)
    {
        if (route is { Enabled: false }) route = null;
        await gate.WaitAsync(lifetime);
        try
        {
            if (current == route && (route is null || worker is { IsCompleted: false })) return;
            Cancel();
            if (worker is not null)
            {
                try { await worker.WaitAsync(TimeSpan.FromSeconds(4)); }
                catch (OperationCanceledException) { }
            }
            cancellation?.Dispose();
            cancellation = null; worker = null; current = null;
            if (route is null) return;
            lifetime.ThrowIfCancellationRequested();
            cancellation = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
            var token = cancellation.Token;
            current = route;
            worker = Task.Run(() => run(route, token), CancellationToken.None);
        }
        finally { gate.Release(); }
    }
    public void Cancel()
    {
        try { cancellation?.Cancel(); } catch (ObjectDisposedException) { }
    }
}
