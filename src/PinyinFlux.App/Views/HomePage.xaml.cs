// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using PinyinFlux.App.Services;
using PinyinFlux.App.ViewModels;

namespace PinyinFlux.App.Views;

public sealed partial class HomePage : Page
{
    public HomePage() => InitializeComponent();

    public CoreViewModel ViewModel => App.Instance.Status;

    public SettingsViewModel Settings => App.Instance.Settings;

    public Brush StateBrush(CoreState state) => (Brush)Application.Current.Resources[state switch
    {
        CoreState.Running => "SystemFillColorSuccessBrush",
        CoreState.Degraded => "SystemFillColorCautionBrush",
        CoreState.Stopped => "SystemFillColorCriticalBrush",
        _ => "SystemFillColorNeutralBrush",
    }];

    public Visibility ShowStart(CoreState state) => state == CoreState.Stopped ? Visibility.Visible : Visibility.Collapsed;

    private void OnOpenLanguageSettings(object sender, RoutedEventArgs e) => MicrosoftPinyin.OpenLanguageSettings();

    private void OnOpenSources(object sender, RoutedEventArgs e) => App.Instance.Window.Navigate("Sources");

    private void OnTestKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != Windows.System.VirtualKey.Enter) return;
        e.Handled = true;
        // The binding updates on PropertyChanged, so TestInput is already current here.
        ViewModel.TestCommand.Execute(null);
    }
}
