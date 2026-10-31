// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml.Controls;
using PinyinFlux.App.ViewModels;

namespace PinyinFlux.App.Views;

public sealed partial class SettingsPage : Page
{
    public SettingsPage() => InitializeComponent();

    public SettingsViewModel ViewModel => App.Instance.Settings;

    public CoreViewModel Status => App.Instance.Status;

    public UpdateViewModel Updates => App.Instance.Updates;
}
