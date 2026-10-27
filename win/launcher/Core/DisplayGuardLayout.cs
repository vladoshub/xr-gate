namespace XrGate.Launcher.Core;

public readonly record struct ScreenBounds(int Left, int Top, int Right, int Bottom)
{
    public int Width => Right - Left;
    public int Height => Bottom - Top;
    public bool Valid => Width > 0 && Height > 0;
    public bool Contains(int x, int y) => x >= Left && x < Right && y >= Top && y < Bottom;
    public long Overlap(ScreenBounds other) =>
        (long)Math.Max(0, Math.Min(Right, other.Right) - Math.Max(Left, other.Left)) *
        Math.Max(0, Math.Min(Bottom, other.Bottom) - Math.Max(Top, other.Top));

    public (int X, int Y) Clamp(int x, int y) => (Math.Clamp(x, Left, Right - 1), Math.Clamp(y, Top, Bottom - 1));
    public long Distance(int x, int y)
    {
        var point = Clamp(x, y);
        return (long)(point.X - x) * (point.X - x) + (long)(point.Y - y) * (point.Y - y);
    }
}

public sealed record GuardMonitor(string Name, string Id, ScreenBounds Bounds, ScreenBounds WorkArea, bool Primary);

public sealed record DisplayGuardLayout(GuardMonitor Device, IReadOnlyList<GuardMonitor> Desktop)
{
    // Match both the observed GDI output and the saved monitor interface identity.
    // Never infer the protected output from its number, resolution or location.
    public static DisplayGuardLayout? Create(string name, string id, IEnumerable<GuardMonitor> monitors)
    {
        if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(id)) return null;
        var all = monitors.Where(m => m.Bounds.Valid && m.WorkArea.Valid).ToArray();
        var matching = all.Where(m => m.Name.Equals(name, StringComparison.OrdinalIgnoreCase)).ToArray();
        if (matching.Length != 1 || matching[0].Primary || !matching[0].Id.Equals(id, StringComparison.OrdinalIgnoreCase)) return null;
        var device = matching[0];
        var desktop = all.Where(m => m != device).ToArray();
        if (desktop.Length == 0 || desktop.Any(m => m.Bounds.Overlap(device.Bounds) > 0)) return null;
        return new(device, desktop);
    }

    public (int X, int Y)? RedirectPointer(int x, int y) => Device.Bounds.Contains(x, y)
        ? Desktop.MinBy(m => m.Bounds.Distance(x, y))!.Bounds.Clamp(x, y) : null;

    public ScreenBounds? RedirectWindow(ScreenBounds window, bool maximized)
    {
        if (!window.Valid || window.Overlap(Device.Bounds) == 0) return null;
        var target = Desktop.OrderByDescending(m => m.Bounds.Overlap(window))
            .ThenBy(m => m.Bounds.Distance(window.Left, window.Top)).First().WorkArea;
        if (maximized) return target;
        int width = Math.Min(window.Width, target.Width), height = Math.Min(window.Height, target.Height);
        int x = Math.Clamp(window.Left, target.Left, target.Right - width);
        int y = Math.Clamp(window.Top, target.Top, target.Bottom - height);
        return new(x, y, x + width, y + height);
    }
}
