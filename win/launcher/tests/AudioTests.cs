using System.Buffers.Binary;
using System.Diagnostics;
using System.Text.Json;
using System.Threading.Channels;
using XrGate.Launcher.Core;
using XrGate.Launcher.Services;

internal static class AudioTests
{
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    private sealed class Clock : TimeProvider
    {
        public long Now;
        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => Now;
    }
    public static async Task RunAsync(string source, string temporary)
    {
        string root = Path.Combine(temporary, "audio");
        Directory.CreateDirectory(Path.Combine(root, "hardware"));
        File.Copy(Path.Combine(source, ".env.example"), Path.Combine(root, ".env.example"));
        File.Copy(Path.Combine(source, "hardware/xreal_ultra.env"), Path.Combine(root, "hardware/xreal_ultra.env"));
        string env = Path.Combine(root, ".env"), state = Path.Combine(root, "state.json");
        File.WriteAllText(state, JsonSerializer.Serialize(new { Distribution = "Ubuntu-24.04", LinuxUser = "audio-user" }));
        File.WriteAllText(env, "# Keep my settings\nXR_TCP_PORT=45999\n");
        var settings = LauncherSettings.Load(root, state);
        Check(settings.AudioEnabled && settings.AudioOutputId == "none" && settings.AudioSourceId == "default", "Audio must initially be enabled with automatic output selection at Start VR.");
        string output = "alsa:" + new string('a', 64);
        settings.AudioSourceId = "{0.0.0.00000000}.{ab12}"; settings.AudioOutputId = output;
        settings.ReleaseTag = "Unfinished invalid field $";
        settings.SaveAudioSelection(root);
        var loaded = LauncherSettings.Load(root, state);
        Check(loaded.AudioSourceId == settings.AudioSourceId && loaded.AudioOutputId == output && loaded.LinuxUser == "audio-user",
            "Audio/user selections did not survive restart.");
        Check(File.ReadAllText(env).Contains("# Keep my settings\nXR_TCP_PORT=45999") && loaded.ReleaseTag == "latest",
            "Saving audio changed advanced settings or saved another unfinished form field.");
        loaded.AudioEnabled = false; loaded.SaveAudioSelection(root); loaded.Save(root);
        loaded = LauncherSettings.Load(root, state);
        Check(!loaded.AudioEnabled && loaded.AudioOutputId == output && loaded.AudioSourceId == settings.AudioSourceId,
            "Disabling audio lost device identities or did not survive a general save/restart.");
        loaded.AudioEnabled = true; loaded.SaveAudioSelection(root);
        Check(LauncherSettings.Load(root, state).AudioEnabled, "Enabling audio was not saved.");
        loaded.AudioOutputId = "none"; loaded.SaveAudioSelection(root); loaded.Save(root);
        Check(LauncherSettings.Load(root, state).AudioOutputId == "none", "A general save or restart undid No audio.");
        byte[] before = File.ReadAllBytes(env);
        loaded.AudioOutputId = "hw:0; injected";
        try { loaded.SaveAudioSelection(root); throw new Exception("Invalid audio identity was accepted."); } catch (FormatException) { }
        Check(File.ReadAllBytes(env).SequenceEqual(before), "Invalid audio modified the settings file.");
        var known = new[] { new AudioDeviceChoice(output, "Headset"), new AudioDeviceChoice("alsa:" + new string('b', 64), "Headset") };
        Check(AudioDeviceCatalog.OutputForStart(true, "none", [AudioDeviceChoice.None, known[0] with { Available = false }, known[1]]) == known[1].Id,
            "Start VR failed to choose the first available Ubuntu audio output.");
        Check(AudioDeviceCatalog.OutputForStart(true, known[1].Id, known) == known[1].Id,
            "Start VR replaced an available saved output.");
        Check(AudioDeviceCatalog.OutputForStart(false, "none", known) == "none" &&
            AudioDeviceCatalog.OutputForStart(true, output, []) == output,
            "Disabled audio or missing devices changed a saved selection.");
        var absent = AudioDeviceCatalog.Merge(known, [], output, source: false);
        Check(absent.Count == 3 && !absent.Single(item => item.Id == output).Available,
            "Disconnect forgot the identity/name or merged duplicate friendly names.");
        var returned = AudioDeviceCatalog.Merge(absent, [known[0]], output, source: false);
        Check(returned.Single(item => item.Id == output).Available && !returned.Single(item => item.Id == known[1].Id).Available,
            "Reconnect selected a different device with the same name.");
        var catalog = new AudioDeviceCatalog { Sources = [new("source", "Speakers")], Outputs = returned };
        string cache = Path.Combine(root, "audio-devices.json"); catalog.Save(cache);
        Check(AudioDeviceCatalog.Load(cache).Outputs.Single(item => item.Id == output).Name == "Headset", "Remembered names did not survive restart.");
        var empty = AudioDeviceCatalog.Merge([], [], output, source: false);
        Check(empty.Any(item => item.Id == output && !item.Available) && empty[0].Id == "none", "Missing cache lost the selection or No audio.");
        var clock = new Clock(); var queue = new AudioPacketQueue(clock);
        byte[] data = [1, 2, 3, 4]; queue.Add(data, data.Length); Array.Fill(data, (byte)99);
        var packet = await queue.ReadAsync(CancellationToken.None);
        Check(BinaryPrimitives.ReadInt32LittleEndian(packet.FramedData) == 4 && packet.FramedData[4] == 1,
            "Capture reused mutable WASAPI memory or corrupted framing.");
        for (int i = 0; i < 20; i++) queue.Add([(byte)i, 0, 0, 0], 4);
        Check((await queue.ReadAsync(CancellationToken.None)).FramedData[4] == 20 - AudioPacketQueue.Capacity,
            "Slow playback retained old audio instead of bounding latency.");
        clock.Now = 1000; queue.Add([77, 0, 0, 0], 4);
        Check((await queue.ReadAsync(CancellationToken.None)).FramedData[4] == 77, "Paused playback replayed stale sound.");
        var split = new AudioPacketQueue(clock); split.Add(new byte[AudioPacketQueue.PacketBytes + 4], AudioPacketQueue.PacketBytes + 4);
        Check((await split.ReadAsync(CancellationToken.None)).FramedData.Length == AudioPacketQueue.PacketBytes + 4 &&
            (await split.ReadAsync(CancellationToken.None)).FramedData.Length == 8, "Capture split incomplete PCM frames.");
        var starts = Channel.CreateUnbounded<AudioRoute>(); int stops = 0;
        var controller = new AudioSessionController(async (route, cancellation) =>
        {
            await starts.Writer.WriteAsync(route);
            try { await Task.Delay(Timeout.Infinite, cancellation); } finally { Interlocked.Increment(ref stops); }
        });
        var selected = new AudioRoute("Ubuntu-24.04", "audio-user", "source", output);
        await controller.ConfigureAsync(selected);
        Check(await starts.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2)) == selected, "Start opened the wrong route.");
        await controller.ConfigureAsync(selected);
        Check(!starts.Reader.TryRead(out _) && stops == 0, "Status restarted an unchanged audio session.");
        await controller.ConfigureAsync(selected with { OutputId = "none" });
        Check(!controller.Enabled && stops == 1, "No audio did not cancel playback/reconnect.");
        await controller.ConfigureAsync(selected with { PlaybackEnabled = false });
        Check(!controller.Enabled && !starts.Reader.TryRead(out _) && stops == 1, "Disabled audio started capture/playback despite a selected output.");
        using var lifetime = new CancellationTokenSource();
        await controller.ConfigureAsync(selected, lifetime.Token);
        await starts.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        lifetime.Cancel(); await controller.ConfigureAsync(null);
        Check(stops == 2 && !controller.Enabled && !starts.Reader.TryRead(out _), "Close left an audio worker or reconnected after Stop.");
        foreach (string mode in new[] { "eof", "hang", "ack", "badack" })
        {
            var info = new ProcessStartInfo(Environment.ProcessPath!)
            { UseShellExecute = false, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            if (Path.GetFileNameWithoutExtension(Environment.ProcessPath) == "dotnet") info.ArgumentList.Add(typeof(AudioTests).Assembly.Location);
            info.ArgumentList.Add("--audio-receiver-fixture"); info.ArgumentList.Add(mode);
            var process = Process.Start(info)!;
            int pid = process.Id;
            var receiver = new WslAudioReceiver(process, _ => { });
            Check(await process.StandardOutput.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(3)) == "ready", "Audio process fixture did not start.");
            if (mode == "eof")
            {
                await receiver.Input.WriteAsync(new byte[] { 1, 2, 3, 4 });
                process.StandardInput.Close();
                Check(await process.StandardOutput.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(3)) == "4", "Audio pipe failed to preserve samples or deliver EOF.");
            }
            if (mode is "ack" or "badack")
            {
                var framed = new byte[8]; BinaryPrimitives.WriteInt32LittleEndian(framed, 4);
                try
                {
                    await receiver.SendAsync(framed, CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(3));
                    Check(mode == "ack", "Invalid receiver acknowledgement was accepted.");
                }
                catch (InvalidDataException) { Check(mode == "badack", "A valid receiver acknowledgement was rejected."); }
            }
            await receiver.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(3));
            bool alive;
            try { using var child = Process.GetProcessById(pid); alive = !child.HasExited; } catch (ArgumentException) { alive = false; }
            Check(!alive, "Audio stop left its receiver process running: " + mode);
        }
        Console.WriteLine("OK: audio persistence, reconnect identity, bounded PCM and Start/Stop ownership.");
    }
}
