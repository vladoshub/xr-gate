using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private const int StartHotkey = 0x5855, StopHotkey = 0x5856, RestartHotkey = 0x5857, WmHotkey = 0x0312;
    private HwndSource? hotkeySource;
    private bool startHotkeyRegistered, stopHotkeyRegistered, restartHotkeyRegistered;

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RegisterHotKey(nint window, int id, uint modifiers, uint key);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterHotKey(nint window, int id);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowEnabled(nint window);

    private void InitializeHotkeys()
    {
        if (smoke) return; // UI fixtures must not reserve the user's keys.
        hotkeySource = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
        hotkeySource?.AddHook(HotkeyMessage);
        ConfigureHotkeys();
    }

    private void UnregisterHotkeys()
    {
        if (hotkeySource is not null)
        {
            if (startHotkeyRegistered) UnregisterHotKey(hotkeySource.Handle, StartHotkey);
            if (stopHotkeyRegistered) UnregisterHotKey(hotkeySource.Handle, StopHotkey);
            if (restartHotkeyRegistered) UnregisterHotKey(hotkeySource.Handle, RestartHotkey);
        }
        startHotkeyRegistered = stopHotkeyRegistered = restartHotkeyRegistered = false;
    }

    private void ConfigureHotkeys()
    {
        UnregisterHotkeys();
        if (!settings.GlobalHotkeys)
        {
            HotkeysStatus.Text = "Global hotkeys are disabled. Changes are saved automatically.";
            return;
        }
        if (smoke || hotkeySource is null) return;
        var status = new List<string>();
        bool Register(int id, uint key, string label)
        {
            // MOD_NOREPEAT avoids repeatedly starting/stopping while held down.
            if (RegisterHotKey(hotkeySource.Handle, id, 0x4000, key)) { status.Add(label + ": ready"); return true; }
            string error = new Win32Exception(Marshal.GetLastWin32Error()).Message;
            status.Add(label + ": unavailable");
            Log("[Hotkeys] " + label + " could not be registered: " + error + ". The on-screen buttons remain available.");
            return false;
        }
        startHotkeyRegistered = Register(StartHotkey, 0x74, "F5");
        stopHotkeyRegistered = Register(StopHotkey, 0x75, "F6");
        restartHotkeyRegistered = Register(RestartHotkey, 0x76, "F7");
        HotkeysStatus.Text = string.Join(". ", status) + ". Work while XR Gate is open, including when minimized. If a key is occupied, close the other application and toggle this checkbox to retry.";
    }

    private void GlobalHotkeys_Click(object sender, RoutedEventArgs e)
    {
        GlobalHotkeysBox.GetBindingExpression(CheckBox.IsCheckedProperty)?.UpdateSource();
        try
        {
            ConfigureHotkeys();
            if (!smoke) settings.SaveHotkeySettings(root);
        }
        catch (Exception error) { HotkeysStatus.Text += " Hotkeys setting could not be saved: " + error.Message; Log("[Hotkeys] " + error.Message); }
    }

    private nint HotkeyMessage(nint window, int message, nint parameter, nint data, ref bool handled)
    {
        int id = unchecked((int)parameter);
        if (message != WmHotkey || !(id == StartHotkey && startHotkeyRegistered || id == StopHotkey && stopHotkeyRegistered || id == RestartHotkey && restartHotkeyRegistered)) return 0;
        handled = true;
        // A modal setup/confirmation dialog disables its owner. Do not run VR
        // behind that dialog; minimized windows remain enabled.
        if (!closing && !resourcesReleased && IsWindowEnabled(window)) _ = HandleHotkeyAsync(id);
        return 0;
    }

    private async Task HandleHotkeyAsync(int id)
    {
        if (!settings.GlobalHotkeys || closing || resourcesReleased) return;
        try
        {
            if (id == StartHotkey) await StartVrAsync();
            else if (id == StopHotkey && StopButton.IsEnabled) await StopSessionAsync();
            else if (id == RestartHotkey) await RestartTrackingAsync();
        }
        catch (Exception error) { StatusDetail.Text = error.Message; Log("[Hotkeys] " + error.Message); }
    }

    private void ReleaseHotkeys()
    {
        UnregisterHotkeys();
        hotkeySource?.RemoveHook(HotkeyMessage);
        hotkeySource = null;
    }
}
