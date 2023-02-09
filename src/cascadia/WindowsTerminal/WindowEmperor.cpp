// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowEmperor.h"

// #include "MonarchFactory.h"
// #include "CommandlineArgs.h"
#include "../inc/WindowingBehavior.h"
// #include "FindTargetWindowArgs.h"
// #include "ProposeCommandlineResult.h"

#include "../../types/inc/utils.hpp"

#include "resource.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation;
using namespace ::Microsoft::Console;
using namespace std::chrono_literals;
using VirtualKeyModifiers = winrt::Windows::System::VirtualKeyModifiers;

#define TERMINAL_MESSAGE_CLASS_NAME L"TERMINAL_MESSAGE_CLASS"
extern "C" IMAGE_DOS_HEADER __ImageBase;

WindowEmperor::WindowEmperor() noexcept :
    _app{}
{
    _manager.FindTargetWindowRequested([this](const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                              const winrt::Microsoft::Terminal::Remoting::FindTargetWindowArgs& findWindowArgs) {
        {
            const auto targetWindow = _app.Logic().FindTargetWindow(findWindowArgs.Args().Commandline());
            findWindowArgs.ResultTargetWindow(targetWindow.WindowId());
            findWindowArgs.ResultTargetWindowName(targetWindow.WindowName());
        }
    });

    _dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
}

WindowEmperor::~WindowEmperor()
{
    _app.Close();
    _app = nullptr;
}

void _buildArgsFromCommandline(std::vector<winrt::hstring>& args)
{
    if (auto commandline{ GetCommandLineW() })
    {
        auto argc = 0;

        // Get the argv, and turn them into a hstring array to pass to the app.
        wil::unique_any<LPWSTR*, decltype(&::LocalFree), ::LocalFree> argv{ CommandLineToArgvW(commandline, &argc) };
        if (argv)
        {
            for (auto& elem : wil::make_range(argv.get(), argc))
            {
                args.emplace_back(elem);
            }
        }
    }
    if (args.empty())
    {
        args.emplace_back(L"wt.exe");
    }
}

bool WindowEmperor::HandleCommandlineArgs()
{
    std::vector<winrt::hstring> args;
    _buildArgsFromCommandline(args);
    auto cwd{ wil::GetCurrentDirectoryW<std::wstring>() };

    Remoting::CommandlineArgs eventArgs{ { args }, { cwd } };

    const auto result = _manager.ProposeCommandline2(eventArgs);

    // TODO! createWindow is false in cases like wt --help. Figure that out.

    if (result.ShouldCreateWindow())
    {
        CreateNewWindowThread(Remoting::WindowRequestedArgs{ result, eventArgs }, true);

        _manager.RequestNewWindow([this](auto&&, const Remoting::WindowRequestedArgs& args) {
            CreateNewWindowThread(args, false);
        });

        _becomeMonarch();
    }

    return result.ShouldCreateWindow();
}

bool WindowEmperor::ShouldExit()
{
    // TODO!
    return false;
}

