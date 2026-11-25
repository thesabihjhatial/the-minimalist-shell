// ==WindhawkMod==
// @id              the-minimalist-shell
// @name            The Minimalist Shell
// @description     Turns the Windows 11 shell into a minimalistic MacOS style shell
// @version         1.0
// @author          Muhammad Sabih
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lgdi32 -luser32 -lgdiplus -lshell32 -ladvapi32 -ldwmapi
// ==/WindhawkMod==

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <utility>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Color;

constexpr int kTopBarHeight = 48;
constexpr int kMagnifyPercent = 150;
constexpr double kMagnifyRange = 90.0;
std::atomic<bool> g_unloading;
std::atomic<bool> g_taskbarViewDllLoaded;

namespace Motion {
constexpr int kFrameMs = 16;
constexpr double kFollowTauMs = 55.0;

bool Enabled() {
    static std::atomic<int> cached{-1};
    static std::atomic<ULONGLONG> cachedAt{0};
    ULONGLONG now = GetTickCount64();
    if (cached.load() < 0 || now - cachedAt.load() > 2000) {
        BOOL animations = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
            animations = TRUE;
        cached = animations != FALSE ? 1 : 0;
        cachedAt = now;
    }
    return cached.load() != 0;
}

double Approach(double current, double target, double dtMs, double tauMs) {
    if (!Enabled() || tauMs <= 0.0)
        return target;
    double next = current + (target - current) * (1.0 - std::exp(-dtMs / tauMs));
    if (std::abs(target - next) <= 0.001)
        return target;
    return next;
}
}

template <typename F>
FrameworkElement FindChild(DependencyObject parent, F pred) {
    int count = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++) {
        auto child =
            Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (child && pred(child))
            return child;
    }
    return nullptr;
}

template <typename F>
FrameworkElement FindDescendant(DependencyObject parent, F pred, int maxDepth) {
    if (maxDepth <= 0)
        return nullptr;
    int count = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++) {
        auto childObj = Media::VisualTreeHelper::GetChild(parent, i);
        auto child = childObj.try_as<FrameworkElement>();
        if (child && pred(child))
            return child;
        if (auto found = FindDescendant(childObj, pred, maxDepth - 1))
            return found;
    }
    return nullptr;
}

bool HasName(FrameworkElement const& e, PCWSTR name) {
    return e.Name() == name;
}

bool HasClass(FrameworkElement const& e, PCWSTR cls) {
    return winrt::get_class_name(e) == cls;
}

void* AbiKey(IInspectable const& obj) {
    if (!obj)
        return nullptr;
    return winrt::get_abi(obj.as<winrt::Windows::Foundation::IUnknown>());
}

struct SavedProp {
    winrt::weak_ref<DependencyObject> obj;
    void* key;
    DependencyProperty dp{nullptr};
    IInspectable value{nullptr};
};

std::vector<SavedProp> g_savedProps;

void SetProp(DependencyObject const& obj,
             DependencyProperty const& dp,
             IInspectable const& value) {
    void* key = AbiKey(obj);
    bool saved = false;
    for (auto& s : g_savedProps) {
        if (s.key == key && s.dp == dp && s.obj.get()) {
            saved = true;
            break;
        }
    }
    if (!saved)
        g_savedProps.push_back({winrt::make_weak(obj), key, dp, obj.ReadLocalValue(dp)});
    obj.SetValue(dp, value);
}

void RestoreProps() {
    for (auto it = g_savedProps.rbegin(); it != g_savedProps.rend(); ++it) {
        auto obj = it->obj.get();
        if (!obj)
            continue;
        try {
            if (it->value == DependencyProperty::UnsetValue())
                obj.ClearValue(it->dp);
            else
                obj.SetValue(it->dp, it->value);
        } catch (...) {}
    }
    g_savedProps.clear();
}

struct DockState {
    void* frameKey = nullptr;
    winrt::weak_ref<FrameworkElement> frame;
    winrt::weak_ref<FrameworkElement> repeater;
    winrt::weak_ref<FrameworkElement> tray;
    winrt::weak_ref<FrameworkElement> startButton;
    winrt::event_token layoutToken{};
    DispatcherTimer timer{nullptr};
    DispatcherTimer motionTimer{nullptr};
    IInspectable moveHandler{nullptr};
    IInspectable exitHandler{nullptr};
    DWORD lastTrayLookup = 0;
    DWORD lastStartLookup = 0;
    ULONGLONG lastMotionTick = 0;
    double lastPointerX = -1e9;
    int trayAttempts = 0;
    int startAttempts = 0;
    int collapseMisses = 0;
    bool collapseHolds = true;
    bool motionRunning = false;
};

struct ZoomEntry {
    void* buttonKey = nullptr;
    winrt::weak_ref<UIElement> button;
    winrt::weak_ref<FrameworkElement> target;
    Media::ScaleTransform scale{nullptr};
    double currentScale = 1.0;
    double targetScale = 1.0;
};

std::vector<std::shared_ptr<DockState>> g_states;
std::vector<std::shared_ptr<ZoomEntry>> g_zooms;

XamlRoot GetTaskbarXamlRoot(HWND hWnd, bool isSecondary);

void FindTrayIfNeeded(DockState& st) {
    if (st.tray.get())
        return;
    DWORD now = GetTickCount();

    DWORD interval = st.trayAttempts < 10 ? 1000 : 10000;
    if (st.lastTrayLookup && now - st.lastTrayLookup < interval)
        return;
    st.lastTrayLookup = now;
    st.trayAttempts++;

    auto frame = st.frame.get();
    if (!frame || !frame.XamlRoot())
        return;
    auto content = frame.XamlRoot().Content();
    if (!content)
        return;
    auto tray = FindDescendant(
        content, [](FrameworkElement const& e) { return HasName(e, L"SystemTrayFrameGrid"); },
        20);
    if (tray) {
        st.tray = winrt::make_weak(tray);
        SetProp(tray, UIElement::OpacityProperty(), winrt::box_value(0.0));
        SetProp(tray, UIElement::IsHitTestVisibleProperty(), winrt::box_value(false));
    }
}

bool ContainsNoCase(std::wstring_view haystack, std::wstring_view needle) {
    if (haystack.empty() || needle.empty())
        return false;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
    return it != haystack.end();
}

void CollectByClassOrName(DependencyObject parent, std::wstring_view needle, int depth,
                          int maxDepth, std::vector<FrameworkElement>* out, int* visited) {
    if (!parent || depth > maxDepth || *visited > 4000)
        return;
    (*visited)++;
    if (auto fe = parent.try_as<FrameworkElement>()) {
        if (ContainsNoCase(winrt::get_class_name(fe), needle) ||
            ContainsNoCase(fe.Name(), needle)) {
            out->push_back(fe);
        }
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++)
        CollectByClassOrName(Media::VisualTreeHelper::GetChild(parent, i), needle, depth + 1,
                             maxDepth, out, visited);
}

