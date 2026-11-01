// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;

namespace PinyinFlux.App.Services;

/// <summary>
/// %LOCALAPPDATA%\PinyinFlux\config.ini, read and written with the same Win32 profile APIs the
/// core uses, so both sides agree on the format. Missing keys fall back to the core's defaults.
/// </summary>
public sealed partial class CoreConfig
{
    public int Port { get; set; } = 18765;
    public List<string> Providers { get; set; } = ["baidu", "sogou"];
    public int TimeoutMs { get; set; } = 900;
    public int MaxCandidates { get; set; } = 4;
    public bool NativeInjection { get; set; } = true;
    public bool ImeOnly { get; set; } = true;
    public bool LogToFile { get; set; }
    public bool ReclaimProxy { get; set; } = true;
    public bool TypingStats { get; set; } = true;
    public string SogouIp { get; set; } = "";

    // [App]: settings of this app only; the core ignores the section.
    public bool AutoUpdate { get; set; } = true;
    public bool IncludePrerelease { get; set; }

    public static string DataDir { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PinyinFlux");

    public static string FilePath => Path.Combine(DataDir, "config.ini");

    public static CoreConfig Load()
    {
        var c = new CoreConfig();
        c.Port = Math.Clamp(ReadInt("Core", "Port", c.Port), 1024, 65535);
        c.NativeInjection = ReadInt("Core", "NativeInjection", 1) != 0;
        c.TimeoutMs = Math.Clamp(ReadInt("Core", "TimeoutMs", c.TimeoutMs), 100, 5000);
        c.MaxCandidates = Math.Clamp(ReadInt("Core", "MaxCandidates", c.MaxCandidates), 1, 10);
        c.ImeOnly = ReadInt("Core", "ImeOnly", 1) != 0;
        c.LogToFile = ReadInt("Core", "Log", 0) != 0;
        c.ReclaimProxy = ReadInt("Core", "ReclaimProxy", 1) != 0;
        c.TypingStats = ReadInt("Core", "TypingStats", 1) != 0;
        string providers = ReadString("Core", "Providers", "");
        if (!string.IsNullOrWhiteSpace(providers))
        {
            c.Providers = providers.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .Select(p => p.ToLowerInvariant()).ToList();
        }
        c.SogouIp = ReadString("Sogou", "Ip", "").Trim();
        c.AutoUpdate = ReadInt("App", "AutoUpdate", 1) != 0;
        c.IncludePrerelease = ReadInt("App", "IncludePrerelease", 0) != 0;
        return c;
    }

    public void Save()
    {
        Directory.CreateDirectory(DataDir);
        Write("Core", "Port", Port.ToString());
        Write("Core", "NativeInjection", NativeInjection ? "1" : "0");
        Write("Core", "TimeoutMs", TimeoutMs.ToString());
        Write("Core", "MaxCandidates", MaxCandidates.ToString());
        Write("Core", "ImeOnly", ImeOnly ? "1" : "0");
        Write("Core", "Log", LogToFile ? "1" : "0");
        Write("Core", "ReclaimProxy", ReclaimProxy ? "1" : "0");
        Write("Core", "TypingStats", TypingStats ? "1" : "0");
        Write("Core", "Providers", string.Join(',', Providers));
        Write("Sogou", "Ip", SogouIp);
        Write("App", "AutoUpdate", AutoUpdate ? "1" : "0");
        Write("App", "IncludePrerelease", IncludePrerelease ? "1" : "0");
    }

    private static int ReadInt(string section, string key, int fallback) =>
        int.TryParse(ReadString(section, key, ""), out int v) ? v : fallback;

    private static string ReadString(string section, string key, string fallback)
    {
        var buffer = new char[1024];
        uint n = GetPrivateProfileStringW(section, key, "\u0001", buffer, (uint)buffer.Length, FilePath);
        string value = new(buffer, 0, (int)n);
        return value == "\u0001" ? fallback : value;
    }

    private static void Write(string section, string key, string value) =>
        WritePrivateProfileStringW(section, key, value, FilePath);

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint GetPrivateProfileStringW(string section, string key, string fallback,
        [Out] char[] buffer, uint size, string path);

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool WritePrivateProfileStringW(string section, string key, string value, string path);
}
