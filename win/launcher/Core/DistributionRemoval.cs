using System.Text.Json;
using System.Text.RegularExpressions;

namespace XrGate.Launcher.Core;

public sealed record DistributionRemoval(string Distribution, bool RemoveUbuntu = true)
{
    public string Confirmation => !RemoveUbuntu ? "Remove the remaining SteamVR driver copies installed by the XR Gate launcher?\n\n" +
        "Their cached files and registrations will be deleted. Driver packages at other locations will be preserved. Continue?" :
        "Permanently delete the WSL distribution '" + Distribution + "'?\n\n" +
        "This deletes ALL files, applications and settings inside that Ubuntu distribution, including XR Gate and controller calibrations. This cannot be undone.\n\n" +
        "SteamVR driver copies installed by the XR Gate launcher will also be unregistered and deleted from its driver cache. Other driver locations are preserved.\n\n" +
        "Tracking and audio will stop, and selected USB devices will be returned to Windows before deletion. Continue?";

    public static DistributionRemoval Load(string stateFile)
    {
        using var state = JsonDocument.Parse(File.ReadAllText(stateFile));
        string name = state.RootElement.GetProperty("Distribution").GetString() ?? "";
        if (!Regex.IsMatch(name, @"^[A-Za-z0-9][A-Za-z0-9._-]*$") || name.StartsWith("docker-desktop", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The saved XR Gate Ubuntu distribution is invalid. No distribution was deleted.");
        bool configured = state.RootElement.TryGetProperty("LinuxUser", out var user) && user.ValueKind == JsonValueKind.String && !string.IsNullOrEmpty(user.GetString());
        bool pending = state.RootElement.TryGetProperty("InstallPending", out var install) && install.ValueKind == JsonValueKind.True;
        bool drivers = new[] { "DriverPath", "RegisteredDriver" }.Any(key => state.RootElement.TryGetProperty(key, out var value) &&
            value.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(value.GetString()));
        if (!configured && !pending && !drivers) throw new InvalidDataException("No saved XR Gate installation is available to delete.");
        return new(name, configured || pending);
    }
}