void StyleStartButton(DockState& st) {
    if (auto start = st.startButton.get()) {
        if (st.collapseHolds && start.Visibility() != Visibility::Collapsed) {
            if (++st.collapseMisses > 3)
                st.collapseHolds = false;
            else
                start.Visibility(Visibility::Collapsed);
        }
        return;
    }

    DWORD now = GetTickCount();
    DWORD interval = st.startAttempts < 20 ? 1000 : 10000;
    if (st.lastStartLookup && now - st.lastStartLookup < interval)
        return;
    st.lastStartLookup = now;
    st.startAttempts++;

    auto frame = st.frame.get();
    if (!frame)
        return;

    std::vector<FrameworkElement> candidates;
    int visited = 0;
    CollectByClassOrName(frame, L"launchlistbutton", 0, 24, &candidates, &visited);
    if (candidates.empty())
        CollectByClassOrName(frame, L"startbutton", 0, 24, &candidates, &visited);
    if (candidates.empty())
        CollectByClassOrName(frame, L"start", 0, 24, &candidates, &visited);

    if (candidates.empty())
        return;

    for (auto const& candidate : candidates) {
        double width = candidate.ActualWidth();
        if (width > 120.0)
            continue;
        SetProp(candidate, UIElement::VisibilityProperty(),
                winrt::box_value(Visibility::Collapsed));
        SetProp(candidate, UIElement::OpacityProperty(), winrt::box_value(0.0));
        SetProp(candidate, UIElement::IsHitTestVisibleProperty(), winrt::box_value(false));
        st.startButton = winrt::make_weak(candidate);
        return;
    }
}

FrameworkElement ZoomTarget(UIElement const& button) {
    auto icon = FindDescendant(
        button, [](FrameworkElement const& e) { return HasName(e, L"Icon"); }, 4);
    if (icon)
        return icon;
    return FindChild(button, [](FrameworkElement const&) { return true; });
}

ZoomEntry* GetZoomEntry(UIElement const& button) {
    void* key = AbiKey(button);
    for (auto& z : g_zooms) {
        if (z->buttonKey == key && z->button.get()) {
            if (auto target = z->target.get()) {
                if (target.RenderTransform() == z->scale)
                    return z.get();
            }

            auto target = ZoomTarget(button);
            if (!target)
                return nullptr;
            z->currentScale = 1.0;
            z->targetScale = 1.0;
            z->target = winrt::make_weak(target);
            z->scale = Media::ScaleTransform();
            SetProp(target, UIElement::RenderTransformProperty(), z->scale);
            SetProp(target, UIElement::RenderTransformOriginProperty(),
                    winrt::box_value(winrt::Windows::Foundation::Point{0.5f, 0.5f}));
            return z.get();
        }
    }

    auto target = ZoomTarget(button);
    if (!target)
        return nullptr;
    auto z = std::make_shared<ZoomEntry>();
    z->buttonKey = key;
    z->button = winrt::make_weak(button);
    z->target = winrt::make_weak(target);
    z->scale = Media::ScaleTransform();
    SetProp(target, UIElement::RenderTransformProperty(), z->scale);
    SetProp(target, UIElement::RenderTransformOriginProperty(),
            winrt::box_value(winrt::Windows::Foundation::Point{0.5f, 0.5f}));
    g_zooms.push_back(z);
    return z.get();
}

bool MagnifyTick(double dtMs) {
    bool busy = false;
    for (auto& z : g_zooms) {
        if (!z->scale || z->currentScale == z->targetScale)
            continue;
        z->currentScale =
            Motion::Approach(z->currentScale, z->targetScale, dtMs, Motion::kFollowTauMs);
        z->scale.ScaleX(z->currentScale);
        z->scale.ScaleY(z->currentScale);
        busy = busy || z->currentScale != z->targetScale;
    }
    return busy;
}

void EnsureMotion(DockState& st) {
    if (!Motion::Enabled()) {
        for (auto& z : g_zooms) {
            if (!z->scale || z->currentScale == z->targetScale)
                continue;
            z->currentScale = z->targetScale;
            z->scale.ScaleX(z->currentScale);
            z->scale.ScaleY(z->currentScale);
        }
        return;
    }
    if (!st.motionTimer || st.motionRunning)
        return;
    st.motionRunning = true;
    st.lastMotionTick = GetTickCount64();
    st.motionTimer.Start();
}

void MotionTick(std::weak_ptr<DockState> weak) {
    auto st = weak.lock();
    if (!st)
        return;
    ULONGLONG now = GetTickCount64();
    double dt = std::clamp((double)(now - st->lastMotionTick), 1.0, 50.0);
    st->lastMotionTick = now;
    try {
        if (g_unloading || !MagnifyTick(dt)) {
            st->motionRunning = false;
            st->motionTimer.Stop();
        }
    } catch (...) {
        st->motionRunning = false;
        st->motionTimer.Stop();
    }
}

void SetMagnifyTargets(FrameworkElement const& repeater, double cursorX) {
    double maxScale = kMagnifyPercent / 100.0;
    double range = kMagnifyRange;

    int count = Media::VisualTreeHelper::GetChildrenCount(repeater);
    for (int i = 0; i < count; i++) {
        auto button = Media::VisualTreeHelper::GetChild(repeater, i).try_as<FrameworkElement>();
        if (!button || button.Visibility() != Visibility::Visible || button.ActualWidth() <= 0)
            continue;

        double center = button.TransformToVisual(repeater).TransformPoint({0, 0}).X +
                        button.ActualWidth() / 2;
        double d = (cursorX - center) / range;
        double scale = 1.0;
        if (std::abs(d) < 1.0) {
            double t = 0.5 + 0.5 * std::cos(d * 3.14159265358979);
            scale = 1.0 + (maxScale - 1.0) * t;
        }

        ZoomEntry* z = GetZoomEntry(button);
        if (z)
            z->targetScale = scale;
    }
}

void ReleaseMagnify() {
    std::erase_if(g_zooms, [](auto const& z) { return !z->button.get(); });
    for (auto& z : g_zooms) {
        if (z->scale)
            z->targetScale = 1.0;
    }
}

