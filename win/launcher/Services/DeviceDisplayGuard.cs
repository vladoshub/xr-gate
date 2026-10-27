using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Threading;
using XrGate.Launcher.Core;

namespace XrGate.Launcher.Services;

// A separate message loop keeps the input hook independent of WPF dialogs/setup.
// No covering window, display-mode switch, permanent cursor clip or focus change.
public sealed class DeviceDisplayGuard : IDisposable
{
    private sealed record Target(string Name, string Id);
    private readonly Action<string> report;
    private readonly Thread thread;
    private readonly Native.WinEvent callback;
    private readonly Native.MouseHook mouseCallback;
    private readonly Dictionary<nint, long> moved = [];
    private readonly HashSet<nint> pending = [];
    private Target requested = new("", "");
    private Dispatcher? dispatcher;
    private DisplayGuardLayout? layout;
    private nint locationHook, showHook, mouseHook;
    private int disposed;
    private string lastStatus = "";

    public DeviceDisplayGuard(Action<string> report)
    {
        this.report = report;
        callback = WindowChanged;
        mouseCallback = MouseChanged;
        thread = new Thread(Run) { IsBackground = true, Name = "XR Gate display guard" };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
    }

    public void Configure(string name, string id)
    {
        var next = new Target(name, id);
        if (Volatile.Read(ref requested) == next || Volatile.Read(ref disposed) != 0) return;
        Volatile.Write(ref requested, next);
        var loop = Volatile.Read(ref dispatcher);
        if (loop is not null && !loop.HasShutdownStarted) loop.BeginInvoke(Refresh);
    }

    private void Run()
    {
        dispatcher = Dispatcher.CurrentDispatcher;
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        timer.Tick += (_, _) => Refresh();
        try
        {
            timer.Start();
            Refresh();
            if (Volatile.Read(ref disposed) == 0) Dispatcher.Run();
        }
        catch (Exception error) { Status("Protection stopped: " + error.Message); }
        finally { timer.Stop(); ReleaseHooks(); }
    }

    private void Refresh()
    {
        if (Volatile.Read(ref disposed) != 0) { dispatcher?.InvokeShutdown(); return; }
        try
        {
            var target = Volatile.Read(ref requested);
            if (target.Name.Length == 0 || target.Id.Length == 0)
            {
                layout = null; ReleaseHooks(); Status("Waiting for an enabled, selected secondary XR display."); return;
            }
            layout = DisplayGuardLayout.Create(target.Name, target.Id, ReadMonitors());
            if (layout is null)
            {
                ReleaseHooks(); Status("Waiting for the selected display. Protection requires a separate, non-primary output and another desktop monitor."); return;
            }
            locationHook = locationHook != 0 ? locationHook : Native.SetWinEventHook(0x800B, 0x800B, 0, callback, 0, 0, 0);
            showHook = showHook != 0 ? showHook : Native.SetWinEventHook(0x8002, 0x8002, 0, callback, 0, 0, 0);
            mouseHook = mouseHook != 0 ? mouseHook : Native.SetWindowsHookEx(14, mouseCallback, Native.GetModuleHandle(null), 0);
            Status("Protecting " + target.Name + (mouseHook == 0 || locationHook == 0 || showHook == 0
                ? "; some event hooks are unavailable. Periodic window/cursor protection remains active." : ": cursor and ordinary windows stay on desktop monitors."));
            Native.EnumWindows((window, _) => { ReturnWindow(window); return true; }, 0);
            if (Native.GetCursorPos(out var point)) RedirectPointer(point);
            if (moved.Count > 512) moved.Clear();
        }
        catch (Exception error) { layout = null; ReleaseHooks(); Status("Protection paused: " + error.Message); }
    }

    private static List<GuardMonitor> ReadMonitors()
    {
        var result = new List<GuardMonitor>();
        Native.EnumDisplayMonitors(0, 0, (monitor, _, _, _) =>
        {
            var info = new Native.MonitorInfo { Size = Marshal.SizeOf<Native.MonitorInfo>() };
            if (!Native.GetMonitorInfo(monitor, ref info) || string.IsNullOrEmpty(info.Name)) return true;
            var identities = new List<string>();
            for (uint index = 0; ; index++)
            {
                var device = new Native.DisplayDevice { Size = (uint)Marshal.SizeOf<Native.DisplayDevice>() };
                // EDD_GET_DEVICE_INTERFACE_NAME matches the PowerShell selector's DeviceId.
                if (!Native.EnumDisplayDevices(info.Name, index, ref device, 1)) break;
                if ((device.Flags & 1) != 0 && !string.IsNullOrEmpty(device.Id)) identities.Add(device.Id);
            }
            // A cloned GDI output cannot be reserved independently.
            result.Add(new(info.Name, identities.Count == 1 ? identities[0] : "", info.Monitor.Bounds, info.Work.Bounds, (info.Flags & 1) != 0));
            return true;
        }, 0);
        return result;
    }

