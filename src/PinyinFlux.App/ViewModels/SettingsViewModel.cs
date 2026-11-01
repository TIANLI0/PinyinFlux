// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PinyinFlux.App.Services;

namespace PinyinFlux.App.ViewModels;

/// <summary>
/// Two-way projection of <see cref="CoreConfig"/>; every change is written to config.ini right away
/// and the running core is told to reload it.
/// </summary>
public sealed partial class SettingsViewModel : ObservableObject
{
    private static readonly (string Id, string Name, string Description)[] KnownProviders =
    [
        ("baidu", "百度输入法云", "HTTPS 接口，覆盖面广，热词和人名多。"),
        ("sogou", "搜狗输入法云", "手机输入法协议（明文 HTTP），长句和网络新词强。"),
    ];

    private readonly CoreConfig _config;

    public SettingsViewModel(CoreConfig config)
    {
        _config = config;
        // Enabled providers first, in their saved priority order; then the rest.
        foreach (string id in config.Providers)
        {
            if (KnownProviders.FirstOrDefault(p => p.Id == id) is { Id: not null } known)
                Providers.Add(new ProviderItemViewModel(known.Id, known.Name, known.Description, true, this));
        }
        foreach (var known in KnownProviders.Where(p => !config.Providers.Contains(p.Id)))
            Providers.Add(new ProviderItemViewModel(known.Id, known.Name, known.Description, false, this));
        Renumber();
    }

    /// <summary>Order = priority: earlier sources win when both answer.</summary>
    public ObservableCollection<ProviderItemViewModel> Providers { get; } = [];

    public int Port => _config.Port;

    /// <summary>0 = 直接接入 (native injection), 1 = 兼容模式 (PAC proxy + local certificate).</summary>
    public int ConnectionMode
    {
        get => _config.NativeInjection ? 0 : 1;
        set
        {
            bool native = value == 0;
            if (value < 0 || native == _config.NativeInjection) return;
            Update(() => _config.NativeInjection = native);
            OnPropertyChanged(nameof(IsCompatibilityMode));
            OnPropertyChanged(nameof(ConnectionModeDescription));
            ConnectionModeChanged?.Invoke(native);
        }
    }

    /// <summary>Raised after the mode is saved, with true for native injection; a running core has to be started again.</summary>
    public event Action<bool>? ConnectionModeChanged;

    public bool IsCompatibilityMode => !_config.NativeInjection;

    public string ConnectionModeDescription => _config.NativeInjection
        ? "在微软拼音内部补充云词，打字停顿即可出现。不安装证书，不改动网络设置；需要授予一次管理员权限。"
        : "直接接入不可用时的备选：安装一个本机证书，并让系统代理把微软拼音发往 Bing 的云请求交给 PinyinFlux。何时查询由微软拼音决定，云词出现得较少。";

    public double TimeoutMs
    {
        get => _config.TimeoutMs;
        set => Update(() => _config.TimeoutMs = (int)Math.Round(value / 50) * 50);
    }

    public double MaxCandidates
    {
        get => _config.MaxCandidates;
        set => Update(() => _config.MaxCandidates = (int)value);
    }

    public bool ImeOnly
    {
        get => _config.ImeOnly;
        set => Update(() => _config.ImeOnly = value);
    }

    public bool ReclaimProxy
    {
        get => _config.ReclaimProxy;
        set => Update(() => _config.ReclaimProxy = value);
    }

    /// <summary>The core counts committed characters (direct mode only); never what was typed.</summary>
    public bool TypingStats
    {
        get => _config.TypingStats;
        set => Update(() => _config.TypingStats = value);
    }

    public bool LogToFile
    {
        get => _config.LogToFile;
        set => Update(() => _config.LogToFile = value);
    }

    public string SogouIp
    {
        get => _config.SogouIp;
        set => Update(() => _config.SogouIp = value.Trim());
    }

    public bool AutoUpdate
    {
        get => _config.AutoUpdate;
        set => Update(() => _config.AutoUpdate = value);
    }

    public bool IncludePrerelease
    {
        get => _config.IncludePrerelease;
        set => Update(() => _config.IncludePrerelease = value);
    }

    public bool StartWithWindows
    {
        get => CoreClient.StartsWithWindows;
        set
        {
            CoreClient.StartsWithWindows = value;
            OnPropertyChanged();
        }
    }

    public bool HasEnabledProvider => Providers.Any(p => p.IsEnabled);

    /// <summary>"百度输入法云、搜狗输入法云", in priority order.</summary>
    public string EnabledProviderNames => HasEnabledProvider ? string.Join("、", Providers.Where(p => p.IsEnabled).Select(p => p.Name)) : "未启用任何云词库";

    [RelayCommand]
    private static void OpenDataFolder()
    {
        Directory.CreateDirectory(CoreConfig.DataDir);
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(CoreConfig.DataDir) { UseShellExecute = true })?.Dispose();
    }

    internal void Move(ProviderItemViewModel item, int delta)
    {
        int from = Providers.IndexOf(item);
        int to = Math.Clamp(from + delta, 0, Providers.Count - 1);
        if (from < 0 || from == to) return;
        Providers.Move(from, to);
        Renumber();
        SaveProviders();
    }

    internal void SaveProviders()
    {
        Update(() => _config.Providers = Providers.Where(p => p.IsEnabled).Select(p => p.Id).ToList(), nameof(HasEnabledProvider));
        OnPropertyChanged(nameof(EnabledProviderNames));
    }

    private void Renumber()
    {
        for (int i = 0; i < Providers.Count; i++)
        {
            Providers[i].CanMoveUp = i > 0;
            Providers[i].CanMoveDown = i < Providers.Count - 1;
        }
    }

    private void Update(Action change, [System.Runtime.CompilerServices.CallerMemberName] string? property = null)
    {
        change();
        _config.Save();
        CoreClient.SignalReload();
        OnPropertyChanged(property);
    }
}

/// <summary>One cloud lexicon in the sources list.</summary>
public sealed partial class ProviderItemViewModel : ObservableObject
{
    private readonly SettingsViewModel _owner;

    public ProviderItemViewModel(string id, string name, string description, bool enabled, SettingsViewModel owner)
    {
        Id = id;
        Name = name;
        Description = description;
        _isEnabled = enabled;
        _owner = owner;
    }

    public string Id { get; }
    public string Name { get; }
    public string Description { get; }

    private bool _isEnabled;

    public bool IsEnabled
    {
        get => _isEnabled;
        set
        {
            if (SetProperty(ref _isEnabled, value)) _owner.SaveProviders();
        }
    }

    [ObservableProperty]
    public partial bool CanMoveUp { get; set; }

    [ObservableProperty]
    public partial bool CanMoveDown { get; set; }

    [RelayCommand]
    private void MoveUp() => _owner.Move(this, -1);

    [RelayCommand]
    private void MoveDown() => _owner.Move(this, 1);
}
