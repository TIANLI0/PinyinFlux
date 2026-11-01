// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PinyinFlux.App.ViewModels;

namespace PinyinFlux.App.Views;

/// <summary>Typing statistics: today, the last 30 days, all time, and the switch that turns counting on or off.</summary>
public sealed partial class StatsPage : Page
{
    public StatsPage() => InitializeComponent();

    public TypingViewModel ViewModel => App.Instance.Typing;

    public SettingsViewModel Settings => App.Instance.Settings;

    private void OnEnable(object sender, RoutedEventArgs e) => Settings.TypingStats = true;

    private void OnClear(object sender, RoutedEventArgs e)
    {
        ClearFlyout.Hide();
        ViewModel.ClearCommand.Execute(null);
    }
}
