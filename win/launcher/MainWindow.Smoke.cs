using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifyUiSmokeAsync()
    {
        if (!smoke) throw new InvalidOperationException("UI fixture is only available in smoke mode.");
        configurationProblem = null;
        UpdateRecoveryUi();
        MainTabs.SelectedItem = SettingsTab;
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        string originalUsbId = settings.BluetoothUsbId;
        var usbIdBinding = BluetoothUsbIdBox.GetBindingExpression(TextBox.TextProperty)
            ?? throw new InvalidOperationException("The Bluetooth VID:PID field has no binding.");
        usbIdBinding.UpdateTarget();
        if (BluetoothUsbIdBox.Text != originalUsbId)
            throw new InvalidOperationException("The Bluetooth VID:PID field did not load its profile value.");
        BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, "abcd:0123");
        usbIdBinding.UpdateSource();
        if (settings.BluetoothUsbId != "abcd:0123")
            throw new InvalidOperationException("Editing Bluetooth VID:PID did not update settings.");
        BluetoothUsbIdBox.SetCurrentValue(TextBox.TextProperty, originalUsbId);
        usbIdBinding.UpdateSource();
        MainTabs.SelectedIndex = 0;
        ApplyBluetoothDevices(JsonSerializer.SerializeToElement(new
        {
            devices = new[]
            {
                new { address = "AA:BB:CC:DD:EE:01", label = "Same controller [AA:BB:CC:DD:EE:01]" },
                new { address = "AA:BB:CC:DD:EE:02", label = "Same controller [AA:BB:CC:DD:EE:02]" }
            },
            selected = new { left = "AA:BB:CC:DD:EE:01", right = (string?)null }
        }));
        await Dispatcher.Yield(DispatcherPriority.DataBind);
        if (LeftBluetoothController.SelectedValue as string != "AA:BB:CC:DD:EE:01" ||
            RightBluetoothController.SelectedValue as string != "none" || LeftBluetoothController.Items.Count != 3)
            throw new InvalidOperationException("The controller picker lost duplicate-name identity or the omitted side.");
        smokeAction = async (action, cancellation) =>
        {
            if (action is "Doctor" or "Test" or "Usb" or "BluetoothScan" or "Pair") await Task.Delay(Timeout.Infinite, cancellation);
            return new ActionResult { ExitCode = 0, Data = JsonSerializer.SerializeToElement(new { installed = true, running = false }) };
        };
        // Exercise the real async action, routed Cancel click, finally block and
        // subsequent action. No PowerShell, WSL, USB or user files are touched.
        foreach (string action in new[] { "Doctor", "Test", "Usb", "BluetoothScan", "Pair" })
        {
            Task<bool> pending = ExecuteAsync(action, saveSettings: false);
            await Dispatcher.Yield(DispatcherPriority.Background);
            if (CancelDiagnosticsButton.Visibility != Visibility.Visible)
                throw new InvalidOperationException(action + " has no usable Cancel button.");
            CancelDiagnosticsButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            if (await pending.WaitAsync(TimeSpan.FromSeconds(5)))
                throw new InvalidOperationException("Cancelled check reported success.");
            if (!ActionPanel.IsEnabled || !ConnectionPanel.IsEnabled || !SettingsPanel.IsEnabled)
                throw new InvalidOperationException("Cancellation left the UI blocked.");
            if (!await ExecuteAsync("Status", saveSettings: false))
                throw new InvalidOperationException("The UI could not start an action after cancellation.");
        }
        Task<bool> closingUsb = ExecuteAsync("Usb", saveSettings: false);
        await Dispatcher.Yield(DispatcherPriority.Background);
        // The close handler uses this same cancellation-and-wait path.
        if (!IsCancellableOperation(busyAction)) throw new InvalidOperationException("USB attach still blocks window closing.");
        await CancelActiveOperationAsync().WaitAsync(TimeSpan.FromSeconds(5));
        if (await closingUsb || busyAction.Length != 0 || !ActionPanel.IsEnabled || !ConnectionPanel.IsEnabled)
            throw new InvalidOperationException("Closing during USB attach left an active operation.");
        int calls = 0;
        smokeAction = (_, _) => { calls++; throw new InvalidOperationException("Recovery invoked a backend action."); };
        configurationProblem = new ConfigurationProblem(Path.Combine(root, ".env"), "fixture configuration error", true);
        UpdateRecoveryUi();
        if (RecoveryPanel.Visibility != Visibility.Visible || !SettingsFields.IsEnabled || ActionPanel.IsEnabled || ConnectionPanel.IsEnabled ||
            await ExecuteAsync("Doctor", saveSettings: false) || calls != 0)
            throw new InvalidOperationException("Configuration recovery did not keep the repair UI available and actions blocked.");
        configurationProblem = null;
        smokeAction = null;
        UpdateRecoveryUi();
    }
}