void WindowEmperor::WaitForWindows()
{
    // std::thread one{ [this]() {
    //     WindowThread foo{ _app.Logic() };
    //     return foo.WindowProc();
    // } };

    // Sleep(2000);

    // std::thread two{ [this]() {
    //     WindowThread foo{ _app.Logic() };
    //     return foo.WindowProc();
    // } };

    // one.join();
    // two.join();

    // Sleep(30000); //30s

    // TODO! This creates a loop that never actually exits right now. It seems
    // to get a message when another window is activated, but never a WM_CLOSE
    // (that makes sense). It keeps running even when the threads all exit,
    // which is INTERESTING for sure.
    //
    // what we should do:
    // - Add an event to Monarch to indicate that we should exit, because all the
    //   peasants have exited.
    // - We very well may need an HWND_MESSAGE that's connected to the main
    //   thread, for processing global hotkeys. Consider that in the future too.

    MSG message;
    while (GetMessage(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    // _threads.clear();
}

void WindowEmperor::CreateNewWindowThread(Remoting::WindowRequestedArgs args, const bool firstWindow)
{
    Remoting::Peasant peasant{ _manager.CreateAPeasant(args) };

    auto func = [this, args, peasant, firstWindow]() {
        auto window{ std::make_shared<WindowThread>(_app.Logic(), args, _manager, peasant) };
        _windows.push_back(window);
        return window->WindowProc();
    };

    _threads.emplace_back(func);

    LOG_IF_FAILED(SetThreadDescription(_threads.back().native_handle(), L"Window Thread"));
}

void WindowEmperor::_becomeMonarch()
{
    _createMessageWindow();

    ////////////////////////////////////////////////////////////////////////////
    _setupGlobalHotkeys();

    _app.Logic().SettingsChanged([this](auto&&, const TerminalApp::SettingsLoadEventArgs& args) {
        if (SUCCEEDED(args.Result()))
        {
            _setupGlobalHotkeys();
        }
    });

    ////////////////////////////////////////////////////////////////////////////

    //     if (_windowManager2.DoesQuakeWindowExist() ||
    //         _window->IsQuakeWindow() ||
    //         (_windowLogic.GetAlwaysShowNotificationIcon() || _windowLogic.GetMinimizeToNotificationArea()))
    //     {
    //         _CreateNotificationIcon();
    //     }

    //     // These events are coming from peasants that become or un-become quake windows.
    //     _revokers.ShowNotificationIconRequested = _windowManager2.ShowNotificationIconRequested(winrt::auto_revoke, { this, &AppHost::_ShowNotificationIconRequested });
    //     _revokers.HideNotificationIconRequested = _windowManager2.HideNotificationIconRequested(winrt::auto_revoke, { this, &AppHost::_HideNotificationIconRequested });

    ////////////////////////////////////////////////////////////////////////////

    //     // Set the number of open windows (so we know if we are the last window)
    //     // and subscribe for updates if there are any changes to that number.
    //     _windowLogic.SetNumberOfOpenWindows(_windowManager2.GetNumberOfPeasants());

    _WindowCreatedToken = _manager.WindowCreated({ this, &WindowEmperor::_numberOfWindowsChanged });
    _WindowClosedToken = _manager.WindowClosed({ this, &WindowEmperor::_numberOfWindowsChanged });

    ////////////////////////////////////////////////////////////////////////////

    //     // If the monarch receives a QuitAll event it will signal this event to be
    //     // ran before each peasant is closed.
    //     _revokers.QuitAllRequested = _windowManager2.QuitAllRequested(winrt::auto_revoke, { this, &AppHost::_QuitAllRequested });

    ////////////////////////////////////////////////////////////////////////////

    // The monarch should be monitoring if it should save the window layout.
    // We want at least some delay to prevent the first save from overwriting
    _getWindowLayoutThrottler.emplace(std::move(std::chrono::seconds(10)), std::move([this]() { _SaveWindowLayoutsRepeat(); }));
    _getWindowLayoutThrottler.value()();
}

// sender and args are always nullptr
void WindowEmperor::_numberOfWindowsChanged(const winrt::Windows::Foundation::IInspectable&,
                                            const winrt::Windows::Foundation::IInspectable&)
{
    if (_getWindowLayoutThrottler)
    {
        _getWindowLayoutThrottler.value()();
    }

    const auto& numWindows{ _manager.GetNumberOfPeasants() };
    for (const auto& _windowThread : _windows)
    {
        _windowThread->Logic().SetNumberOfOpenWindows(numWindows);
    }
    // TODO! apparently, we crash when whe actually post a quit, handle it, and
    // then leak all our threads. That's not good, but also, this should only
    // happen once our threads all exited. So figure that out.
    if (numWindows == 0)
    {
        // _close();
    }
}

winrt::Windows::Foundation::IAsyncAction WindowEmperor::_SaveWindowLayouts()
{
    // Make sure we run on a background thread to not block anything.
    co_await winrt::resume_background();

    if (_app.Logic().ShouldUsePersistedLayout())
    {
        try
        {
            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Collect",
                              TraceLoggingDescription("Logged when collecting window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));

            const auto layoutJsons = _manager.GetAllWindowLayouts();

            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Save",
                              TraceLoggingDescription("Logged when writing window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));

            _app.Logic().SaveWindowLayoutJsons(layoutJsons);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_SaveWindowLayouts_Failed",
                              TraceLoggingDescription("An error occurred when collecting or writing window state"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));
        }
    }

    co_return;
}

winrt::fire_and_forget WindowEmperor::_SaveWindowLayoutsRepeat()
{
    // Make sure we run on a background thread to not block anything.
    co_await winrt::resume_background();

    co_await _SaveWindowLayouts();

    // Don't need to save too frequently.
    co_await 30s;

    // As long as we are supposed to keep saving, request another save.
    // This will be delayed by the throttler so that at most one save happens
    // per 10 seconds, if a save is requested by another source simultaneously.
    if (_getWindowLayoutThrottler.has_value())
    {
        TraceLoggingWrite(g_hWindowsTerminalProvider,
                          "AppHost_requestGetLayout",
                          TraceLoggingDescription("Logged when triggering a throttled write of the window state"),
                          TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                          TraceLoggingKeyword(TIL_KEYWORD_TRACE));

        _getWindowLayoutThrottler.value()();
    }
}

static WindowEmperor* GetThisFromHandle(HWND const window) noexcept
{
    const auto data = GetWindowLongPtr(window, GWLP_USERDATA);
    return reinterpret_cast<WindowEmperor*>(data);
}
[[nodiscard]] static LRESULT __stdcall MessageWndProc(HWND const window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
{
    WINRT_ASSERT(window);

    if (WM_NCCREATE == message)
    {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        WindowEmperor* that = static_cast<WindowEmperor*>(cs->lpCreateParams);
        WINRT_ASSERT(that);
        WINRT_ASSERT(!that->_window);
        that->_window = wil::unique_hwnd(window);
        SetWindowLongPtr(that->_window.get(), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
    }
    else if (WindowEmperor* that = GetThisFromHandle(window))
    {
        return that->MessageHandler(message, wparam, lparam);
    }

    return DefWindowProc(window, message, wparam, lparam);
}
void WindowEmperor::_createMessageWindow()
{
    WNDCLASS wc{};
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    wc.lpszClassName = TERMINAL_MESSAGE_CLASS_NAME;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MessageWndProc;
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    RegisterClass(&wc);
    WINRT_ASSERT(!_window);

    WINRT_VERIFY(CreateWindow(wc.lpszClassName,
                              L"Windows Terminal",
                              0,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              HWND_MESSAGE,
                              nullptr,
                              wc.hInstance,
                              this));
}

LRESULT WindowEmperor::MessageHandler(UINT const message, WPARAM const wParam, LPARAM const lParam) noexcept
{
    switch (message)
    {
    case WM_HOTKEY:
    {
        _hotkeyPressed(static_cast<long>(wParam));
        return 0;
    }
    }
    return DefWindowProc(_window.get(), message, wParam, lParam);
}

winrt::fire_and_forget WindowEmperor::_close()
{
    // Important! Switch back to the main thread for the emperor. That way, the
    // quit will go to the emperor's message pump.
    co_await wil::resume_foreground(_dispatcher);
    PostQuitMessage(0);
}

void WindowEmperor::_hotkeyPressed(const long hotkeyIndex)
{
    if (hotkeyIndex < 0 || static_cast<size_t>(hotkeyIndex) > _hotkeys.size())
    {
        return;
    }

    const auto& summonArgs = til::at(_hotkeys, hotkeyIndex);
    Remoting::SummonWindowSelectionArgs args{ summonArgs.Name() };

    // desktop:any - MoveToCurrentDesktop=false, OnCurrentDesktop=false
    // desktop:toCurrent - MoveToCurrentDesktop=true, OnCurrentDesktop=false
    // desktop:onCurrent - MoveToCurrentDesktop=false, OnCurrentDesktop=true
    args.OnCurrentDesktop(summonArgs.Desktop() == Settings::Model::DesktopBehavior::OnCurrent);
    args.SummonBehavior().MoveToCurrentDesktop(summonArgs.Desktop() == Settings::Model::DesktopBehavior::ToCurrent);
    args.SummonBehavior().ToggleVisibility(summonArgs.ToggleVisibility());
    args.SummonBehavior().DropdownDuration(summonArgs.DropdownDuration());

    switch (summonArgs.Monitor())
    {
    case Settings::Model::MonitorBehavior::Any:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::InPlace);
        break;
    case Settings::Model::MonitorBehavior::ToCurrent:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::ToCurrent);
        break;
    case Settings::Model::MonitorBehavior::ToMouse:
        args.SummonBehavior().ToMonitor(Remoting::MonitorBehavior::ToMouse);
        break;
    }

    _manager.SummonWindow(args);
    if (args.FoundMatch())
    {
        // Excellent, the window was found. We have nothing else to do here.
    }
    else
    {
        // We should make the window ourselves.
        // TODO!
        // _createNewTerminalWindow(summonArgs);
    }
}

bool WindowEmperor::_registerHotKey(const int index, const winrt::Microsoft::Terminal::Control::KeyChord& hotkey) noexcept
{
    const auto vkey = hotkey.Vkey();
    auto hotkeyFlags = MOD_NOREPEAT;
    {
        const auto modifiers = hotkey.Modifiers();
        WI_SetFlagIf(hotkeyFlags, MOD_WIN, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Windows));
        WI_SetFlagIf(hotkeyFlags, MOD_ALT, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Menu));
        WI_SetFlagIf(hotkeyFlags, MOD_CONTROL, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Control));
        WI_SetFlagIf(hotkeyFlags, MOD_SHIFT, WI_IsFlagSet(modifiers, VirtualKeyModifiers::Shift));
    }

    // TODO GH#8888: We should display a warning of some kind if this fails.
    // This can fail if something else already bound this hotkey.
    const auto result = ::RegisterHotKey(_window.get(), index, hotkeyFlags, vkey);
    LOG_LAST_ERROR_IF(!result);
    TraceLoggingWrite(g_hWindowsTerminalProvider,
                      "RegisterHotKey",
                      TraceLoggingDescription("Emitted when setting hotkeys"),
                      TraceLoggingInt64(index, "index", "the index of the hotkey to add"),
                      TraceLoggingUInt64(vkey, "vkey", "the key"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_WIN), "win", "is WIN in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_ALT), "alt", "is ALT in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_CONTROL), "control", "is CONTROL in the modifiers"),
                      TraceLoggingUInt64(WI_IsFlagSet(hotkeyFlags, MOD_SHIFT), "shift", "is SHIFT in the modifiers"),
                      TraceLoggingBool(result, "succeeded", "true if we succeeded"),
                      TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                      TraceLoggingKeyword(TIL_KEYWORD_TRACE));

    return result;
}

