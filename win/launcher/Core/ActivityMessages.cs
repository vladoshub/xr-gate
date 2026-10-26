using System.Text.RegularExpressions;

namespace XrGate.Launcher.Core;

public static class ActivityMessages
{
    public const string TrackingSetupMarker = "[Tracking setup]";
    public const string ReconnectHint = "Stop tracking, unplug and reconnect the XR device and the selected USB Bluetooth adapter (if used), then retry.";

    public static string Tone(string message) => message.Contains(TrackingSetupMarker, StringComparison.Ordinal)
        ? "guidance" : "normal";

    public static bool SuggestReconnect(string message)
    {
        if (Regex.IsMatch(message, @"permission denied|access is denied|not authorized", RegexOptions.IgnoreCase)) return false;
        return Regex.IsMatch(message,
            @"device busy|device appears to be used by Windows|no controller became ready|no ready BlueZ.*controller|no default controller available|" +
            @"usbipd.*(?:timed out|timeout|did not finish)|" +
            @"(?:USB|Bluetooth|camera|HCI|adapter).*(?:not responding|did not respond|timed out|timeout|did not return|device disconnected|error state)",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    }
}
