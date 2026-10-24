using System.Text.Json;

namespace XrGate.Launcher.Core;

public sealed record FirstLoginSession(int ProcessId, long StartTicks)
{
    public static FirstLoginSession? Read(JsonElement data)
    {
        if (data.ValueKind != JsonValueKind.Object || !data.TryGetProperty("first_login", out var login) ||
            login.ValueKind != JsonValueKind.Object || !login.TryGetProperty("process_id", out var pid) ||
            pid.ValueKind != JsonValueKind.Number || !pid.TryGetInt32(out int id) || id <= 0 || !login.TryGetProperty("start_ticks", out var start) ||
            start.ValueKind != JsonValueKind.String || !long.TryParse(start.GetString(), out long ticks) || ticks <= 0) return null;
        return new(id, ticks);
    }
}