void AttachMagnify(std::shared_ptr<DockState> state) {
    if (!state)
        return;
    auto repeater = state->repeater.get();
    if (!repeater)
        return;

    winrt::weak_ref<FrameworkElement> weakRepeater = winrt::make_weak(repeater);
    std::weak_ptr<DockState> weakState = state;

    state->moveHandler = winrt::box_value(Input::PointerEventHandler(
        [weakRepeater, weakState](IInspectable const&, Input::PointerRoutedEventArgs const& args) {
            if (g_unloading)
                return;
            auto rep = weakRepeater.get();
            auto st = weakState.lock();
            if (!rep || !st)
                return;
            try {
                auto pos = args.GetCurrentPoint(rep).Position();
                if (std::abs(pos.X - st->lastPointerX) < 1.0)
                    return;
                st->lastPointerX = pos.X;
                SetMagnifyTargets(rep, pos.X);
                EnsureMotion(*st);
            } catch (...) {}
        }));

    state->exitHandler = winrt::box_value(Input::PointerEventHandler(
        [weakRepeater, weakState](IInspectable const&, Input::PointerRoutedEventArgs const& args) {
            if (g_unloading)
                return;
            auto rep = weakRepeater.get();
            auto st = weakState.lock();
            if (!rep || !st)
                return;
            try {
                auto pos = args.GetCurrentPoint(rep).Position();

                double slack = 12.0;
                bool inside = pos.X >= -slack && pos.Y >= -slack &&
                              pos.X <= rep.ActualWidth() + slack &&
                              pos.Y <= rep.ActualHeight() + slack;
                if (!inside) {
                    st->lastPointerX = -1e9;
                    ReleaseMagnify();
                    EnsureMotion(*st);
                }
            } catch (...) {}
        }));

    repeater.AddHandler(UIElement::PointerMovedEvent(), state->moveHandler, true);
    repeater.AddHandler(UIElement::PointerExitedEvent(), state->exitHandler, true);
    repeater.AddHandler(UIElement::PointerCanceledEvent(), state->exitHandler, true);
    repeater.AddHandler(UIElement::PointerCaptureLostEvent(), state->exitHandler, true);
}

void DetachMagnify(DockState& st) {
    if (auto repeater = st.repeater.get()) {
        try {
            if (st.moveHandler)
                repeater.RemoveHandler(UIElement::PointerMovedEvent(), st.moveHandler);
            if (st.exitHandler) {
                repeater.RemoveHandler(UIElement::PointerExitedEvent(), st.exitHandler);
                repeater.RemoveHandler(UIElement::PointerCanceledEvent(), st.exitHandler);
                repeater.RemoveHandler(UIElement::PointerCaptureLostEvent(), st.exitHandler);
            }
        } catch (...) {}
    }
    st.moveHandler = nullptr;
    st.exitHandler = nullptr;
}

bool IsFrameApplied(FrameworkElement const& frame) {
    void* key = AbiKey(frame);
    for (auto& st : g_states) {
        if (st->frameKey == key && st->frame.get())
            return true;
    }
    return false;
}

void OnLayoutUpdated(std::weak_ptr<DockState> weak) {
    if (g_unloading)
        return;
    auto st = weak.lock();
    if (!st)
        return;
    try {
        FindTrayIfNeeded(*st);
        StyleStartButton(*st);
    } catch (...) {}
}

void ApplyToFrame(FrameworkElement const& frame) {
    if (g_unloading || !frame || IsFrameApplied(frame))
        return;

    std::erase_if(g_states, [](auto const& st) { return !st->frame.get(); });

    auto repeater = FindDescendant(
        frame, [](FrameworkElement const& e) { return HasName(e, L"TaskbarFrameRepeater"); }, 6);
    if (!repeater)
        return;

    if (auto background = FindDescendant(
            frame, [](FrameworkElement const& e) {
                return HasClass(e, L"Taskbar.TaskbarBackground");
            }, 6)) {
        for (PCWSTR name : {L"BackgroundFill", L"BackgroundStroke"}) {
            auto el = FindDescendant(
                background, [name](FrameworkElement const& e) { return HasName(e, name); }, 4);
            if (!el)
                continue;
            SetProp(el, UIElement::OpacityProperty(), winrt::box_value(0.0));
            if (wcscmp(name, L"BackgroundFill") == 0) {
                if (auto holder = Media::VisualTreeHelper::GetParent(el).try_as<UIElement>())
                    SetProp(holder, UIElement::OpacityProperty(), winrt::box_value(0.0));
            }
        }
    }

    auto st = std::make_shared<DockState>();
    st->frameKey = AbiKey(frame);
    st->frame = winrt::make_weak(frame);
    st->repeater = winrt::make_weak(repeater);

    std::weak_ptr<DockState> weak = st;
    st->layoutToken = repeater.LayoutUpdated(
        [weak](IInspectable const&, IInspectable const&) { OnLayoutUpdated(weak); });

    st->motionTimer = DispatcherTimer();
    st->motionTimer.Interval(std::chrono::milliseconds(Motion::kFrameMs));
    st->motionTimer.Tick(
        [weak](IInspectable const&, IInspectable const&) { MotionTick(weak); });

    st->timer = DispatcherTimer();
    st->timer.Interval(std::chrono::milliseconds(1000));
    st->timer.Tick([weak](IInspectable const&, IInspectable const&) { OnLayoutUpdated(weak); });
    st->timer.Start();

    g_states.push_back(st);
    StyleStartButton(*st);
    FindTrayIfNeeded(*st);
    AttachMagnify(st);
}

void RestoreAll() {
    for (auto& st : g_states)
        DetachMagnify(*st);
    g_zooms.clear();
    for (auto& st : g_states) {
        try {
            if (st->timer)
                st->timer.Stop();
            if (st->motionTimer)
                st->motionTimer.Stop();
            if (auto repeater = st->repeater.get())
                repeater.LayoutUpdated(st->layoutToken);
        } catch (...) {}
    }
    g_states.clear();
    RestoreProps();
}

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;
CTaskBand_GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;

size_t g_taskbarHostElementOffset = 0x48;

void DetectTaskbarHostElementOffset(void* frameHeight) {
    g_taskbarHostElementOffset = 0x48;
    const BYTE* b = (const BYTE*)frameHeight;
    if (b && b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 &&
        b[6] == 0xC1 && b[7] <= 0x7F) {
        g_taskbarHostElementOffset = b[7];
    }
}

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot XamlRootFromTaskbarHostSharedPtr(void* taskbarHostSharedPtr[2]) {
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1])
        return nullptr;
    if (!taskbarHostSharedPtr[0]) {
        if (taskbarHostSharedPtr[1])
            std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
        return nullptr;
    }

    auto* unk = *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] + g_taskbarHostElementOffset);
    FrameworkElement fe = nullptr;
    if (unk)
        unk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(fe));

    auto result = fe ? fe.XamlRoot() : nullptr;
    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hWnd, bool isSecondary) {
    HWND hTaskSwWnd = isSecondary
                          ? FindWindowEx(hWnd, nullptr, L"WorkerW", nullptr)
                          : (HWND)GetProp(hWnd, L"TaskbandHWND");
    if (!hTaskSwWnd)
        return nullptr;

    void* vftable = isSecondary ? CSecondaryTaskBand_ITaskListWndSite_vftable
                                : CTaskBand_ITaskListWndSite_vftable;
    void* p = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    if (!p || !vftable)
        return nullptr;
    for (int i = 0; *(void**)p != vftable; i++) {
        if (i == 20)
            return nullptr;
        p = (void**)p + 1;
    }

    void* sharedPtr[2]{};
    if (isSecondary)
        CSecondaryTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    else
        CTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    return XamlRootFromTaskbarHostSharedPtr(sharedPtr);
}

