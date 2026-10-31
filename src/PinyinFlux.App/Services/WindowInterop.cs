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

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(nint hwnd);
}
