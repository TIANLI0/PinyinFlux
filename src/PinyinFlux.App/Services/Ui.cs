// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml;

namespace PinyinFlux.App.Services;

/// <summary>x:Bind helper functions.</summary>
public static class Ui
{
    public static Visibility Visible(bool value) => value ? Visibility.Visible : Visibility.Collapsed;

    public static Visibility Collapsed(bool value) => value ? Visibility.Collapsed : Visibility.Visible;

    public static Visibility VisibleIfText(string? text) => string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;

    public static bool Not(bool value) => !value;

    /// <summary>Slider value readout: "900 ms".</summary>
    public static string Milliseconds(double value) => value.ToString("0", System.Globalization.CultureInfo.CurrentCulture) + " ms";

    /// <summary>Slider value readout: "4".</summary>
    public static string Number(double value) => value.ToString("0", System.Globalization.CultureInfo.CurrentCulture);
}
