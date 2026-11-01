// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Globalization;
using System.ComponentModel;
using System.Security.Cryptography.X509Certificates;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Dispatching;
using PinyinFlux.App.Services;
using Windows.ApplicationModel.DataTransfer;

namespace PinyinFlux.App.ViewModels;

public enum CoreState
{
    /// <summary>The certificate isn't installed yet: nothing has been changed on the system.</summary>
    NotInstalled,
    Stopped,
    Running,
    /// <summary>Running, but the proxy setting isn't pointing at us (another tool took it over).</summary>
    Degraded,
}

/// <summary>State of PinyinFluxCore.exe, polled from its /status endpoint while the window is active.</summary>
public sealed partial class CoreViewModel : ObservableObject
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);

    private readonly CoreClient _core;
    private readonly Func<int> _port;
    private readonly Func<bool> _nativeMode;
    private readonly DispatcherQueueTimer _timer;
    private bool _polling;

    public CoreViewModel(CoreClient core, Func<int> port, Func<bool> nativeMode, DispatcherQueue dispatcher)
    {
        _core = core;
        _port = port;
        _nativeMode = nativeMode;
        _timer = dispatcher.CreateTimer();
        _timer.Interval = PollInterval;
        _timer.Tick += async (_, _) => await RefreshAsync();
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Title), nameof(Summary), nameof(IsRunning), nameof(IsInstalled), nameof(CanRetry), nameof(ShowCloudSwitchOff))]
    public partial CoreState State { get; set; } = CoreState.NotInstalled;

    [ObservableProperty]
    public partial CoreStatus? Status { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(SetupCommand), nameof(StopCommand), nameof(UninstallCommand), nameof(RestartElevatedCommand),
                                nameof(RestartCommand))]
    public partial bool IsBusy { get; set; }

    /// <summary>Output of the last install / uninstall, shown under the buttons.</summary>
    [ObservableProperty]
    public partial string Message { get; set; } = "";

    [ObservableProperty]
    public partial string TestInput { get; set; } = "";

    /// <summary>Progress or failure of a connection mode switch, shown under the mode in settings.</summary>
    [ObservableProperty]
    public partial string ModeMessage { get; set; } = "";

    /// <summary>Feedback of "复制诊断信息".</summary>
    [ObservableProperty]
    public partial string DiagnosticsMessage { get; set; } = "";

    /// <summary>Microsoft Pinyin is one of the user's input methods; re-read on every poll.</summary>
    [ObservableProperty]
    public partial bool PinyinAdded { get; set; } = true;

    /// <summary>Microsoft Pinyin's own cloud candidate switch is on; re-read on every poll.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowCloudSwitchOff))]
    public partial bool PinyinCloudEnabled { get; set; } = true;

    /// <summary>Set up once, but the switch was turned off afterwards (before setup, one click turns it on anyway).</summary>
    public bool ShowCloudSwitchOff => !PinyinCloudEnabled && IsInstalled;

    public bool CanEnableCloudCandidates => !MicrosoftPinyin.CloudCandidatesBlocked;

    public string CloudSwitchOffMessage => MicrosoftPinyin.CloudCandidatesBlocked
        ? "云词显示在云候选的位置，但它已被单位的管理员策略关闭，无法在这里打开。"
        : "云词显示在微软拼音云候选的位置，关闭后不会出现。";

    public ObservableCollection<string> TestResults { get; } = [];

    [ObservableProperty]
    public partial string TestMessage { get; set; } = "";

    public bool IsRunning => State is CoreState.Running or CoreState.Degraded;

    public bool IsInstalled => State != CoreState.NotInstalled;

    public bool CoreExists => CoreClient.CoreExists;
    public bool NeedsElevation => Status is { NativeMode: true, NativeDenied: > 0 };

    /// <summary>Running but not working, and starting over may help (as opposed to granting rights).</summary>
    public bool CanRetry => State == CoreState.Degraded && !NeedsElevation;

    public bool IsNativeMode => Status?.NativeMode == true;

    public bool IsCompatibilityMode => Status is { NativeMode: false };

    public string Title => State switch
    {
        CoreState.Running => IsNativeMode ? "正在工作" : "正在工作（兼容模式）",
        CoreState.Degraded => !IsNativeMode ? "系统代理被其他程序占用"
            : Status!.NativeUnsupported > 0 ? "暂不支持这个 Windows 版本"
            : Status.NativeDenied > 0 ? "需要管理员权限"
            : Status.NativeErrors > 0 ? "接入微软拼音失败"
            : "等待微软拼音",
        CoreState.Stopped => "已停止",
        _ => "尚未启用",
    };

    public string Summary => State switch
    {
        CoreState.Running => IsNativeMode
            ? "用微软拼音打字时，停顿片刻，本地没有的词会出现在候选栏第 2 位，带云朵图标。"
            : "微软拼音向 Bing 查询云候选时，PinyinFlux 会换成百度、搜狗的结果；查询时机由微软拼音决定。",
        CoreState.Degraded => !IsNativeMode
            ? "其他代理软件改写了系统代理，微软拼音的云请求没有经过 PinyinFlux。关闭那个软件后点“重新启动”，或在设置中改用“直接接入”。"
            : Status!.NativeUnsupported > 0 ? "没能确认本机微软拼音的内部结构，为安全起见没有接入，输入法照常可用。首次使用需要联网从微软下载约 10 MB 的公开调试信息，联网后点“重新启动”；仍不行请到 GitHub 反馈。"
            : Status.NativeDenied > 0 ? "Windows 不允许 PinyinFlux 接入输入法。点“授予权限”，在弹出的窗口中选“是”。只需一次，以后开机会自动以该权限启动。"
            : Status.NativeErrors > 0 ? $"接入输入法时出错（Windows 错误 {Status.NativeLastError}）。点“重新启动”再试；仍不行请在“设置 → 诊断”中复制诊断信息并反馈。"
            : "切换到微软拼音并打几个字，PinyinFlux 就会接入。",
        CoreState.Stopped => "微软拼音照常可用，只是不会出现百度、搜狗的云词。",
        _ => "点“一键启用”即可：自动打开微软拼音的云候选、设为开机启动并在后台运行，Windows 询问权限时选“是”。不安装证书，不改动系统代理。",
    };

    public string CloudRequests => Format(Status?.CloudRequests);
    public string Answered => Status is { Queries: > 0 } s ? $"{100.0 * s.Answered / s.Queries:0}%" : "—";
    public string CacheHits => Status is { Queries: > 0 } s ? $"{100.0 * s.CacheHits / s.Queries:0}%" : "—";
    public string Latency => Status is { } s && s.Queries > s.CacheHits ? $"{s.AvgLatencyMs} ms" : "—";
    public string Memory => Status is { WorkingSetKB: > 0 } s ? $"{s.WorkingSetKB / 1024.0:0.0} MB" : "—";
    public string LastQuery => string.IsNullOrEmpty(Status?.LastQuery) ? "—" : Status.LastQuery;
    public string Version => Status?.Version is { Length: > 0 } v ? v : "—";
    public string CertState => Status is null ? "—" : Status.CertTrusted ? "已信任" : "未信任";
    public string ProxyState => Status is null ? "—" : Status.ProxyApplied ? "已指向 PinyinFlux（仅 Bing 的请求）" : "未指向 PinyinFlux";
    public string ImeSessions => Status?.NativeMode == true ? Format(Status.NativeReady) : Format(Status?.ImeSessions);
    public string NativeRequests => Format(Status?.NativeRequests);

    partial void OnStatusChanged(CoreStatus? value)
    {
        foreach (string name in new[] { nameof(CloudSwitchOffMessage), nameof(CanEnableCloudCandidates), nameof(CloudRequests), nameof(Answered), nameof(CacheHits), nameof(Latency), nameof(Memory),
                                        nameof(LastQuery), nameof(Version), nameof(CertState), nameof(ProxyState), nameof(ImeSessions),
                                        nameof(NativeRequests), nameof(NeedsElevation), nameof(CanRetry), nameof(IsNativeMode),
                                        nameof(IsCompatibilityMode), nameof(Title), nameof(Summary) })
        {
            OnPropertyChanged(name);
        }
    }

    public void StartPolling()
    {
        if (_polling) return;
        _polling = true;
        _ = RefreshAsync();
        _timer.Start();
    }

    public void StopPolling()
    {
        _polling = false;
        _timer.Stop();
    }

    public async Task RefreshAsync()
    {
        PinyinAdded = MicrosoftPinyin.IsAdded;
        PinyinCloudEnabled = MicrosoftPinyin.CloudCandidatesEnabled;
        var status = await _core.GetStatusAsync(_port());
        Status = status;
        State = status is null
            ? (CoreClient.StartsWithWindows || IsCertificateInstalled() ? CoreState.Stopped : CoreState.NotInstalled)
            : (status.NativeMode ? status.NativeReady > 0 : status.ProxyApplied) ? CoreState.Running : CoreState.Degraded;
    }

    /// <summary>
    /// "一键启用" / "启动": everything that can be done for the user. Turns Microsoft Pinyin's cloud candidates on, starts
    /// the core (the first time with --install, which also enables autostart) and, when Windows refuses the injection for
    /// lack of rights, goes straight on to the UAC prompt. Confirming it once also registers the elevated start task, so
    /// later starts need no prompt.
    /// </summary>
    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task SetupAsync()
    {
        IsBusy = true;
        try
        {
            if (!MicrosoftPinyin.CloudCandidatesEnabled && !MicrosoftPinyin.CloudCandidatesBlocked) MicrosoftPinyin.EnableCloudCandidates();
            if (await _core.GetStatusAsync(_port()) is null)
            {
                Message = "正在启动…";
                string error;
                if (State == CoreState.NotInstalled)
                {
                    var (code, output) = await CoreClient.RunVerbAsync("--install");
                    error = code == 0 ? "" : output;
                }
                else
                {
                    error = await StartCoreAsync();
                }
                if (error.Length > 0)
                {
                    Message = error;
                    return;
                }
            }
            // The core tries the input host right away; give it a few seconds to report success or refusal.
            for (int i = 0; i < 12; i++)
            {
                await Task.Delay(500);
                await RefreshAsync();
                if (Status is { NativeMode: false } or { NativeReady: > 0 } or { NativeUnsupported: > 0 } || NeedsElevation) break;
            }
            Message = "";
        }
        finally
        {
            IsBusy = false;
        }
        if (NeedsElevation) await RestartElevatedAsync();
    }

    [RelayCommand]
    private async Task EnableCloudCandidatesAsync()
    {
        MicrosoftPinyin.EnableCloudCandidates();
        await RefreshAsync();
    }

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task StopAsync()
    {
        IsBusy = true;
        try
        {
            await CoreClient.RunVerbAsync("--stop");
            await Task.Delay(500);
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
    }

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task UninstallAsync()
    {
        IsBusy = true;
        try
        {
            var (_, output) = await CoreClient.RunVerbAsync("--uninstall");
            Message = output;
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
    }

    private bool CanRunVerb() => !IsBusy && CoreClient.CoreExists;

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task RestartAsync()
    {
        IsBusy = true;
        try
        {
            await CoreClient.RunVerbAsync("--stop");
            Message = await StartCoreAsync();
            await Task.Delay(800);
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
    }

    /// <summary>
    /// The connection mode was changed in settings. A running core exits by itself when it sees the change;
    /// start it again in the new mode (proxy mode first creates and trusts its certificate).
    /// </summary>
    public async Task ApplyModeChangeAsync(bool native)
    {
        if (IsBusy) return;
        if (!IsRunning)
        {
            ModeMessage = State == CoreState.NotInstalled ? "" : "已保存，下次启动时生效。";
            return;
        }
        IsBusy = true;
        ModeMessage = native ? "正在切换到直接接入…" : "正在切换到兼容模式，Windows 可能会询问是否安装 PinyinFlux 证书…";
        try
        {
            for (int i = 0; i < 20 && await _core.GetStatusAsync(_port()) is not null; i++) await Task.Delay(250);
            ModeMessage = await StartCoreAsync();
            await Task.Delay(800);
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
    }

    /// <summary>
    /// Starts the core in the configured mode. Proxy mode needs its certificate trusted first, which only --install does;
    /// --install also turns autostart on, so the user's choice is put back. Returns the core's output on failure.
    /// </summary>
    private async Task<string> StartCoreAsync()
    {
        if (_nativeMode())
        {
            CoreClient.Start();
            return "";
        }
        bool autostart = CoreClient.StartsWithWindows;
        var (code, output) = await CoreClient.RunVerbAsync("--install");
        CoreClient.StartsWithWindows = autostart;
        return code == 0 ? "" : output;
    }

    /// <summary>Puts what a bug report needs on the clipboard: versions, Microsoft Pinyin setup and the core's full status.</summary>
    [RelayCommand]
    private async Task CopyDiagnosticsAsync()
    {
        string status = await _core.GetRawStatusAsync(_port()) ?? "(core not running)";
        string text = $"{Views.AboutPage.SystemInfoText}\nMicrosoft Pinyin: {(MicrosoftPinyin.IsAdded ? "added" : "not added")}, "
            + $"cloud candidates {(MicrosoftPinyin.CloudCandidatesEnabled ? "on" : "off")}\nStatus: {status}";
        try
        {
            var package = new DataPackage();
            package.SetText(text);
            Clipboard.SetContent(package);
            DiagnosticsMessage = "已复制。反馈问题时粘贴到 GitHub Issue 中即可；其中包含最近一次查询的拼音。";
        }
        catch (System.Runtime.InteropServices.COMException)
        {
            DiagnosticsMessage = "剪贴板被其他程序占用，请稍后再试。";
        }
    }

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task RestartElevatedAsync()
    {
        IsBusy = true;
        try
        {
            var (code, output) = await CoreClient.RunVerbAsync("--stop");
            if (code != 0) { Message = output; return; }
            Message = "请在 Windows 弹出的窗口中选“是”。";
            CoreClient.StartElevated();
            await Task.Delay(700);
            Message = "";
        }
        catch (Win32Exception ex)
        {
            Message = ex.NativeErrorCode == 1223 ? "已取消。需要时可再次点击“授予权限”。" : $"未能以管理员权限启动：{ex.Message}";
        }
        finally
        {
            await RefreshAsync();
            IsBusy = false;
        }
    }

    [RelayCommand]
    private async Task TestAsync()
    {
        TestResults.Clear();
        string input = TestInput.Trim();
        if (input.Length == 0) return;
        if (!IsRunning)
        {
            TestMessage = "PinyinFlux 未运行，请先启动。";
            return;
        }
        var words = await _core.QueryAsync(_port(), input);
        if (words is null)
        {
            TestMessage = "查询失败。";
            return;
        }
        foreach (string w in words) TestResults.Add(w);
        TestMessage = words.Count == 0 ? "云词库没有给出完整匹配这串拼音的词。" : "";
        await RefreshAsync();
    }

    /// <summary>Installed = our certificate is in CurrentUser\Root (what --install adds and --uninstall removes).</summary>
    private static bool IsCertificateInstalled()
    {
        using var store = new X509Store(StoreName.Root, StoreLocation.CurrentUser);
        store.Open(OpenFlags.ReadOnly);
        var certificates = store.Certificates;  // a new collection on every access
        bool found = certificates.Any(c => c.Subject.Contains("O=PinyinFlux", StringComparison.Ordinal));
        foreach (var c in certificates) c.Dispose();
        return found;
    }

    private static string Format(long? n) => n is { } v ? v.ToString("N0", CultureInfo.CurrentCulture) : "—";
}