// Method Description:
// - Call UnregisterHotKey once for each previously registered hotkey.
// Return Value:
// - <none>
void WindowEmperor::_unregisterHotKey(const int index) noexcept
{
    TraceLoggingWrite(
        g_hWindowsTerminalProvider,
        "UnregisterHotKey",
        TraceLoggingDescription("Emitted when clearing previously set hotkeys"),
        TraceLoggingInt64(index, "index", "the index of the hotkey to remove"),
        TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
        TraceLoggingKeyword(TIL_KEYWORD_TRACE));

    LOG_IF_WIN32_BOOL_FALSE(::UnregisterHotKey(_window.get(), index));
}

winrt::fire_and_forget WindowEmperor::_setupGlobalHotkeys()
{
    // The hotkey MUST be registered on the main thread. It will fail otherwise!
    co_await wil::resume_foreground(_dispatcher);

    if (!_window)
    {
        // MSFT:36797001 There's a surprising number of hits of this callback
        // getting triggered during teardown. As a best practice, we really
        // should make sure _window exists before accessing it on any coroutine.
        // We might be getting called back after the app already began getting
        // cleaned up.
        co_return;
    }
    // Unregister all previously registered hotkeys.
    //
    // RegisterHotKey(), will not unregister hotkeys automatically.
    // If a hotkey with a given HWND and ID combination already exists
    // then a duplicate one will be added, which we don't want.
    // (Additionally we want to remove hotkeys that were removed from the settings.)
    for (auto i = 0, count = gsl::narrow_cast<int>(_hotkeys.size()); i < count; ++i)
    {
        _unregisterHotKey(i);
    }

    _hotkeys.clear();

    // Re-register all current hotkeys.
    for (const auto& [keyChord, cmd] : _app.Logic().GlobalHotkeys())
    {
        if (auto summonArgs = cmd.ActionAndArgs().Args().try_as<Settings::Model::GlobalSummonArgs>())
        {
            auto index = gsl::narrow_cast<int>(_hotkeys.size());
            const auto succeeded = _registerHotKey(index, keyChord);

            TraceLoggingWrite(g_hWindowsTerminalProvider,
                              "AppHost_setupGlobalHotkey",
                              TraceLoggingDescription("Emitted when setting a single hotkey"),
                              TraceLoggingInt64(index, "index", "the index of the hotkey to add"),
                              TraceLoggingWideString(cmd.Name().c_str(), "name", "the name of the command"),
                              TraceLoggingBoolean(succeeded, "succeeded", "true if we succeeded"),
                              TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                              TraceLoggingKeyword(TIL_KEYWORD_TRACE));
            _hotkeys.emplace_back(summonArgs);
        }
    }
}