FrameworkElement FindTaskbarFrameUp(DependencyObject current) {
    for (int i = 0; current && i < 30; i++) {
        if (auto fe = current.try_as<FrameworkElement>()) {
            if (HasClass(fe, L"Taskbar.TaskbarFrame"))
                return fe;
        }
        current = Media::VisualTreeHelper::GetParent(current);
    }
    return nullptr;
}

void ApplyToXamlRoot(XamlRoot const& xamlRoot) {
    auto content = xamlRoot ? xamlRoot.Content() : nullptr;
    if (!content) {
        return;
    }

    FrameworkElement frame = nullptr;
    if (auto contentFe = content.try_as<FrameworkElement>();
        contentFe && HasClass(contentFe, L"Taskbar.TaskbarFrame"))
        frame = contentFe;
    if (!frame)
        frame = FindDescendant(
            content,
            [](FrameworkElement const& e) { return HasClass(e, L"Taskbar.TaskbarFrame"); }, 30);

    if (!frame) {
        auto repeater = FindDescendant(
            content, [](FrameworkElement const& e) { return HasName(e, L"TaskbarFrameRepeater"); },
            30);
        if (repeater) {
            frame = FindTaskbarFrameUp(repeater);
            if (!frame) {
                auto parent = Media::VisualTreeHelper::GetParent(repeater);
                auto grand = parent ? Media::VisualTreeHelper::GetParent(parent) : nullptr;
                frame = grand ? grand.try_as<FrameworkElement>() : nullptr;
            }
        }
    }

    if (frame) {
        ApplyToFrame(frame);
        return;
    }

}

void RefreshFromTaskbarThread() {
    RestoreAll();
    if (g_unloading)
        return;

    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hWnd, LPARAM) -> BOOL {
            WCHAR cls[32];
            if (GetClassName(hWnd, cls, ARRAYSIZE(cls)) == 0)
                return TRUE;
            try {
                bool primary = _wcsicmp(cls, L"Shell_TrayWnd") == 0;
                bool secondary = _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
                if (primary || secondary) {
                    auto xamlRoot = GetTaskbarXamlRoot(hWnd, secondary);
                    ApplyToXamlRoot(xamlRoot);
                }
            } catch (...) {}
            return TRUE;
        },
        0);
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hWnd = nullptr;
    while ((hWnd = FindWindowEx(nullptr, hWnd, L"Shell_TrayWnd", nullptr))) {
        DWORD pid;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == GetCurrentProcessId())
            return hWnd;
    }
    return nullptr;
}

void RunOnTaskbarThread() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return;
    }

    static const UINT msg = RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    DWORD threadId = GetWindowThreadProcessId(hTaskbarWnd, nullptr);
    if (!threadId)
        return;

    if (threadId == GetCurrentThreadId()) {
        RefreshFromTaskbarThread();
        return;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION && ((const CWPSTRUCT*)lParam)->message == msg)
                RefreshFromTaskbarThread();
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook)
        return;

    DWORD_PTR result = 0;
    if (!SendMessageTimeout(hTaskbarWnd, msg, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                            &result))
    UnhookWindowsHookEx(hook);
}

using TaskbarFrame_OnLoaded_t = void(WINAPI*)(void* pThis, void* sender, void* args);
TaskbarFrame_OnLoaded_t TaskbarFrame_OnLoaded_Original;
void WINAPI TaskbarFrame_OnLoaded_Hook(void* pThis, void* sender, void* args) {
    TaskbarFrame_OnLoaded_Original(pThis, sender, args);
    if (g_unloading)
        return;
    try {
        FrameworkElement frame = nullptr;
        ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(frame));
        if (frame && HasClass(frame, L"Taskbar.TaskbarFrame"))
            ApplyToFrame(frame);
    } catch (...) {}
}

bool HookTaskbarDllSymbols() {
    HMODULE module = LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        return false;
    }

    {
        void* frameHeightAddress = nullptr;
        WindhawkUtils::SYMBOL_HOOK probe[] = {
            {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
             &frameHeightAddress},
        };
        if (WindhawkUtils::HookSymbols(module, probe, ARRAYSIZE(probe)))
            DetectTaskbarHostElementOffset(frameHeightAddress);
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
         &std__Ref_count_base__Decref_Original},
    };
    bool ok = WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
    return ok;
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskbarFrame::OnLoaded(struct winrt::Windows::Foundation::IInspectable const &,struct winrt::Windows::UI::Xaml::RoutedEventArgs const &))"},
         &TaskbarFrame_OnLoaded_Original,
         TaskbarFrame_OnLoaded_Hook,
         true},
    };
    if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks))) {
        return false;
    }
    return true;
}

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE m = GetModuleHandle(L"Taskbar.View.dll");
    return m ? m : GetModuleHandle(L"ExplorerExtensions.dll");
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module && !g_taskbarViewDllLoaded && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllLoaded.exchange(true)) {
        if (HookTaskbarViewDllSymbols(module))
            Wh_ApplyHookOperations();
    }
    return module;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace TopBar {
enum Action { kNone, kNotifications, kQuickSettings, kSearch, kStart };

struct Item {
    RECT rc;
    Action action;
};

HANDLE g_thread = nullptr;
std::atomic<HWND> g_wnd{nullptr};
std::vector<Item> g_items;
bool g_appBar = false;
constexpr UINT kAppBarMsg = WM_APP + 42;
constexpr UINT kRadioMsg = WM_APP + 43;
constexpr WCHAR kClassName[] = L"MinimalistTopBar";

void PressKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> inputs;
    for (WORD vk : keys) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        inputs.push_back(in);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = *it;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

void RunAction(Action action) {
    switch (action) {
        case kNotifications:
            PressKeys({VK_LWIN, 'N'});
            break;
        case kQuickSettings:
            PressKeys({VK_LWIN, 'A'});
            break;
        case kSearch:
            PressKeys({VK_LWIN, 'S'});
            break;
        case kStart:
            PressKeys({VK_LWIN});
            break;
        default:
            break;
    }
}

RECT PrimaryMonitorRect() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

int BarHeightPx(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    if (!dpi)
        dpi = 96;
    return MulDiv(kTopBarHeight, dpi, 96);
}

void ApplyBlur(HWND hwnd) {
    BOOL excluded = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
    int corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF noBorder = 0xFFFFFFFE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
}

void PositionBar(HWND hwnd) {
    RECT mon = PrimaryMonitorRect();
    int h = BarHeightPx(hwnd);
    RECT rc{mon.left, mon.top, mon.right, mon.top + h};
    if (g_appBar) {
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = hwnd;
        abd.uEdge = ABE_TOP;
        abd.rc = rc;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + h;
        SHAppBarMessage(ABM_SETPOS, &abd);
        rc = abd.rc;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE);
}

std::wstring DateText() {
    WCHAR date[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"ddd MMM d", date, 64, nullptr);
    return date;
}

std::wstring TimeText() {
    WCHAR time[64]{};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr,
                    L"h:mm tt", time, 64);
    return time;
}