    private void WindowChanged(nint hook, uint eventType, nint window, int objectId, int child, uint eventThread, uint time)
    {
        // Ignore caret/child events. Do not query other processes inside this callback.
        if (objectId != 0 || child != 0 || window == 0 || layout is null || Volatile.Read(ref disposed) != 0) return;
        if (dispatcher is { HasShutdownStarted: false } && pending.Add(window))
            dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(() => { pending.Remove(window); ReturnWindow(window); }));
    }

    private void ReturnWindow(nint window)
    {
        try
        {
            var current = layout;
            if (current is null || Volatile.Read(ref disposed) != 0 || !Native.IsWindowVisible(window) || Native.IsIconic(window) ||
                Native.GetAncestor(window, 2) != window || !Native.GetWindowRect(window, out var rectangle)) return;
            var visible = rectangle;
            if (Native.DwmGetWindowAttribute(window, 9, out Native.Rect frame, Marshal.SizeOf<Native.Rect>()) == 0) visible = frame;
            if (visible.Bounds.Overlap(current.Device.Bounds) == 0) return;
            if (Native.DwmGetWindowAttribute(window, 14, out int cloaked, sizeof(int)) == 0 && cloaked != 0) return;
            var className = new StringBuilder(256);
            Native.GetClassName(window, className, className.Capacity);
            if (className.ToString() is "Progman" or "WorkerW" or "Shell_TrayWnd" or "Shell_SecondaryTrayWnd" or "#32768") return;
            Native.GetWindowThreadProcessId(window, out uint processId);
            using (var process = Process.GetProcessById((int)processId))
                if (process.ProcessName.Equals("vrcompositor", StringComparison.OrdinalIgnoreCase)) return;
            long now = Environment.TickCount64;
            if (moved.TryGetValue(window, out long previous) && now - previous < 250) return;
            var destination = current.RedirectWindow(rectangle.Bounds, Native.IsZoomed(window));
            if (destination is not { } bounds) return;
            moved[window] = now;
            // Async placement never waits for an unresponsive application's input queue.
            Native.SetWindowPos(window, 0, bounds.Left, bounds.Top, bounds.Width, bounds.Height, 0x4000 | 0x0004 | 0x0010 | 0x0200);
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        { /* The window may disappear or belong to an elevated/system process. */ }
    }

    private nint MouseChanged(int code, nint message, nint data)
    {
        if (code >= 0 && message == 0x0200 && Volatile.Read(ref disposed) == 0 && layout is not null &&
            RedirectPointer(Marshal.PtrToStructure<Native.Point>(data))) return 1;
        return Native.CallNextHookEx(0, code, message, data);
    }

    private bool RedirectPointer(Native.Point point)
    {
        if (layout?.RedirectPointer(point.X, point.Y) is not { } destination) return false;
        return Native.SetCursorPos(destination.X, destination.Y);
    }

    private void Status(string value)
    {
        if (value == lastStatus || Volatile.Read(ref disposed) != 0) return;
        lastStatus = value;
        report(value);
    }

    private void ReleaseHooks()
    {
        if (locationHook != 0) Native.UnhookWinEvent(locationHook);
        if (showHook != 0) Native.UnhookWinEvent(showHook);
        if (mouseHook != 0) Native.UnhookWindowsHookEx(mouseHook);
        locationHook = showHook = mouseHook = 0;
        moved.Clear();
        pending.Clear();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        var loop = Volatile.Read(ref dispatcher);
        if (loop is not null && !loop.HasShutdownStarted) loop.BeginInvokeShutdown(DispatcherPriority.Send);
        thread.Join(1500);
        GC.KeepAlive(callback); GC.KeepAlive(mouseCallback);
    }

    private static class Native
    {
        [StructLayout(LayoutKind.Sequential)]
        internal readonly record struct Point(int X, int Y);
        [StructLayout(LayoutKind.Sequential)]
        internal struct Rect
        {
            public int Left, Top, Right, Bottom;
            public readonly ScreenBounds Bounds => new(Left, Top, Right, Bottom);
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct MonitorInfo
        {
            public int Size; public Rect Monitor, Work; public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string? Name;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct DisplayDevice
        {
            public uint Size;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string? Name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string? Label;
            public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string? Id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string? Key;
        }
        internal delegate void WinEvent(nint hook, uint eventType, nint window, int objectId, int child, uint thread, uint time);
        internal delegate nint MouseHook(int code, nint message, nint data);
        internal delegate bool EnumWindow(nint window, nint data);
        internal delegate bool EnumMonitor(nint monitor, nint context, nint rectangle, nint data);
        [DllImport("user32.dll")] internal static extern nint SetWinEventHook(uint first, uint last, nint module, WinEvent callback, uint process, uint thread, uint flags);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool UnhookWinEvent(nint hook);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern nint SetWindowsHookEx(int kind, MouseHook callback, nint module, uint thread);
        [DllImport("user32.dll")] internal static extern nint CallNextHookEx(nint hook, int code, nint message, nint data);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool UnhookWindowsHookEx(nint hook);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] internal static extern nint GetModuleHandle(string? name);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool EnumWindows(EnumWindow callback, nint data);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool IsWindowVisible(nint window);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool IsIconic(nint window);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool IsZoomed(nint window);
        [DllImport("user32.dll")] internal static extern nint GetAncestor(nint window, uint flags);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool GetWindowRect(nint window, out Rect rectangle);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern int GetClassName(nint window, StringBuilder name, int size);
        [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint window, out uint process);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool EnumDisplayMonitors(nint context, nint rectangle, EnumMonitor callback, nint data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool EnumDisplayDevices(string name, uint index, ref DisplayDevice device, uint flags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool GetCursorPos(out Point point);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool SetCursorPos(int x, int y);
        [DllImport("dwmapi.dll")] internal static extern int DwmGetWindowAttribute(nint window, uint attribute, out Rect value, int size);
        [DllImport("dwmapi.dll")] internal static extern int DwmGetWindowAttribute(nint window, uint attribute, out int value, int size);
    }
}
