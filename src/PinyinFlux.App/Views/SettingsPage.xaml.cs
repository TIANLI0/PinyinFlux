// SPDX-License-Identifier: MIT
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PinyinFlux.App.Services;
using PinyinFlux.App.ViewModels;

namespace PinyinFlux.App.Views;

public sealed partial class SettingsPage : Page
{
    public SettingsPage() => InitializeComponent();

    public SettingsViewModel ViewModel => App.Instance.Settings;

    public CoreViewModel Status => App.Instance.Status;

    public UpdateViewModel Updates => App.Instance.Updates;

    public string CloudSwitchText(bool enabled) => enabled ? "已打开。PinyinFlux 的云词显示在云候选的位置。"
        : MicrosoftPinyin.CloudCandidatesBlocked ? "已被单位的管理员策略关闭，PinyinFlux 的云词无法显示。"
        : "已关闭。PinyinFlux 的云词显示在云候选的位置，关闭时不会出现。";

    public Visibility ShowEnableCloud(bool enabled) =>
        enabled || MicrosoftPinyin.CloudCandidatesBlocked ? Visibility.Collapsed : Visibility.Visible;

    private void OnOpenPinyinSettings(object sender, RoutedEventArgs e) => MicrosoftPinyin.OpenSettings();
}
