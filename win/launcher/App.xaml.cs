using System.Security.Cryptography;
using System.Text;
using System.Windows;

namespace XrGate.Launcher;

public partial class App : Application
{
    private Mutex? instance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        try
        {
            bool smoke = e.Args.Contains("--smoke-test");
            int rootIndex = Array.IndexOf(e.Args, "--bundle-root");
            string root = rootIndex >= 0 && rootIndex + 1 < e.Args.Length
                ? Path.GetFullPath(e.Args[rootIndex + 1]) : AppContext.BaseDirectory;
            if (!smoke)
            {
                string user = Environment.UserDomainName + "\\" + Environment.UserName;
                string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(user)))[..20];
                instance = new Mutex(true, @"Local\XR-Gate-Launcher-" + hash, out bool first);
                if (!first)
                {
                    MessageBox.Show("XR Gate Launcher is already open for this Windows user.", "XR Gate");
                    Shutdown(0);
                    return;
                }
            }
            var window = new MainWindow(root, smoke);
            MainWindow = window;
            window.Show();
        }
        catch (Exception exception)
        {
            if (!e.Args.Contains("--smoke-test"))
                MessageBox.Show(exception.Message, "XR Gate could not start", MessageBoxButton.OK, MessageBoxImage.Error);
            else Console.Error.WriteLine(exception);
            Shutdown(1);
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        instance?.Dispose();
        base.OnExit(e);
    }
}
