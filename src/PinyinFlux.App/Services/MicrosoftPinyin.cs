// SPDX-License-Identifier: MIT
using System.Diagnostics;
using Microsoft.Win32;

namespace PinyinFlux.App.Services;

/// <summary>The parts of the user's Microsoft Pinyin setup that cloud candidates depend on, read from the registry.</summary>
public static class MicrosoftPinyin
{
    /// <summary>Language profile of Microsoft Pinyin: zh-Hans text service CLSID + profile GUID.</summary>
    private const string ProfileId = "{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}{FA550B04-5AD7-411F-A5AC-CA038EC515D7}";
    private const string ProfilesKey = @"Control Panel\International\User Profile";
    private const string SettingsKey = @"Software\Microsoft\InputMethod\Settings\CHS";
    private const string PolicyKey = @"Software\Policies\Microsoft\InputMethod\Settings\Shared";
    private const string CloudValue = "Enable Cloud Candidate";

    /// <summary>Microsoft Pinyin is in the user's input method list (Settings → 时间和语言 → 语言和区域).</summary>
    public static bool IsAdded
    {
        get
        {
            try
            {
                using var profiles = Registry.CurrentUser.OpenSubKey(ProfilesKey);
                if (profiles is null) return false;
                foreach (string language in profiles.GetSubKeyNames())
                {
                    using var key = profiles.OpenSubKey(language);
                    if (key?.GetValueNames().Any(n => n.Contains(ProfileId, StringComparison.OrdinalIgnoreCase)) == true) return true;
                }
                return false;
            }
            catch (Exception ex) when (ex is System.Security.SecurityException or IOException or UnauthorizedAccessException)
            {
                return true;  // unknown: don't nag
            }
        }
    }

    /// <summary>
    /// Microsoft Pinyin's own "云候选" switch (微软拼音设置 → 词库和自学习), off until turned on; a policy value overrides it.
    /// The injected extension reads the same values, so a change applies without restarting anything.
    /// </summary>
    public static bool CloudCandidatesEnabled => ReadCloudValue(PolicyKey) is { } policy ? policy != 0 : ReadCloudValue(SettingsKey) is > 0;

    /// <summary>An administrator's policy turned cloud candidates off; the user's switch has no effect.</summary>
    public static bool CloudCandidatesBlocked => ReadCloudValue(PolicyKey) is 0;

    /// <summary>Turns the switch on, as the toggle in Microsoft Pinyin's settings does.</summary>
    public static void EnableCloudCandidates()
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(SettingsKey);
            key.SetValue(CloudValue, 1, RegistryValueKind.DWord);
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or IOException or UnauthorizedAccessException) { }
    }

    private static int? ReadCloudValue(string path)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(path);
            return key?.GetValue(CloudValue) as int?;
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Windows Settings → 语言和区域, where input methods are added.</summary>
    public static void OpenLanguageSettings() => Open("ms-settings:regionlanguage");

    /// <summary>Microsoft Pinyin settings, top level.</summary>
    public static void OpenSettings() => Open("ms-settings:regionlanguage-chsime-pinyin");

    private static void Open(string uri) => Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true })?.Dispose();
}
