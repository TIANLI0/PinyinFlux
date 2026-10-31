// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Text.Json;
using Microsoft.Win32;

namespace PinyinFlux.App.Services;

public sealed record CoreStatus(
    string Version,
    long ImeSessions,
    long CloudRequests,
    long Queries,
    long CacheHits,
    long Answered,
    long AvgLatencyMs,
    string LastQuery,
    IReadOnlyList<string> Providers,
    bool ProxyApplied,
    bool CertTrusted,
    long WorkingSetKB,
    bool NativeMode,
    long NativeReady,
    long NativePending,
    long NativeDenied,
    long NativeUnsupported,
    long NativeErrors,
    long NativeLastError,
    long NativeTriggers,
    long NativeRequests);

/// <summary>Talks to PinyinFluxCore.exe: its loopback status endpoint and its command-line verbs.</summary>
public sealed class CoreClient : IDisposable
{
    private const string ReloadEventName = @"Local\PinyinFlux.Reload";
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValue = "PinyinFlux";

    // Never through a proxy: the core is on loopback, and the system proxy may be the core itself.
    private readonly HttpClient _http = new(new SocketsHttpHandler { UseProxy = false })
    {
        Timeout = TimeSpan.FromSeconds(3),
    };

    public static string CorePath { get; } = Path.Combine(AppContext.BaseDirectory, "PinyinFluxCore.exe");

    public static bool CoreExists => File.Exists(CorePath);

    public async Task<CoreStatus?> GetStatusAsync(int port, CancellationToken ct = default)
    {
        try
        {
            using var doc = JsonDocument.Parse(await _http.GetStringAsync($"http://127.0.0.1:{port}/status", ct));
            var r = doc.RootElement;
            return new CoreStatus(
                Str(r, "version"),
                Num(r, "imeSessions"),
                Num(r, "cloudRequests"),
                Num(r, "queries"),
                Num(r, "cacheHits"),
                Num(r, "answered"),
                Num(r, "avgLatencyMs"),
                Str(r, "lastQuery"),
                r.TryGetProperty("providers", out var p) ? p.EnumerateArray().Select(e => e.GetString() ?? "").ToList() : [],
                Bool(r, "proxyApplied"),
                Bool(r, "certTrusted"),
                Num(r, "workingSetKB"),
                Bool(r, "nativeMode"), Num(r, "nativeReady"), Num(r, "nativePending"),
                Num(r, "nativeDenied"), Num(r, "nativeUnsupported"), Num(r, "nativeErrors"),
                Num(r, "nativeLastError"), Num(r, "nativeTriggers"), Num(r, "nativeRequests"));
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException && !ct.IsCancellationRequested)
        {
            return null;  // not running
        }
    }

    /// <summary>Queries the same lexicon engine using its readable diagnostic response.</summary>
    public async Task<IReadOnlyList<string>?> QueryAsync(int port, string pinyin, CancellationToken ct = default)
    {
        try
        {
            string json = await _http.GetStringAsync($"http://127.0.0.1:{port}/query?q={Uri.EscapeDataString(pinyin)}", ct);
            using var doc = JsonDocument.Parse(json);
            var words = new List<string>();
            if (doc.RootElement.TryGetProperty("AS", out var a) && a.TryGetProperty("Results", out var results))
            {
                foreach (var result in results.EnumerateArray())
                {
                    if (!result.TryGetProperty("Suggests", out var suggests)) continue;
                    foreach (var s in suggests.EnumerateArray())
                    {
                        if (s.TryGetProperty("Txt", out var txt) && txt.GetString() is { } t) words.Add(t);
                    }
                }
            }
            return words;
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException && !ct.IsCancellationRequested)
        {
            return null;
        }
    }

    /// <summary>Tells the running core to re-read config.ini. False when it isn't running.</summary>
    public static bool SignalReload()
    {
        try
        {
            using var evt = EventWaitHandle.OpenExisting(ReloadEventName);
            return evt.Set();
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            return false;
        }
    }

    /// <summary>Runs a core verb (--install, --uninstall, --stop) and returns its exit code and output.</summary>
    public static async Task<(int ExitCode, string Output)> RunVerbAsync(string verb)
    {
        var psi = new ProcessStartInfo(CorePath, verb)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,  // the core writes UTF-8
            CreateNoWindow = true,
        };
        using var process = Process.Start(psi) ?? throw new InvalidOperationException("PinyinFluxCore.exe did not start");
        string output = await process.StandardOutput.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, output.Trim());
    }

    public static void Start() =>
        Process.Start(new ProcessStartInfo(CorePath) { UseShellExecute = false, CreateNoWindow = true })?.Dispose();

    public static void StartElevated() =>
        Process.Start(new ProcessStartInfo(CorePath)
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden,
        })?.Dispose();

    /// <summary>The core's autostart entry, written by --install and removed by --uninstall.</summary>
    public static bool StartsWithWindows
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(RunValue) is string;
        }
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKey);
            if (value) key.SetValue(RunValue, $"\"{CorePath}\"");
            else key.DeleteValue(RunValue, throwOnMissingValue: false);
        }
    }

    public void Dispose() => _http.Dispose();

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static long Num(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetInt64() : 0;

    private static bool Bool(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
}
