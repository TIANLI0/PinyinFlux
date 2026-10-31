// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml.Controls;
using PinyinFlux.App.ViewModels;

namespace PinyinFlux.App.Views;

public sealed partial class SourcesPage : Page
{
    public SourcesPage() => InitializeComponent();

    public SettingsViewModel ViewModel => App.Instance.Settings;
}
