using System.Diagnostics;
using System.Windows;
using XrGate.Launcher.Core;

namespace XrGate.Launcher;

public partial class MainWindow
{
    private FirstLoginSession? firstLoginSession;
    private CancellationTokenSource? firstLoginCancellation;
    private Task? firstLoginWaitTask;
    private Func<FirstLoginSession, CancellationToken, Task<bool>>? smokeFirstLoginWait;

    private void ShowSetupPrompt(string message)
    {
        SetupNoticeText.Text = message; // Keep the shell command exit lowercase.
        SetupNoticeText.Visibility = Visibility.Visible;
        LogCalibration(new CalibrationNotice(SetupNoticeText.Text));
    }

    private void CancelFirstLoginWait()
    {
        var pending = firstLoginCancellation;
        firstLoginCancellation = null;
        pending?.Cancel();
    }

    private void StartFirstLoginWait(FirstLoginSession session)
    {
        CancelFirstLoginWait();
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(deviceCancellation.Token);
        cancellation.CancelAfter(TimeSpan.FromSeconds(settings.FirstLoginTimeoutSeconds));
        firstLoginCancellation = cancellation;
        firstLoginWaitTask = ContinueAfterFirstLoginAsync(session, settings.Distribution, cancellation);
    }

    private async Task ContinueAfterFirstLoginAsync(FirstLoginSession session, string distribution, CancellationTokenSource cancellation)
    {
        // Publish the wait task before a process that has already exited can
        // continue setup. Setup may itself request shutdown for a real reboot.
        await Task.Yield();
        bool continueInstallation = false;
        try
        {
            bool completed;
            if (smokeFirstLoginWait is not null) completed = await smokeFirstLoginWait(session, cancellation.Token);
            else
            {
                using var process = Process.GetProcessById(session.ProcessId);
                _ = process.Handle;
                if (process.StartTime.ToUniversalTime().Ticks != session.StartTicks) throw new InvalidOperationException("The Ubuntu login window changed.");
                await process.WaitForExitAsync(cancellation.Token);
                completed = process.ExitCode == 0;
            }
            if (closing || cancellation.IsCancellationRequested) return;
            if (distribution != settings.Distribution)
            {
                ShowSetupPrompt("SELECT " + distribution + " IN SETTINGS, THEN CLICK INSTALL TO CONTINUE.");
                return;
            }
            if (!completed) throw new InvalidOperationException("The Ubuntu login window closed before setup was confirmed.");
            ShowSetupPrompt("UBUNTU LOGIN FINISHED. CONTINUING INSTALLATION...");
            continueInstallation = true;
        }
        catch (OperationCanceledException)
        {
            if (!closing && firstLoginCancellation == cancellation && !deviceCancellation.IsCancellationRequested)
                ShowSetupPrompt("FINISH YOUR UBUNTU USERNAME AND PASSWORD, TYPE exit, THEN CLICK INSTALL TO CONTINUE.");
        }
        catch (Exception error)
        {
            if (!closing)
            {
                Log("[Setup] Automatic continuation paused: " + error.Message);
                ShowSetupPrompt("FINISH YOUR UBUNTU USERNAME AND PASSWORD, TYPE exit, THEN CLICK INSTALL TO CONTINUE.");
            }
        }
        finally
        {
            if (firstLoginCancellation == cancellation)
            {
                firstLoginCancellation = null;
                firstLoginWaitTask = null;
            }
            cancellation.Dispose();
        }
        if (continueInstallation && !closing)
        {
            if (busyAction.Length > 0 || configurationProblem is not null || runtimeMayBeRunning)
                ShowSetupPrompt("UBUNTU LOGIN FINISHED. FINISH THE CURRENT ACTION, THEN CLICK INSTALL TO CONTINUE.");
            else await BeginInstallationAsync(confirmed: true, firstLoginContinuation: true);
        }
    }
}