float EdgePadding(float scale) {
    if (scale <= 1.0f)
        return 18.0f;
    if (scale <= 1.25f)
        return 25.0f;
    if (scale <= 1.5f)
        return 32.0f;
    return 32.0f * scale / 1.5f;
}

bool IsAirplaneMode() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement",
                     L"SystemRadioState", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

struct RadioSnapshot {
    bool known = false;
    bool anyOn = false;
    bool bluetoothOn = false;
};

std::atomic<bool> g_radioStop{false};
HANDLE g_radioThread = nullptr;
std::mutex g_radioMutex;
bool g_radioAirplane = false;
bool g_radioBluetooth = false;

DWORD WINAPI RadioThreadProc(void*) {
    bool apartment = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
    } catch (...) {}

    while (!g_radioStop) {
        RadioSnapshot snap;
        try {
            auto op = winrt::Windows::Devices::Radios::Radio::GetRadiosAsync();
            if (op.wait_for(std::chrono::seconds(2)) ==
                winrt::Windows::Foundation::AsyncStatus::Completed) {
                for (auto const& radio : op.GetResults()) {
                    snap.known = true;
                    bool on = radio.State() == winrt::Windows::Devices::Radios::RadioState::On;
                    if (on) {
                        snap.anyOn = true;
                        if (radio.Kind() == winrt::Windows::Devices::Radios::RadioKind::Bluetooth)
                            snap.bluetoothOn = true;
                    }
                }
            } else {
                op.Cancel();
            }
        } catch (...) {}

        bool airplane = IsAirplaneMode() || (snap.known && !snap.anyOn);
        bool bluetooth = !airplane && snap.bluetoothOn;
        bool changed;
        {
            std::lock_guard<std::mutex> lock(g_radioMutex);
            changed = g_radioAirplane != airplane || g_radioBluetooth != bluetooth;
            g_radioAirplane = airplane;
            g_radioBluetooth = bluetooth;
        }
        if (changed) {
            if (HWND bar = g_wnd.load())
                PostMessage(bar, kRadioMsg, 0, 0);
        }

        for (int i = 0; i < 50 && !g_radioStop; i++)
            Sleep(100);
    }

    if (apartment) {
        try {
            winrt::uninit_apartment();
        } catch (...) {}
    }
    return 0;
}

void StartRadioWorker() {
    if (g_radioThread)
        return;
    g_radioStop = false;
    g_radioThread = CreateThread(nullptr, 0, RadioThreadProc, nullptr, 0, nullptr);
}

void StopRadioWorker() {
    g_radioStop = true;
    if (!g_radioThread)
        return;
    WaitForSingleObject(g_radioThread, 4000);
    CloseHandle(g_radioThread);
    g_radioThread = nullptr;
}

void ReadRadioIcons(bool* airplane, bool* bluetooth) {
    std::lock_guard<std::mutex> lock(g_radioMutex);
    *airplane = g_radioAirplane;
    *bluetooth = g_radioBluetooth;
}

struct PaintKey {
    std::wstring date;
    std::wstring time;
    int batteryPercent = -1;
    int acLine = -1;
    bool airplane = false;
    bool bluetooth = false;
};

PaintKey CurrentPaintKey() {
    PaintKey key;
    key.date = DateText();
    key.time = TimeText();
    ReadRadioIcons(&key.airplane, &key.bluetooth);
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        key.batteryPercent = ps.BatteryLifePercent;
        key.acLine = ps.ACLineStatus;
    }
    return key;
}

bool SamePaintKey(PaintKey const& a, PaintKey const& b) {
    return a.date == b.date && a.time == b.time && a.batteryPercent == b.batteryPercent &&
           a.acLine == b.acLine && a.airplane == b.airplane && a.bluetooth == b.bluetooth;
}

PaintKey g_paintKey;
bool g_paintKeyValid = false;

void Paint(HWND hwnd);
void PresentLayered(HWND hwnd);

constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT_PTR kClockTimerId = 3;

constexpr UINT_PTR kPulseTimerId = 4;
constexpr double kPulseScale = 1.25;
constexpr double kPulseInMs = 320.0;
constexpr double kPulseHoldMs = 1000.0;
constexpr double kPulseOutMs = 450.0;
double g_batteryScale = 1.0;
ULONGLONG g_pulseStart = 0;

void StartBatteryPulse(HWND hwnd) {
    g_pulseStart = GetTickCount64();
    g_batteryScale = 1.0;
    SetTimer(hwnd, kPulseTimerId, Motion::kFrameMs, nullptr);
}

void StopBatteryPulse(HWND hwnd) {
    KillTimer(hwnd, kPulseTimerId);
    g_pulseStart = 0;
    g_batteryScale = 1.0;
}

bool PulseStep(double elapsed, double& scale) {
    double total = kPulseInMs + kPulseHoldMs + kPulseOutMs;
    if (elapsed >= total) {
        scale = 1.0;
        return false;
    }
    if (!Motion::Enabled()) {
        scale = elapsed < kPulseInMs + kPulseHoldMs ? kPulseScale : 1.0;
        return true;
    }
    double e;
    if (elapsed < kPulseInMs) {
        double t = elapsed / kPulseInMs;
        e = 1.0 - std::pow(1.0 - t, 3.0);
    } else if (elapsed < kPulseInMs + kPulseHoldMs) {
        e = 1.0;
    } else {
        double t = (elapsed - kPulseInMs - kPulseHoldMs) / kPulseOutMs;
        e = t < 0.5 ? 1.0 - 4.0 * t * t * t : 1.0 - (1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0);
    }
    scale = 1.0 + (kPulseScale - 1.0) * e;
    return true;
}

void RefreshBar(HWND hwnd) {
    PaintKey key = CurrentPaintKey();
    if (!g_paintKeyValid || !SamePaintKey(key, g_paintKey)) {
        int prevAc = g_paintKeyValid ? g_paintKey.acLine : -1;
        if (key.acLine == 1 && prevAc != -1 && prevAc != 1)
            StartBatteryPulse(hwnd);
        else if (key.acLine != 1 && g_pulseStart)
            StopBatteryPulse(hwnd);
        g_paintKey = key;
        g_paintKeyValid = true;
        Paint(hwnd);
    }
}

