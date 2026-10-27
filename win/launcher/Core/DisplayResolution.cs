using System.Text.RegularExpressions;

namespace XrGate.Launcher.Core;

public sealed record DisplayResolution(int Width, int Height)
{
    public static DisplayResolution? Parse(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return null;
        var match = Regex.Match(value.Trim(), @"^([1-9][0-9]{0,4})[xX]([1-9][0-9]{0,4})$");
        if (!match.Success) throw new FormatException("SBS_RESOLUTION must be WIDTHxHEIGHT, for example 3840x1080, or empty.");
        return new(int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value));
    }

    public override string ToString() => $"{Width}x{Height}";
}
