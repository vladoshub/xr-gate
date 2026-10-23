using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace XrGateWsl {
    // Shared by Windows PowerShell 5.1 and PowerShell 7. One deadline covers
    // every read, including a peer that keeps sending an unfinished line.
    public sealed class DeadlineLineReader {
        private readonly Stream stream;
        private readonly Stopwatch clock;
        private readonly int limitMilliseconds;
        private readonly byte[] buffer = new byte[8192];
        private int start, end;
        private const int MaxLineBytes = 1024 * 1024;
        private long lastValidation = -1000;
        // Called on the PowerShell runspace, never a background timer thread.
        public Action Validate { get; set; }

        public DeadlineLineReader(Stream stream, Stopwatch clock, int limitMilliseconds) {
            this.stream = stream;
            this.clock = clock;
            this.limitMilliseconds = limitMilliseconds;
        }

        public string ReadLine() {
            using (var line = new MemoryStream()) {
                while (true) {
                    int remaining = limitMilliseconds - (int)clock.ElapsedMilliseconds;
                    if (remaining <= 0) throw new TimeoutException("XR Gate TCP deadline reached.");
                    if (Validate != null && clock.ElapsedMilliseconds - lastValidation >= 500) {
                        Validate();
                        lastValidation = clock.ElapsedMilliseconds;
                    }
                    for (int i = start; i < end; i++) {
                        if (buffer[i] != 10) continue;
                        Append(line, start, i - start);
                        start = i + 1;
                        return new UTF8Encoding(false, true).GetString(line.ToArray()).TrimEnd('\r');
                    }
                    Append(line, start, end - start);
                    stream.ReadTimeout = Validate == null ? remaining : Math.Min(500, remaining);
                    start = 0;
                    end = 0;
                    try { end = stream.Read(buffer, 0, buffer.Length); }
                    catch (IOException error) {
                        var socketError = error.InnerException as System.Net.Sockets.SocketException;
                        if (Validate != null && socketError != null && socketError.SocketErrorCode == System.Net.Sockets.SocketError.TimedOut) continue;
                        throw;
                    }
                    if (end == 0) {
                        if (line.Length != 0) throw new IOException("XR Gate TCP closed during a frame.");
                        return null;
                    }
                }
            }
        }

        private void Append(MemoryStream line, int offset, int count) {
            if (line.Length + count > MaxLineBytes) throw new IOException("XR Gate TCP line exceeds 1 MiB.");
            line.Write(buffer, offset, count);
        }
    }
}
