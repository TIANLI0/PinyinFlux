// SPDX-License-Identifier: MIT
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Dispatching;
using PinyinFlux.App.Services.Updates;

namespace PinyinFlux.App.ViewModels;

/// <summary>Update state for the settings card and the "ready to install" bar of the main window.</summary>
public sealed partial class UpdateViewModel : ObservableObject
{
    private readonly UpdateManager _updates;
    private readonly DispatcherQueue _dispatcher;
    private readonly Action _exit;
    private int _queued;

    /// <param name="exit">Closes the app after the installer has been started, so it can replace the files.</param>
    public UpdateViewModel(UpdateManager updates, DispatcherQueue dispatcher, Action exit)
    {
        _updates = updates;
        _dispatcher = dispatcher;
        _exit = exit;
        _updates.StatusChanged += (_, _) =>
        {
            if (Interlocked.Exchange(ref _queued, 1) == 0)
                _dispatcher.TryEnqueue(() => { Volatile.Write(ref _queued, 0); Apply(); });
        };
        Apply();
    }

    public string CurrentVersion => _updates.CurrentVersion.ToString();

    public string VersionHeader => $"当前版本 {CurrentVersion}";

    [ObservableProperty] public partial string StatusText { get; set; } = "";

    [ObservableProperty] public partial bool IsDownloading { get; set; }

    [ObservableProperty] public partial double Progress { get; set; }

    [ObservableProperty] public partial bool IsActionVisible { get; set; }

    [ObservableProperty] public partial string ActionText { get; set; } = "下载并安装";

    /// <summary>A verified installer is on disk: the main window offers it.</summary>
    [ObservableProperty] public partial bool IsReady { get; set; }

    [ObservableProperty] public partial string ReadyTitle { get; set; } = "";

    [ObservableProperty] public partial string ReleaseNotes { get; set; } = "";

    [ObservableProperty] public partial bool HasReleaseNotes { get; set; }

    [ObservableProperty] public partial Uri? ReleaseNotesUrl { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(CheckCommand), nameof(RunActionCommand))]
    public partial bool IsIdle { get; set; } = true;

    private void Apply()
    {
        var status = _updates.Status;
        string? version = status.Release?.Version.ToString();
        string text = status.Stage switch
        {
            UpdateStage.Checking => "正在检查更新…",
            UpdateStage.UpToDate => "已是最新版本。",
            UpdateStage.Available => $"发现新版本 {version}。",
            UpdateStage.Downloading => $"正在下载 {version}（{Bytes(status.Received)} / {Bytes(status.Total)}）…",
            UpdateStage.Ready => $"新版本 {version} 已下载并校验，可以安装。",
            UpdateStage.Installing => "正在启动安装程序…",
            UpdateStage.Failed => status.Failure switch
            {
                UpdateFailure.Offline => "无法连接 GitHub，请检查网络。",
                UpdateFailure.RateLimited => "GitHub 请求过于频繁，请稍后再试。",
                UpdateFailure.NotFound => "未找到发布信息。",
                UpdateFailure.Verification => "下载的安装包未通过校验，已丢弃。",
                UpdateFailure.Storage => "无法保存或启动安装程序。",
                _ => "检查更新失败。",
            },
            _ => status.LastChecked is null ? "尚未检查更新。" : "",
        };
        if (!status.IsBusy && status.LastChecked is { } last)
            text = (text.Length > 0 ? text + " " : "") + $"上次检查：{last.ToLocalTime().ToString("g", CultureInfo.CurrentCulture)}";
        StatusText = text;
        IsDownloading = status.Stage == UpdateStage.Downloading;
        Progress = status.Total > 0 ? Math.Clamp(100.0 * status.Received / status.Total, 0, 100) : 0;
        // A failed download keeps its release, so the same button retries it.
        bool canDownload = status.Release is not null && status.Stage is UpdateStage.Available or UpdateStage.Failed;
        IsActionVisible = canDownload || status.Stage == UpdateStage.Ready;
        ActionText = status.Stage == UpdateStage.Ready ? "立即安装" : "下载并安装";
        IsReady = status.Stage == UpdateStage.Ready;
        ReadyTitle = $"PinyinFlux {version} 已准备好";
        ReleaseNotesUrl = status.Release?.NotesUrl;
        ReleaseNotes = status.Release?.Notes ?? "";
        HasReleaseNotes = ReleaseNotes.Length > 0;
        IsIdle = !status.IsBusy;
    }

    [RelayCommand(CanExecute = nameof(IsIdle))]
    private async Task CheckAsync()
    {
        IsIdle = false;
        await _updates.CheckNowAsync();
        Apply();
    }

    /// <summary>"Download and install" (downloads, then installs) or "Install now" (installs the downloaded version).</summary>
    [RelayCommand(CanExecute = nameof(IsIdle))]
    private async Task RunActionAsync()
    {
        IsIdle = false;
        try
        {
            if (_updates.Status.Stage != UpdateStage.Ready && await _updates.DownloadAsync() is null) return;
            // The installer replaces this app's files: leave once it has started.
            if (_updates.TryStartInstaller()) _exit();
        }
        finally { Apply(); }
    }

    private static string Bytes(long value) => value >= 1024 * 1024
        ? $"{value / 1024.0 / 1024.0:0.0} MB"
        : $"{value / 1024.0:0} KB";
}
