// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;
using System.Globalization;

namespace PinyinFlux.App.Services.Updates;

/// <summary>
/// A SemVer 2.0 version ("1.2.3", "v1.2.3-beta.2+abc"). Build metadata is ignored, as SemVer requires;
/// a missing minor or patch number reads as 0.
/// </summary>
public sealed class SemanticVersion : IComparable<SemanticVersion>, IEquatable<SemanticVersion>
{
    private SemanticVersion(int major, int minor, int patch, string[] prerelease)
    {
        Major = major;
        Minor = minor;
        Patch = patch;
        Prerelease = prerelease;
    }

    public int Major { get; }

    public int Minor { get; }

    public int Patch { get; }

    /// <summary>Dot-separated pre-release identifiers ("beta", "2"); empty for a release.</summary>
    public IReadOnlyList<string> Prerelease { get; }

    public bool IsPrerelease => Prerelease.Count > 0;

    public static bool TryParse([NotNullWhen(true)] string? text, [NotNullWhen(true)] out SemanticVersion? version)
    {
        version = null;
        if (string.IsNullOrWhiteSpace(text)) return false;
        string value = text.Trim();
        if (value[0] is 'v' or 'V') value = value[1..];
        int plus = value.IndexOf('+');
        if (plus >= 0) value = value[..plus];
        int dash = value.IndexOf('-');
        string core = dash >= 0 ? value[..dash] : value;
        string[] prerelease = dash >= 0 ? value[(dash + 1)..].Split('.') : [];
        string[] parts = core.Split('.');
        if (parts.Length is < 1 or > 3) return false;
        int[] numbers = new int[3];
        for (int i = 0; i < parts.Length; i++)
            if (!IsNumeric(parts[i]) || !int.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out numbers[i])) return false;
        if (prerelease.Any(id => id.Length == 0 || !id.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'))) return false;
        version = new SemanticVersion(numbers[0], numbers[1], numbers[2], prerelease);
        return true;
    }

    private static bool IsNumeric(string s) => s.Length > 0 && s.All(char.IsAsciiDigit);

    public int CompareTo(SemanticVersion? other)
    {
        if (other is null) return 1;
        int c = Major.CompareTo(other.Major);
        if (c == 0) c = Minor.CompareTo(other.Minor);
        if (c == 0) c = Patch.CompareTo(other.Patch);
        if (c != 0) return c;
        // A release has higher precedence than any of its pre-releases.
        if (IsPrerelease != other.IsPrerelease) return IsPrerelease ? -1 : 1;
        for (int i = 0; i < Math.Min(Prerelease.Count, other.Prerelease.Count); i++)
        {
            string a = Prerelease[i], b = other.Prerelease[i];
            bool numericA = IsNumeric(a), numericB = IsNumeric(b);
            if (numericA && numericB)
            {
                // Compare digit strings without overflow: longer (ignoring leading zeros) is larger.
                string ta = a.TrimStart('0'), tb = b.TrimStart('0');
                c = ta.Length != tb.Length ? ta.Length.CompareTo(tb.Length) : string.CompareOrdinal(ta, tb);
            }
            else if (numericA != numericB) c = numericA ? -1 : 1; // Numeric identifiers sort before alphanumeric ones.
            else c = string.CompareOrdinal(a, b);
            if (c != 0) return Math.Sign(c);
        }
        return Prerelease.Count.CompareTo(other.Prerelease.Count);
    }

    public bool Equals(SemanticVersion? other) => other is not null && CompareTo(other) == 0;

    public override bool Equals(object? obj) => obj is SemanticVersion other && Equals(other);

    public override int GetHashCode() => HashCode.Combine(Major, Minor, Patch, string.Join('.', Prerelease));

    public override string ToString() =>
        string.Create(CultureInfo.InvariantCulture, $"{Major}.{Minor}.{Patch}") + (IsPrerelease ? "-" + string.Join('.', Prerelease) : string.Empty);

    public static bool operator >(SemanticVersion a, SemanticVersion b) => a.CompareTo(b) > 0;

    public static bool operator <(SemanticVersion a, SemanticVersion b) => a.CompareTo(b) < 0;

    public static bool operator >=(SemanticVersion a, SemanticVersion b) => a.CompareTo(b) >= 0;

    public static bool operator <=(SemanticVersion a, SemanticVersion b) => a.CompareTo(b) <= 0;

    /// <summary>
    /// The running app's version from an assembly's informational version (VersionPrefix-VersionSuffix,
    /// with "+commit" source-link metadata stripped), falling back to its numeric version.
    /// </summary>
    public static SemanticVersion FromAssembly(System.Reflection.Assembly assembly)
    {
        string? informational = System.Reflection.CustomAttributeExtensions
            .GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(assembly)?.InformationalVersion;
        if (TryParse(informational, out var version)) return version;
        return TryParse(assembly.GetName().Version?.ToString(3), out version) ? version : new SemanticVersion(0, 0, 0, []);
    }
}
