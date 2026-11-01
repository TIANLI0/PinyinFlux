// SPDX-License-Identifier: MIT
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;
using PinyinFlux.App.Services;

namespace PinyinFlux.App.Views;

public sealed partial class MainWindow : Window
{
    private static readonly Dictionary<string, Type> Pages = new()
    {
        ["Home"] = typeof(HomePage),
        ["Sources"] = typeof(SourcesPage),
        ["Stats"] = typeof(StatsPage),
        ["Settings"] = typeof(SettingsPage),
        ["About"] = typeof(AboutPage),
    };

    public ViewModels.UpdateViewModel Updates => App.Instance.Updates;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        var appWindow = WindowInterop.GetAppWindow(this);
        appWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "PinyinFlux.ico"));
        appWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        double scale = WindowInterop.GetScale(this);
        appWindow.Resize(new Windows.Graphics.SizeInt32((int)(980 * scale), (int)(700 * scale)));
        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(560 * scale);
            presenter.PreferredMinimumHeight = (int)(440 * scale);
        }
        CenterOnScreen(appWindow);

        Navigate("Home");
    }

    public void Navigate(string tag)
    {
        var item = Nav.MenuItems.Concat(Nav.FooterMenuItems).OfType<NavigationViewItem>().FirstOrDefault(i => (string)i.Tag == tag);
        if (item is not null && !ReferenceEquals(Nav.SelectedItem, item))
        {
            Nav.SelectedItem = item;
        }
        else if (item is not null && ContentFrame.Content is null)
        {
            NavigateFrame(tag);
        }
    }

    private void OnNavigationSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag }) NavigateFrame(tag);
    }

    private void NavigateFrame(string tag)
    {
        if (Pages.TryGetValue(tag, out var page) && ContentFrame.CurrentSourcePageType != page)
        {
            ContentFrame.Navigate(page, null, new EntranceNavigationTransitionInfo());
        }
    }

    private static void CenterOnScreen(AppWindow appWindow)
    {
        var area = DisplayArea.GetFromWindowId(appWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        int x = area.X + Math.Max(0, (area.Width - appWindow.Size.Width) / 2);
        int y = area.Y + Math.Max(0, (area.Height - appWindow.Size.Height) / 2);
        appWindow.Move(new Windows.Graphics.PointInt32(x, y));
    }
}
