// SPDX-License-Identifier: MIT
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using PinyinFlux.App.Services;
using PinyinFlux.App.ViewModels;
using PinyinFlux.App.Views;

namespace PinyinFlux.App;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    public static App Instance => (App)Current;

    public CoreClient Core { get; } = new();

    public CoreViewModel Status { get; private set; } = null!;

    public SettingsViewModel Settings { get; private set; } = null!;

    /// <summary>Owner window for pickers and dialogs.</summary>
    public MainWindow Window => _window!;

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        Settings = new SettingsViewModel(CoreConfig.Load());
        Status = new CoreViewModel(Core, () => Settings.Port, dispatcher);

        _window = new MainWindow();
        _window.Activated += (_, e) =>
        {
            // Poll the core only while the window is in front; nothing runs while it's in the background.
            if (e.WindowActivationState == WindowActivationState.Deactivated) Status.StopPolling();
            else Status.StartPolling();
        };
        _window.Closed += (_, _) =>
        {
            Status.StopPolling();
            Core.Dispose();
        };
        _window.Activate();

        // --page <Home|Sources|Settings|About>: open on a specific page (shortcuts, screenshots).
        string[] argv = Environment.GetCommandLineArgs();
        int at = Array.IndexOf(argv, "--page");
        if (at >= 0 && at + 1 < argv.Length) _window.Navigate(argv[at + 1]);
    }
}