void ArmClockTimer(HWND hwnd) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    UINT ms = (UINT)((59 - now.wSecond) * 1000 + (1000 - now.wMilliseconds) + 30);
    SetTimer(hwnd, kClockTimerId, ms, nullptr);
}

HDC g_screenDC = nullptr;
HDC g_surfaceDC = nullptr;
HBITMAP g_surfaceBmp = nullptr;
HGDIOBJ g_surfaceOld = nullptr;
void* g_surfaceBits = nullptr;
int g_surfaceW = 0, g_surfaceH = 0;

double g_winAlpha = 1.0;
double g_winAlphaTarget = 1.0;
bool g_closing = false;
constexpr UINT_PTR kMotionTimerId = 2;
ULONGLONG g_lastMotionTick = 0;
bool g_motionRunning = false;

void StopBarMotion() {
    g_motionRunning = false;
    if (HWND hwnd = g_wnd.load())
        KillTimer(hwnd, kMotionTimerId);
}

void StartBarMotion() {
    if (!Motion::Enabled()) {
        g_winAlpha = g_winAlphaTarget;
        if (HWND hwnd = g_wnd.load())
            PresentLayered(hwnd);
        return;
    }
    HWND hwnd = g_wnd.load();
    if (!hwnd || g_motionRunning)
        return;
    g_motionRunning = true;
    g_lastMotionTick = GetTickCount64();
    SetTimer(hwnd, kMotionTimerId, Motion::kFrameMs, nullptr);
}

void DestroySurface() {
    if (g_surfaceDC) {
        if (g_surfaceOld)
            SelectObject(g_surfaceDC, g_surfaceOld);
        DeleteDC(g_surfaceDC);
        g_surfaceDC = nullptr;
    }
    if (g_surfaceBmp) {
        DeleteObject(g_surfaceBmp);
        g_surfaceBmp = nullptr;
    }
    g_surfaceOld = nullptr;
    g_surfaceBits = nullptr;
    g_surfaceW = g_surfaceH = 0;
}

bool EnsureSurface(int w, int h) {
    if (g_surfaceDC && g_surfaceBmp && g_surfaceW == w && g_surfaceH == h)
        return true;
    DestroySurface();
    if (!g_screenDC)
        g_screenDC = GetDC(nullptr);
    if (!g_screenDC)
        return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_surfaceDC = CreateCompatibleDC(g_screenDC);
    if (!g_surfaceDC)
        return false;
    g_surfaceBmp = CreateDIBSection(g_surfaceDC, &bi, DIB_RGB_COLORS, &g_surfaceBits, nullptr, 0);
    if (!g_surfaceBmp) {
        DeleteDC(g_surfaceDC);
        g_surfaceDC = nullptr;
        return false;
    }
    g_surfaceOld = SelectObject(g_surfaceDC, g_surfaceBmp);
    g_surfaceW = w;
    g_surfaceH = h;
    return true;
}

void PresentLayered(HWND hwnd) {
    if (!hwnd || !g_surfaceDC || g_surfaceW <= 0 || g_surfaceH <= 0)
        return;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr))
        return;
    POINT dst{wr.left, wr.top};
    SIZE size{g_surfaceW, g_surfaceH};
    POINT src{0, 0};
    BYTE alpha = (BYTE)std::lround(std::clamp(g_winAlpha, 0.0, 1.0) * 255.0);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, g_screenDC, &dst, &size, g_surfaceDC, &src, 0, &blend,
                        ULW_ALPHA);
}

