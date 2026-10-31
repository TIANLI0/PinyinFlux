// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;

namespace PinyinFlux.App.Controls;

/// <summary>
/// One settings row: header and optional description on the left, the control (switch, slider,
/// combo box…) right-aligned. The control gets the header as its accessible name.
/// </summary>
[ContentProperty(Name = nameof(Action))]
public sealed partial class SettingRow : UserControl
{
    public static readonly DependencyProperty HeaderProperty = DependencyProperty.Register(
        nameof(Header), typeof(string), typeof(SettingRow), new PropertyMetadata(null, (d, _) => ((SettingRow)d).Update()));

    public static readonly DependencyProperty DescriptionProperty = DependencyProperty.Register(
        nameof(Description), typeof(string), typeof(SettingRow), new PropertyMetadata(null, (d, _) => ((SettingRow)d).Update()));

    public static readonly DependencyProperty ActionProperty = DependencyProperty.Register(
        nameof(Action), typeof(object), typeof(SettingRow), new PropertyMetadata(null, (d, _) => ((SettingRow)d).Update()));

    public SettingRow()
    {
        InitializeComponent();
    }

    public string? Header
    {
        get => (string?)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    public string? Description
    {
        get => (string?)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    /// <summary>The control shown on the right.</summary>
    public object? Action
    {
        get => GetValue(ActionProperty);
        set => SetValue(ActionProperty, value);
    }

    private void Update()
    {
        HeaderText.Text = Header ?? string.Empty;
        DescriptionText.Text = Description ?? string.Empty;
        DescriptionText.Visibility = string.IsNullOrEmpty(Description) ? Visibility.Collapsed : Visibility.Visible;
        ActionPresenter.Content = Action;
        if (Action is FrameworkElement control && !string.IsNullOrEmpty(Header)
            && string.IsNullOrEmpty(AutomationProperties.GetName(control)))
        {
            AutomationProperties.SetName(control, Header);
        }
    }
}
