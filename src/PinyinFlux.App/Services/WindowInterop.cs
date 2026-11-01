// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace PinyinFlux.App.Services;

/// <summary>Win32 helpers for the main window.</summary>
internal static partial class WindowInterop
{
    public static nint GetHandle(Window window) => WindowNative.GetWindowHandle(window);

    public static AppWindow GetAppWindow(Window window) =>
        AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(GetHandle(window)));

    public static double GetScale(Window window) => GetDpiForWindow(GetHandle(window)) / 96.0;

    /// <summary>Restores a minimized window and brings it in front of the window the user was in.</summary>
    public static void BringToFront(Window window)
    {
        if (GetAppWindow(window).Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } presenter) presenter.Restore();
        window.Activate();
        SetForegroundWindow(GetHandle(window));
    }

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool AllowSetForegroundWindow(uint processId);

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(nint hwnd);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetForegroundWindow(nint hwnd);
}