void Paint(HWND hwnd) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0)
        return;
    float s = GetDpiForWindow(hwnd) / 96.0f;
    if (s <= 0)
        s = 1;

    if (!EnsureSurface(w, h))
        return;
    void* bits = g_surfaceBits;

    std::vector<Item> items;
    {
        using namespace Gdiplus;
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        {
            SolidBrush hitBrush(Gdiplus::Color(3, 0, 0, 0));
            g.FillRectangle(&hitBrush, RectF(0.0f, 0.0f, (REAL)w, (REAL)h));
        }

        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);

        auto measure = [&](std::wstring const& t, Font& f) {
            RectF box;
            g.MeasureString(t.c_str(), -1, &f, PointF(0, 0), &fmt, &box);
            return box.Width;
        };

        float u = s;
        float cy = h / 2.0f;

        Font clockFont(L"Segoe UI Variable Text", 14.0f * u, FontStyleRegular, UnitPixel);
        Font clockFallback(L"Segoe UI", 14.0f * u, FontStyleRegular, UnitPixel);
        Font& cf = clockFont.IsAvailable() ? clockFont : clockFallback;
        std::wstring dateText = DateText();
        std::wstring timeText = TimeText();
        float timeW = measure(timeText, cf);
        float dateW = measure(dateText, cf);

        float edge = EdgePadding(s);

        bool showStart = true;
        float startSize = 12.0f * u;
        float startL = edge;
        float startGap = 1.1f * u;
        float startPane = (startSize - startGap) / 2.0f;

        float right = w - edge;
        float timeL = right - timeW;
        float dateR = timeL - 11.5f * u;
        float dateL = dateR - dateW;
        float clockL = dateL;
        float ccR = clockL - 23.0f * u, ccW = 13.0f * u, ccL = ccR - ccW;
        float seR = ccL - 25.0f * u, seW = 12.0f * u, seL = seR - seW;
        bool airplane = false, bluetooth = false;
        ReadRadioIcons(&airplane, &bluetooth);
        float cursor = seL - 21.0f * u;
        float apR = 0, apW = 12.0f * u, apL = 0;
        if (airplane) {
            apR = cursor;
            apL = apR - apW;
            cursor = apL - 22.0f * u;
        }
        float blR = 0, blW = 8.0f * u, blL = 0;
        if (bluetooth) {
            blR = cursor;
            blL = blR - blW;
            cursor = blL - 22.0f * u;
        }
        float wfR = cursor, wfW = 15.0f * u, wfL = wfR - wfW;
        float btR = (airplane ? cursor : wfL - 22.0f * u), btW = 23.0f * u, btL = btR - btW;

        auto addItem = [&](float l, float r, float padL, float padR, Action action) {
            items.push_back({{(LONG)(l - padL), 0, (LONG)(r + padR), h}, action});
        };
        if (showStart)
            addItem(startL, startL + startSize, 10 * u, 12 * u, kStart);
        if (airplane)
            addItem(apL, apR, 8 * u, 11 * u, kQuickSettings);
        if (bluetooth)
            addItem(blL, blR, 11 * u, 11 * u, kQuickSettings);
        addItem(btL, btR, 8 * u, 11 * u, kQuickSettings);
        if (!airplane)
            addItem(wfL, wfR, 11 * u, 10.5f * u, kQuickSettings);
        addItem(seL, seR, 10.5f * u, 12.5f * u, kSearch);
        addItem(ccL, ccR, 12.5f * u, 11.5f * u, kQuickSettings);
        addItem(clockL, right, 11.5f * u, 8 * u, kNotifications);

        SYSTEM_POWER_STATUS ps{};
        bool hasBattery = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 &&
                          ps.BatteryLifePercent <= 100;
        int level = hasBattery ? ps.BatteryLifePercent : 100;
        bool charging = hasBattery && ps.ACLineStatus == 1;
        bool saver = hasBattery && (ps.SystemStatusFlag & 1);
        Gdiplus::Color batteryColor(255, 255, 255, 255);
        if (saver)
            batteryColor = Gdiplus::Color(255, 255, 214, 10);
        else if (level < 20)
            batteryColor = Gdiplus::Color(255, 255, 69, 58);
        else if (charging)
            batteryColor = Gdiplus::Color(255, 52, 199, 89);

        auto roundRect = [](GraphicsPath& path, float x, float y, float w, float h, float r) {
            r = std::min(r, std::min(w, h) / 2);
            path.AddArc(x, y, 2 * r, 2 * r, 180, 90);
            path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
            path.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
            path.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
            path.CloseFigure();
        };

        auto drawAll = [&](Gdiplus::Color color, float dy) {
            SolidBrush brush(color);
            Gdiplus::Color faint(BYTE(color.GetA() * 0.4), color.GetR(), color.GetG(),
                                 color.GetB());
            SolidBrush faintBrush(faint);

            if (showStart) {
                float x0 = startL;
                float y0 = cy - startSize / 2 + dy;
                auto pane = [&](float x, float y) {
                    GraphicsPath pf;
                    roundRect(pf, x, y, startPane, startPane, 1.0f * u);
                    g.FillPath(&brush, &pf);
                };
                pane(x0, y0);
                pane(x0 + startPane + startGap, y0);
                pane(x0, y0 + startPane + startGap);
                pane(x0 + startPane + startGap, y0 + startPane + startGap);
            }

            {
                if (g_batteryScale > 1.0001) {
                    float zx = btL + (btW - 2.0f * u) / 2, zy = cy + dy;
                    g.TranslateTransform(zx, zy);
                    g.ScaleTransform((float)g_batteryScale, (float)g_batteryScale);
                    g.TranslateTransform(-zx, -zy);
                }
                float bodyW = btW - 2.0f * u, bodyH = 11.0f * u;
                float x = btL, y = cy - bodyH / 2 + dy;
                GraphicsPath body;
                roundRect(body, x, y, bodyW, bodyH, 3.5f * u);
                g.FillPath(&faintBrush, &body);
                if (color.GetA() == 255) {
                    Pen outline(Gdiplus::Color(255, 255, 255, 255), 1.0f * u);
                    g.DrawPath(&outline, &body);
                }
                float inset = 1.5f * u;
                float fillW = (bodyW - 2 * inset) * level / 100.0f;
                if (fillW > 0) {
                    GraphicsPath fill;
                    roundRect(fill, x + inset, y + inset, fillW, bodyH - 2 * inset, 2.2f * u);
                    SolidBrush levelBrush(color.GetA() == 255 ? batteryColor : color);
                    g.FillPath(&levelBrush, &fill);
                }
                GraphicsPath cap;
                roundRect(cap, x + bodyW + 0.7f * u, cy - 2.2f * u + dy, 1.3f * u, 4.4f * u,
                          0.65f * u);
                g.FillPath(&faintBrush, &cap);
                g.ResetTransform();
            }

            if (bluetooth) {
                Pen pen(color, 1.4f * u);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                pen.SetLineJoin(LineJoinRound);
                float bx = blL + blW / 2, by = cy + dy;
                PointF pts[] = {{bx - 3.4f * u, by - 3.1f * u}, {bx + 3.3f * u, by + 3.1f * u},
                                {bx, by + 6.0f * u},           {bx, by - 6.0f * u},
                                {bx + 3.3f * u, by - 3.1f * u}, {bx - 3.4f * u, by + 3.1f * u}};
                g.DrawLines(&pen, pts, 6);
            }

            if (airplane) {
                float ax = apL + apW / 2, ay = cy + dy;
                static const float k[][2] = {
                    {0, -7},     {1.3f, -4},  {1.3f, -1.5f}, {7, 2.5f},  {7, 4},      {1.3f, 2},
                    {1.3f, 5},   {3, 6.5f},   {3, 7.5f},     {0, 6.8f},  {-3, 7.5f},  {-3, 6.5f},
                    {-1.3f, 5},  {-1.3f, 2},  {-7, 4},       {-7, 2.5f}, {-1.3f, -1.5f}, {-1.3f, -4}};
                PointF pts[18];
                for (int i = 0; i < 18; i++)
                    pts[i] = PointF(ax + k[i][0] * 0.86f * u, ay + k[i][1] * 0.86f * u);
                g.FillPolygon(&brush, pts, 18);
            }

            if (!airplane) {
                Pen pen(color, 2.1f * u);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                float ox = wfL + wfW / 2, oy = cy + 6.0f * u + dy;
                for (float r : {10.0f * u, 6.4f * u}) {
                    g.DrawArc(&pen, ox - r, oy - r, 2 * r, 2 * r, 225.0f, 90.0f);
                }
                float r0 = 3.4f * u;
                g.FillPie(&brush, ox - r0, oy - r0, 2 * r0, 2 * r0, 225.0f, 90.0f);
            }

            {
                Pen pen(color, 1.7f * u);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                float top = cy - 6.0f * u + dy;
                float r = 4.3f * u;
                float ccx = seL + r + 0.8f * u, ccy = top + r + 0.8f * u;
                g.DrawEllipse(&pen, ccx - r, ccy - r, 2 * r, 2 * r);
                float d = r * 0.7071f;
                g.DrawLine(&pen, ccx + d, ccy + d, seR - 0.8f * u, top + 12.0f * u - 0.8f * u);
            }

            {
                float pillH = 5.8f * u, gap = 1.2f * u;
                float top = cy - (2 * pillH + gap) / 2 + dy;
                Pen pen(color, 1.2f * u);
                for (int i = 0; i < 2; i++) {
                    float y = top + i * (pillH + gap);
                    GraphicsPath pill;
                    roundRect(pill, ccL + 0.6f * u, y + 0.6f * u, ccW - 1.2f * u,
                              pillH - 1.2f * u, (pillH - 1.2f * u) / 2);
                    g.DrawPath(&pen, &pill);
                    float k = pillH - 1.0f * u;
                    g.FillEllipse(&brush, ccL + 1.3f * u, y + 0.5f * u, k, k);
                }
            }

            g.DrawString(dateText.c_str(), -1, &cf, RectF(dateL, dy, dateW + 2, (REAL)h), &fmt,
                         &brush);
            g.DrawString(timeText.c_str(), -1, &cf, RectF(timeL, dy, timeW + 2, (REAL)h), &fmt,
                         &brush);
        };

        drawAll(Gdiplus::Color(70, 0, 0, 0), 1.0f * u);
        drawAll(Gdiplus::Color(255, 255, 255, 255), 0);
    }

    g_items = std::move(items);

    static bool loggedEmpty = false;
    if (g_items.empty() && !loggedEmpty) {
        loggedEmpty = true;
    }

    PresentLayered(hwnd);
}

