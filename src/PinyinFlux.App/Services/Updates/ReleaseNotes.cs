// SPDX-License-Identifier: MIT
using System.Text;
using System.Text.RegularExpressions;

namespace PinyinFlux.App.Services.Updates;

/// <summary>Turns a GitHub release's Markdown notes into text for a TextBlock.</summary>
public static partial class ReleaseNotes
{
    private const int MaxLength = 8000;

    [GeneratedRegex(@"\[([^\]]+)\]\([^)]*\)", RegexOptions.CultureInvariant)]
    private static partial Regex Link();

    [GeneratedRegex(@"\*\*|__|`", RegexOptions.CultureInvariant)]
    private static partial Regex Emphasis();

    /// <summary>
    /// Bullets become "•", headings, emphasis and links become plain text, and a trailing download section
    /// (only useful on the GitHub page) is dropped. Null when nothing is left.
    /// </summary>
    public static string? ToPlainText(string? notes)
    {
        if (string.IsNullOrWhiteSpace(notes)) return null;
        var text = new StringBuilder();
        bool blank = false;
        foreach (string raw in notes.Replace("\r\n", "\n").Split('\n'))
        {
            string line = raw.Trim();
            if (line.StartsWith('#'))
            {
                line = line.TrimStart('#').Trim();
                if (line is "下载" or "Download" or "Downloads") break;
            }
            else if (line is ['-' or '*', ' ', .. var item]) line = "• " + item;
            line = Emphasis().Replace(Link().Replace(line, "$1"), string.Empty);
            if (line.Length == 0) { blank = text.Length > 0; continue; }
            if (blank) text.Append('\n');
            if (text.Length > 0) text.Append('\n');
            text.Append(line);
            blank = false;
            if (text.Length >= MaxLength) break;
        }
        return text.Length == 0 ? null : text.ToString();
    }
}
