// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Win32;
using Windows.ApplicationModel.DataTransfer;

namespace PinyinFlux.App.Views;

/// <summary>Version, how it works, developer, sponsorship, feedback and acknowledgements.</summary>
public sealed partial class AboutPage : Page
{
    private DispatcherQueueTimer? _copiedTimer;

    public AboutPage()
    {
        InitializeComponent();
        Unloaded += (_, _) => _copiedTimer?.Stop();
    }

    public static string Version { get; } = Assembly.GetExecutingAssembly()
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0] ?? "?";

    public string AppVersion => "v" + Version;

    /// <summary>What an issue report needs: app version, Windows build, architecture and runtime. Not localized, so it pastes the same everywhere.</summary>
    public string SystemInfo => SystemInfoText;

    public static string SystemInfoText { get; } = $"PinyinFlux {Version} ({Arch(RuntimeInformation.ProcessArchitecture)}) · "
        + $"Windows {WindowsBuild()} {Arch(RuntimeInformation.OSArchitecture)} · .NET {Environment.Version}";

    private static string Arch(Architecture architecture) => architecture.ToString().ToLowerInvariant();

    private static string WindowsBuild()
    {
        var version = Environment.OSVersion.Version;
        try
        {
            // The update revision (UBR) tells cumulative updates apart; Environment.OSVersion leaves it out.
            if (Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion", "UBR", null) is int ubr)
                return $"{version.Major}.{version.Minor}.{version.Build}.{ubr}";
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or IOException or UnauthorizedAccessException) { }
        return version.ToString(3);
    }

    private void OnCopySystemInfo(object sender, RoutedEventArgs e)
    {
        try
        {
            var package = new DataPackage();
            package.SetText(SystemInfo);
            Clipboard.SetContent(package);
        }
        catch (COMException)
        {
            // Another app holds the clipboard; the text stays on screen to copy by hand.
            return;
        }
        CopyInfo.Content = "已复制";
        if (_copiedTimer is null)
        {
            _copiedTimer = DispatcherQueue.CreateTimer();
            _copiedTimer.Interval = TimeSpan.FromSeconds(2);
            _copiedTimer.IsRepeating = false;
            _copiedTimer.Tick += (_, _) => CopyInfo.Content = "复制";
        }
        _copiedTimer.Stop();
        _copiedTimer.Start();
    }

    private void OnOpenLicense(object sender, RoutedEventArgs e)
    {
        string path = Path.Combine(AppContext.BaseDirectory, "LICENSE");
        if (File.Exists(path)) Process.Start(new ProcessStartInfo("notepad.exe", $"\"{path}\"") { UseShellExecute = false })?.Dispose();
    }
}