void BarMotionTick() {
    ULONGLONG now = GetTickCount64();
    double dt = (double)(now - g_lastMotionTick);
    g_lastMotionTick = now;
    dt = std::clamp(dt, 1.0, 50.0);

    HWND hwnd = g_wnd.load();

    double alpha = Motion::Approach(g_winAlpha, g_winAlphaTarget, dt, Motion::kFollowTauMs);
    bool moved = alpha != g_winAlpha;
    g_winAlpha = alpha;
    if (moved && hwnd)
        PresentLayered(hwnd);

    if (!moved) {
        StopBarMotion();
        if (g_closing && hwnd) {
            g_closing = false;
            DestroyWindow(hwnd);
        }
    }
}

int HitTest(int x, int y) {
    for (size_t i = 0; i < g_items.size(); i++) {
        POINT pt{x, y};
        if (PtInRect(&g_items[i].rc, pt))
            return (int)i;
    }
    return -1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER: {
            if (wParam == kMotionTimerId) {
                BarMotionTick();
                return 0;
            }
            if (wParam == kPulseTimerId) {
                double scale = 1.0;
                bool more = g_pulseStart &&
                            PulseStep((double)(GetTickCount64() - g_pulseStart), scale);
                g_batteryScale = scale;
                if (!more)
                    StopBatteryPulse(hwnd);
                Paint(hwnd);
                return 0;
            }
            if (wParam == kClockTimerId) {
                KillTimer(hwnd, kClockTimerId);
                RefreshBar(hwnd);
                ArmClockTimer(hwnd);
                return 0;
            }
            RefreshBar(hwnd);
            return 0;
        }
        case kRadioMsg:
            RefreshBar(hwnd);
            return 0;
        case WM_POWERBROADCAST:
            RefreshBar(hwnd);
            return TRUE;
        case WM_TIMECHANGE:
            KillTimer(hwnd, kClockTimerId);
            ArmClockTimer(hwnd);
            RefreshBar(hwnd);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(nullptr, HitTest(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONUP: {
            int hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit >= 0) {
                RunAction(g_items[hit].action);
            }
            return 0;
        }
        case kAppBarMsg:
            if (wParam == ABN_POSCHANGED)
                PositionBar(hwnd);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PositionBar(hwnd);
            g_paintKeyValid = false;
            Paint(hwnd);
            return 0;
        case WM_SETTINGCHANGE:
            ApplyBlur(hwnd);
            g_paintKeyValid = false;
            Paint(hwnd);
            break;
        case WM_CLOSE:

            if (Motion::Enabled() && !g_closing && g_winAlpha > 0.02 && g_surfaceDC) {
                g_closing = true;
                g_winAlphaTarget = 0.0;
                StartBarMotion();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kPollTimerId);
            KillTimer(hwnd, kClockTimerId);
            KillTimer(hwnd, kMotionTimerId);
            KillTimer(hwnd, kPulseTimerId);
            if (g_appBar) {
                APPBARDATA abd{sizeof(abd)};
                abd.hWnd = hwnd;
                SHAppBarMessage(ABM_REMOVE, &abd);
                g_appBar = false;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

DWORD WINAPI ThreadProc(void*) {
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    StartRadioWorker();

    HINSTANCE instance = (HINSTANCE)&__ImageBase;
    WNDCLASSEX wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassEx(&wc);

    RECT mon = PrimaryMonitorRect();
    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName,
        L"The Minimalist Shell top bar", WS_POPUP, mon.left, mon.top, mon.right - mon.left,
        kTopBarHeight, nullptr, nullptr, instance, nullptr);

    if (hwnd) {
        g_wnd = hwnd;
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = hwnd;
        abd.uCallbackMessage = kAppBarMsg;
        g_appBar = SHAppBarMessage(ABM_NEW, &abd) != 0;
        ApplyBlur(hwnd);
        PositionBar(hwnd);
        g_winAlpha = Motion::Enabled() ? 0.0 : 1.0;
        g_winAlphaTarget = 1.0;
        g_closing = false;
        g_motionRunning = false;
        Paint(hwnd);
        g_paintKey = CurrentPaintKey();
        g_paintKeyValid = true;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, kPollTimerId, 30000, nullptr);
        ArmClockTimer(hwnd);
        StartBarMotion();

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    g_wnd = nullptr;
    g_items.clear();
    StopRadioWorker();
    DestroySurface();
    if (g_screenDC) {
        ReleaseDC(nullptr, g_screenDC);
        g_screenDC = nullptr;
    }
    UnregisterClass(kClassName, instance);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}

void Start() {
    if (g_thread)
        return;
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
}

bool Stop() {
    if (!g_thread)
        return true;
    if (HWND hwnd = g_wnd.load())
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(g_thread, 5000) == WAIT_TIMEOUT) {
        return false;
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
    return true;
}
}

BOOL Wh_ModInit() {
    if (!HookTaskbarDllSymbols()) {
        return FALSE;
    }

    if (HMODULE m = GetTaskbarViewModuleHandle()) {
        g_taskbarViewDllLoaded = true;
        HookTaskbarViewDllSymbols(m);
    } else {
        HMODULE kb = GetModuleHandle(L"kernelbase.dll");
        auto pLoadLibraryExW = (LoadLibraryExW_t)GetProcAddress(kb, "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pLoadLibraryExW, LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_taskbarViewDllLoaded) {
        if (HMODULE m = GetTaskbarViewModuleHandle()) {
            if (!g_taskbarViewDllLoaded.exchange(true)) {
                if (HookTaskbarViewDllSymbols(m))
                    Wh_ApplyHookOperations();
            }
        }
    }
    RunOnTaskbarThread();
    TopBar::Start();
    SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings",
                       SMTO_ABORTIFHUNG, 1000, nullptr);
}

void Wh_ModBeforeUninit() {
    g_unloading = true;
    TopBar::Stop();
    RunOnTaskbarThread();
}

void Wh_ModUninit() {
}
