// SPDX-License-Identifier: MIT
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using PinyinFlux.App.Services;
using PinyinFlux.App.Services.Updates;
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

    public UpdateViewModel Updates { get; private set; } = null!;

    private UpdateManager _updates = null!;

    /// <summary>Owner window for pickers and dialogs.</summary>
    public MainWindow Window => _window!;

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        Settings = new SettingsViewModel(CoreConfig.Load());
        Status = new CoreViewModel(Core, () => Settings.Port, dispatcher);
        string updateFolder = Path.Combine(CoreConfig.DataDir, "updates");
        var client = new GitHubUpdateClient(new HttpClient(), updateFolder, SemanticVersion.FromAssembly(typeof(App).Assembly));
        _updates = new UpdateManager(client, () => Settings.AutoUpdate, () => Settings.IncludePrerelease, Path.Combine(updateFolder, "last-check.txt"));
        Updates = new UpdateViewModel(_updates, dispatcher, () => _window?.Close());

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
            // Off the UI thread and bounded: a running check or download is cancelled.
            Task.Run(() => _updates.DisposeAsync().AsTask()).Wait(TimeSpan.FromSeconds(6));
        };
        _window.Activate();
        _updates.Start();

        // --page <Home|Sources|Settings|About>: open on a specific page (shortcuts, screenshots).
        string[] argv = Environment.GetCommandLineArgs();
        int at = Array.IndexOf(argv, "--page");
        if (at >= 0 && at + 1 < argv.Length) _window.Navigate(argv[at + 1]);
        if (argv.Contains(UpdateManager.RelaunchArgument)) _ = RestoreInjectionAsync();
    }

    /// <summary>
    /// After an in-app update the installer (running as the user) has restarted the core without the rights
    /// to inject into the input host. Restart it elevated once, as "提权重启" would.
    /// </summary>
    private async Task RestoreInjectionAsync()
    {
        for (int i = 0; i < 10; i++)
        {
            await Task.Delay(1000);
            await Status.RefreshAsync();
            if (Status.Status is { NativeReady: > 0 } || Status.Status is { NativeMode: false }) return;
            if (Status.NeedsElevation)
            {
                if (Status.RestartElevatedCommand.CanExecute(null)) await Status.RestartElevatedCommand.ExecuteAsync(null);
                return;
            }
        }
    }
}
