// SPDX-License-Identifier: MIT
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;
using PinyinFlux.App.Services;

namespace PinyinFlux.App;

/// <summary>
/// Replaces the XAML-generated Main (DISABLE_XAML_GENERATED_MAIN) so that only one settings window exists:
/// a second launch hands its arguments to the running instance, which comes to the front, and exits.
/// </summary>
public static class Program
{
    private const string InstanceKey = "PinyinFlux.Settings";

    [STAThread]
    private static int Main()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();

        var current = AppInstance.GetCurrent();
        var main = AppInstance.FindOrRegisterForKey(InstanceKey);
        if (!main.IsCurrent)
        {
            // This process was started by the user, so it may hand the foreground to the running window.
            WindowInterop.AllowSetForegroundWindow(main.ProcessId);
            // Off the STA: the redirect is a cross-process call, and a managed wait on the STA keeps pumping.
            Task.Run(() => main.RedirectActivationToAsync(current.GetActivatedEventArgs()).AsTask()).Wait(TimeSpan.FromSeconds(5));
            return 0;
        }

        Application.Start(_ =>
        {
            SynchronizationContext.SetSynchronizationContext(new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            var app = new App();
            main.Activated += (_, args) => app.OnRedirected(args);
        });
        return 0;
    }
}
