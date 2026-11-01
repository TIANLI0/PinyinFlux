// SPDX-License-Identifier: MIT
using System.Globalization;

namespace PinyinFlux.App.Services;

/// <summary>One day of typing statistics: counts only, as the core writes them.</summary>
public sealed record TypingDay(DateOnly Date, long Commits, long Chars, long Letters, long Cloud, long ActiveSeconds)
{
    public static TypingDay Empty(DateOnly date) => new(date, 0, 0, 0, 0, 0);
}

/// <summary>
/// %LOCALAPPDATA%\PinyinFlux\stats.csv, one line per day written by the core about once a minute
/// (date,commits,chars,letters,cloud,activeSeconds). Today's live counts come from /status instead.
/// </summary>
public static class TypingHistory
{
    private const string ClearEventName = @"Local\PinyinFlux.ClearTyping";

    public static string FilePath => Path.Combine(CoreConfig.DataDir, "stats.csv");

    public static DateTime LastWrite => File.Exists(FilePath) ? File.GetLastWriteTimeUtc(FilePath) : DateTime.MinValue;

    public static IReadOnlyDictionary<DateOnly, TypingDay> Load()
    {
        var days = new Dictionary<DateOnly, TypingDay>();
        try
        {
            foreach (string line in File.ReadLines(FilePath))
            {
                string[] f = line.Split(',');
                if (f.Length < 6 || line.StartsWith('#') ||
                    !DateOnly.TryParseExact(f[0], "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var date)) continue;
                long N(int i) => long.TryParse(f[i], NumberStyles.None, CultureInfo.InvariantCulture, out long v) ? v : 0;
                days[date] = new TypingDay(date, N(1), N(2), N(3), N(4), N(5));
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return days;
    }

    /// <summary>Forgets all statistics. A running core clears its own counts and the file; otherwise the file is deleted here.</summary>
    public static void Clear()
    {
        try
        {
            using var evt = EventWaitHandle.OpenExisting(ClearEventName);
            evt.Set();
            return;
        }
        catch (WaitHandleCannotBeOpenedException) { }
        try
        {
            File.Delete(FilePath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }
}
