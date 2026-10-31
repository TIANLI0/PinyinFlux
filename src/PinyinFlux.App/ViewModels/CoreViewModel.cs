// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Globalization;
using System.ComponentModel;
using System.Security.Cryptography.X509Certificates;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Dispatching;
using PinyinFlux.App.Services;

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
    private readonly DispatcherQueueTimer _timer;
    private bool _polling;

    public CoreViewModel(CoreClient core, Func<int> port, DispatcherQueue dispatcher)
    {
        _core = core;
        _port = port;
        _timer = dispatcher.CreateTimer();
        _timer.Interval = PollInterval;
        _timer.Tick += async (_, _) => await RefreshAsync();
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Title), nameof(Summary), nameof(IsRunning), nameof(IsInstalled))]
    public partial CoreState State { get; set; } = CoreState.NotInstalled;

    [ObservableProperty]
    public partial CoreStatus? Status { get; set; }

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(InstallCommand), nameof(StartCommand), nameof(StopCommand), nameof(UninstallCommand), nameof(RestartElevatedCommand))]
    public partial bool IsBusy { get; set; }

    /// <summary>Output of the last install / uninstall, shown under the buttons.</summary>
    [ObservableProperty]
    public partial string Message { get; set; } = "";

    [ObservableProperty]
    public partial string TestInput { get; set; } = "";

    public ObservableCollection<string> TestResults { get; } = [];

    [ObservableProperty]
    public partial string TestMessage { get; set; } = "";

    public bool IsRunning => State is CoreState.Running or CoreState.Degraded;

    public bool IsInstalled => State != CoreState.NotInstalled;

    public bool CoreExists => CoreClient.CoreExists;
    public bool NeedsElevation => Status is { NativeMode: true, NativeDenied: > 0 };

    public string Title => State switch
    {
        CoreState.Running => Status?.NativeMode == true ? "原生云候选钩子已就绪" : "云词库代理运行中",
        CoreState.Degraded => Status?.NativeMode == true ? "核心运行中，原生注入未就绪" : "正在运行，但代理设置被占用",
        CoreState.Stopped => "已停止",
        _ => "尚未安装",
    };

    public string Summary => State switch
    {
        CoreState.Running => Status?.NativeMode == true ? "已安装原生候选与网络钩子；请实际输入拼音确认云候选显示与选词。" : "替换微软拼音主动发出的云请求，触发频率仍由微软拼音决定。",
        CoreState.Degraded => NativeProblem,
        CoreState.Stopped => "微软拼音的云候选回到 Bing。",
        _ => "默认启用原生注入实验模式，主动请求云候选；不创建证书或改写系统代理。",
    };

    public string CloudRequests => Format(Status?.CloudRequests);
    public string Answered => Status is { Queries: > 0 } s ? $"{100.0 * s.Answered / s.Queries:0}%" : "—";
    public string CacheHits => Status is { Queries: > 0 } s ? $"{100.0 * s.CacheHits / s.Queries:0}%" : "—";
    public string Latency => Status is { } s && s.Queries > s.CacheHits ? $"{s.AvgLatencyMs} ms" : "—";
    public string Memory => Status is { WorkingSetKB: > 0 } s ? $"{s.WorkingSetKB / 1024.0:0.0} MB" : "—";
    public string LastQuery => string.IsNullOrEmpty(Status?.LastQuery) ? "—" : Status.LastQuery;
    public string Version => Status?.Version is { Length: > 0 } v ? v : "—";
    private string NativeProblem => Status?.NativeMode != true
        ? "系统代理未指向 PinyinFlux。"
        : Status.NativeUnsupported > 0 ? "无法确认当前微软拼音版本的挂钩地址（可能需要联网下载微软公开符号），已停止打补丁。"
        : Status.NativeDenied > 0 ? "Windows 拒绝访问输入法宿主（错误 5）。点击“提权重启”并在 UAC 中确认。"
        : Status.NativeErrors > 0 ? $"注入失败，Windows 错误 {Status.NativeLastError}。查看 core.log 获取详情。"
        : "等待输入法宿主加载候选与云词库模块。请切换至微软拼音并输入拼音。";
    public string CertState => Status is null ? "—" : Status.NativeMode ? "注入模式不需要" : Status.CertTrusted ? "已信任" : "未信任";
    public string ProxyState => Status is null ? "—" : Status.NativeMode ? "注入模式不需要" : Status.ProxyApplied ? "已接管（仅 Bing）" : "未接管";
    public string ImeSessions => Status?.NativeMode == true ? $"{Status.NativeReady} 个就绪 / {Status.NativeTriggers} 次触发 / {Status.NativeRequests} 次请求" : Format(Status?.ImeSessions);

    partial void OnStatusChanged(CoreStatus? value)
    {
        foreach (string name in new[] { nameof(CloudRequests), nameof(Answered), nameof(CacheHits), nameof(Latency), nameof(Memory),
                                        nameof(LastQuery), nameof(Version), nameof(CertState), nameof(ProxyState), nameof(ImeSessions),
                                        nameof(NeedsElevation), nameof(Title), nameof(Summary) })
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
        var status = await _core.GetStatusAsync(_port());
        Status = status;
        State = status is null
            ? (CoreClient.StartsWithWindows || IsCertificateInstalled() ? CoreState.Stopped : CoreState.NotInstalled)
            : (status.NativeMode ? status.NativeReady > 0 : status.ProxyApplied) ? CoreState.Running : CoreState.Degraded;
    }

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task InstallAsync()
    {
        IsBusy = true;
        Message = "正在启用核心…";
        try
        {
            var (code, output) = await CoreClient.RunVerbAsync("--install");
            Message = code == 0 ? "" : output;
            await Task.Delay(500);  // let the core bind its port
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
    }

    [RelayCommand(CanExecute = nameof(CanRunVerb))]
    private async Task StartAsync()
    {
        IsBusy = true;
        try
        {
            CoreClient.Start();
            await Task.Delay(500);
            await RefreshAsync();
        }
        finally
        {
            IsBusy = false;
        }
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
    private async Task RestartElevatedAsync()
    {
        IsBusy = true;
        try
        {
            var (code, output) = await CoreClient.RunVerbAsync("--stop");
            if (code != 0) { Message = output; return; }
            Message = "请在 UAC 弹窗中确认管理员权限。";
            CoreClient.StartElevated();
            await Task.Delay(700);
            Message = "";
        }
        catch (Win32Exception ex)
        {
            Message = ex.NativeErrorCode == 1223 ? "已取消提权；可重新点击启动。" : $"提权启动失败：{ex.Message}";
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
            TestMessage = "PinyinFlux 未运行。";
            return;
        }
        var words = await _core.QueryAsync(_port(), input);
        if (words is null)
        {
            TestMessage = "查询失败。";
            return;
        }
        foreach (string w in words) TestResults.Add(w);
        TestMessage = words.Count == 0 ? "云词库没有完整匹配的候选。" : "";
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
