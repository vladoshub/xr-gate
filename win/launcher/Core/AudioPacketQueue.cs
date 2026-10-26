using System.Buffers.Binary;
using System.Threading.Channels;

namespace XrGate.Launcher.Core;

public sealed record AudioPacket(byte[] FramedData, long CapturedAt);

// Capture never waits for WSL. Discard stale sound instead of growing latency.
public sealed class AudioPacketQueue
{
    public const int SampleRate = 48000;
    public const int Channels = 2;
    public const int BlockAlign = Channels * 2;
    public const int PacketBytes = SampleRate * BlockAlign / 50;
    public const int Capacity = 8;
    private readonly TimeProvider clock;
    private readonly Channel<AudioPacket> packets = Channel.CreateBounded<AudioPacket>(new BoundedChannelOptions(Capacity)
    { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true, SingleWriter = true });
    public AudioPacketQueue(TimeProvider? clock = null) => this.clock = clock ?? TimeProvider.System;
    public void Add(byte[] data, int count)
    {
        if (count < 0 || count > data.Length || count % BlockAlign != 0) throw new ArgumentException("Unaligned audio capture packet.");
        for (int offset = 0; offset < count; offset += PacketBytes)
        {
            int length = Math.Min(PacketBytes, count - offset);
            var framed = new byte[length + 4];
            BinaryPrimitives.WriteInt32LittleEndian(framed, length);
            data.AsSpan(offset, length).CopyTo(framed.AsSpan(4));
            packets.Writer.TryWrite(new AudioPacket(framed, clock.GetTimestamp()));
        }
    }
    public async ValueTask<AudioPacket> ReadAsync(CancellationToken cancellation)
    {
        while (true)
        {
            var packet = await packets.Reader.ReadAsync(cancellation);
            if (clock.GetElapsedTime(packet.CapturedAt) <= TimeSpan.FromMilliseconds(160)) return packet;
        }
    }
}
