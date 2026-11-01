// SPDX-License-Identifier: MIT
using System.Collections.ObjectModel;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PinyinFlux.App.Services;

namespace PinyinFlux.App.ViewModels;

/// <summary>
/// The 打字统计 page: today's live counts from the core's /status, history from stats.csv. Refreshes whenever
/// <see cref="CoreViewModel"/> polls, so nothing runs while the window is in the background.
/// </summary>
public sealed partial class TypingViewModel : ObservableObject
{
    private const int ChartDays = 30;
    private const double ChartHeight = 120;

    private readonly CoreViewModel _core;
    private IReadOnlyDictionary<DateOnly, TypingDay> _history = new Dictionary<DateOnly, TypingDay>();
    private DateTime _historyStamp = DateTime.MaxValue;

    public TypingViewModel(CoreViewModel core)
    {
        _core = core;
        core.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(CoreViewModel.Status)) Refresh();
        };
        Refresh();
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Chars), nameof(Time), nameof(Speed), nameof(Keys), nameof(Cloud))]
    public partial TypingDay Today { get; set; } = TypingDay.Empty(DateOnly.FromDateTime(DateTime.Now));

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TotalChars), nameof(TotalTime), nameof(TotalDays), nameof(DailyAverage), nameof(TotalSpeed),
                              nameof(TotalCloud), nameof(BestDay), nameof(BestDayDate))]
    public partial TypingTotals Totals { get; set; } = new(0, 0, 0, 0, null);

    /// <summary>The last 30 days, oldest first.</summary>
    public ObservableCollection<TypingBar> Chart { get; } = [];

    [ObservableProperty]
    public partial string ChartStart { get; set; } = "";

    [ObservableProperty]
    public partial string ChartMiddle { get; set; } = "";

    [ObservableProperty]
    public partial string ChartPeak { get; set; } = "";

    public string Chars => Number(Today.Chars);
    public string Time => Duration(Today.ActiveSeconds);
    /// <summary>Characters per minute of typing time; meaningless for the first half minute.</summary>
    public string Speed => PerMinute(Today.Chars, Today.ActiveSeconds);
    /// <summary>Pinyin letters per character: about 3 for full pinyin, lower with abbreviations and phrases.</summary>
    public string Keys => Today.Chars > 0 ? $"{(double)Today.Letters / Today.Chars:0.0} 键" : "—";
    public string Cloud => Number(Today.Cloud);

    public string TotalChars => Number(Totals.Chars);
    public string TotalTime => Duration(Totals.ActiveSeconds);
    public string TotalDays => Totals.Days > 0 ? $"共 {Totals.Days} 天有记录" : "还没有记录";
    public string DailyAverage => Totals.Days > 0 ? $"{Number(Totals.Chars / Totals.Days)} 字" : "—";
    public string TotalSpeed => PerMinute(Totals.Chars, Totals.ActiveSeconds);
    public string TotalCloud => Number(Totals.Cloud);
    public string BestDay => Totals.Best is { } best ? $"{Number(best.Chars)} 字" : "—";
    public string BestDayDate => Totals.Best is { } best ? $"最多的一天（{best.Date:yyyy年M月d日}）" : "最多的一天";

    /// <summary>Compatibility mode never counts; the page says so.</summary>
    public bool IsCompatibilityMode => _core.IsCompatibilityMode;

    private void Refresh()
    {
        OnPropertyChanged(nameof(IsCompatibilityMode));
        // stats.csv changes about once a minute; re-read it only then.
        DateTime stamp = TypingHistory.LastWrite;
        if (stamp != _historyStamp)
        {
            _historyStamp = stamp;
            _history = TypingHistory.Load();
        }
        var today = DateOnly.FromDateTime(DateTime.Now);
        // The core's live counts are ahead of the file.
        var current = _core.Status?.TypingToday is { } live && live.Date == today ? live
            : _history.TryGetValue(today, out var saved) ? saved : TypingDay.Empty(today);
        if (current != Today) Today = current;

        var days = _history.Values.Where(d => d.Date != today && d.Commits > 0).ToList();
        if (current.Commits > 0) days.Add(current);
        var totals = new TypingTotals(days.Sum(d => d.Chars), days.Sum(d => d.ActiveSeconds), days.Sum(d => d.Cloud), days.Count,
            days.MaxBy(d => d.Chars));
        if (totals != Totals) Totals = totals;

        var range = Enumerable.Range(0, ChartDays).Select(i => today.AddDays(i - ChartDays + 1))
            .Select(d => d == today ? current : _history.TryGetValue(d, out var day) ? day : TypingDay.Empty(d)).ToList();
        long peak = range.Max(d => d.Chars);
        var bars = range.Select(d => new TypingBar(
            peak > 0 ? Math.Max(2, ChartHeight * d.Chars / peak) : 2,
            $"{d.Date:M月d日}：{Number(d.Chars)} 字",
            d.Date == today ? 1.0 : 0.5)).ToList();
        if (!bars.SequenceEqual(Chart))
        {
            Chart.Clear();
            foreach (var bar in bars) Chart.Add(bar);
        }
        ChartStart = range[0].Date.ToString("M月d日", CultureInfo.CurrentCulture);
        ChartMiddle = range[ChartDays / 2].Date.ToString("M月d日", CultureInfo.CurrentCulture);
        ChartPeak = peak > 0 ? $"最多 {Number(peak)} 字" : "还没有记录";
    }

    [RelayCommand]
    private async Task ClearAsync()
    {
        TypingHistory.Clear();
        await Task.Delay(300);  // the core clears its counts and the file
        await _core.RefreshAsync();
    }

    private static string Number(long n) => n.ToString("N0", CultureInfo.CurrentCulture);

    private static string Duration(long seconds) => seconds switch
    {
        <= 0 => "—",
        < 60 => "不到 1 分钟",
        < 3600 => $"{seconds / 60} 分钟",
        _ => $"{seconds / 3600} 小时 {seconds % 3600 / 60} 分",
    };

    private static string PerMinute(long chars, long seconds) => seconds >= 30 ? $"{chars * 60.0 / seconds:0} 字/分" : "—";
}

/// <summary>All recorded days together; <see cref="Best"/> is the day with the most characters.</summary>
public sealed record TypingTotals(long Chars, long ActiveSeconds, long Cloud, int Days, TypingDay? Best);

/// <summary>One day in the chart: bar height in pixels, tooltip, opacity (today stands out).</summary>
public sealed record TypingBar(double Height, string Detail, double Opacity);
