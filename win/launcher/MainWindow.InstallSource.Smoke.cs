using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Threading;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private async Task VerifyInstallSourceUiAsync()
    {
        var original = settings;
        var tab = MainTabs.SelectedItem;
        bool fieldsEnabled = SettingsFields.IsEnabled;
        try
        {
            SettingsFields.IsEnabled = true;
            settings = LauncherSettings.Load(root, defaultsOnly: true);
            DataContext = settings;
            MainTabs.SelectedItem = SettingsTab;
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (RuntimeUrlBox.Text != settings.RuntimeDownloadUrl || RuntimeUrlBox.Text.Length == 0 || !RuntimeUrlBox.IsEnabled)
                throw new InvalidOperationException("The installation-source URL did not display the current default.");
            RuntimeLocalPathBox.SetCurrentValue(TextBox.TextProperty, "D:\\My local archives");
            RuntimeLocalPathBox.GetBindingExpression(TextBox.TextProperty)?.UpdateSource();
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (RuntimeUrlBox.IsEnabled || settings.RuntimeLocalPath != "D:\\My local archives")
                throw new InvalidOperationException("A local source did not reach settings or disable the URL.");
            RuntimeLocalPathBox.SetCurrentValue(TextBox.TextProperty, "");
            RuntimeLocalPathBox.GetBindingExpression(TextBox.TextProperty)?.UpdateSource();
            await Dispatcher.Yield(DispatcherPriority.DataBind);
            if (!RuntimeUrlBox.IsEnabled) throw new InvalidOperationException("Clearing the local source did not enable the URL.");
            if (ProductDescriptionText.Text != "6DoF and Hand Tracking Stack" || PlatformDescriptionText.Text != "Windows launcher" ||
                ProjectLink.NavigateUri.AbsoluteUri != "https://github.com/vladoshub/xr-gate")
                throw new InvalidOperationException("The product labels or project link are incorrect.");
            FlushLog();
            Log("[Tracking setup] Keep device cameras uncovered and ensure good lighting.");
            FlushLog();
            var instruction = (Paragraph)LogBox.Document.Blocks.LastBlock;
            if (instruction.FontSize < 18 || instruction.FontWeight != FontWeights.Bold || instruction.Foreground.ToString() != "#FFFFB454")
                throw new InvalidOperationException("Head-tracking setup instructions must be large and orange.");
            reconnectHintShown = false;
            int before = LogBox.Document.Blocks.Count;
            Log("usbipd: Device busy (exported)");
            Log("usbipd: Device busy (exported)");
            FlushLog();
            var entries = LogBox.Document.Blocks.Skip(before).Cast<Paragraph>();
            if (entries.Count(entry => new TextRange(entry.ContentStart, entry.ContentEnd).Text.Contains(ActivityMessages.ReconnectHint)) != 1)
                throw new InvalidOperationException("Hardware recovery advice must appear once per operation.");
        }
        finally
        {
            settings = original;
            DataContext = original;
            MainTabs.SelectedItem = tab;
            SettingsFields.IsEnabled = fieldsEnabled;
            reconnectHintShown = false;
        }
    }
}
