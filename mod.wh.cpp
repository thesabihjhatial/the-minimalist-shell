// ==WindhawkMod==
// @id              the-minimalist-shell
// @name            The Minimalist Shell
// @description     Turns the Windows 11 shell into a minimalistic MacOS style shell with genie minimize and restore
// @version         1.0
// @author          Muhammad Sabih
// @include         *
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lgdi32 -luser32 -lgdiplus -lshell32 -ladvapi32 -ldwmapi -luuid -luxtheme -lwlanapi
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- showBatteryPercentage: false
  $name: Show battery percentage
  $description: Shows charge level as text, next to the battery icon in the top bar
*/
// ==/WindhawkModSettings==

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>
#include <wlanapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif

#undef GetCurrentTime

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;

constexpr int kMagnifyPercent = 150;
constexpr double kMagnifyRange = 90.0;
constexpr int kTopBarHeight = 24;
std::atomic<bool> g_taskbarViewDllLoaded;
std::atomic<bool> g_unloading;

namespace Motion {
constexpr double kFollowTauMs = 55.0;
constexpr int kFrameMs = 16;

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

HBITMAP CreateDib(HDC dc, int w, int h, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
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
    winrt::weak_ref<UIElement> button;
    winrt::weak_ref<FrameworkElement> target;
    Media::ScaleTransform scale{nullptr};
    double currentScale = 1.0;
    double targetScale = 1.0;
};

std::vector<std::shared_ptr<DockState>> g_states;
std::unordered_map<void*, std::shared_ptr<ZoomEntry>> g_zooms;

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
    for (auto needle : {L"launchlistbutton", L"startbutton"}) {
        int visited = 0;
        CollectByClassOrName(frame, needle, 0, 24, &candidates, &visited);
        if (!candidates.empty())
            break;
    }

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

void ApplyScale(ZoomEntry const& z) {
    z.scale.ScaleX(z.currentScale);
    z.scale.ScaleY(z.currentScale);
}

void BindZoom(ZoomEntry& z, FrameworkElement const& target) {
    z.currentScale = 1.0;
    z.targetScale = 1.0;
    z.target = winrt::make_weak(target);
    z.scale = Media::ScaleTransform();
    SetProp(target, UIElement::RenderTransformProperty(), z.scale);
    SetProp(target, UIElement::RenderTransformOriginProperty(),
            winrt::box_value(winrt::Windows::Foundation::Point{0.5f, 0.5f}));
}

ZoomEntry* GetZoomEntry(UIElement const& button) {
    void* key = AbiKey(button);
    auto it = g_zooms.find(key);
    if (it != g_zooms.end() && it->second->button.get()) {
        auto& z = it->second;
        if (auto target = z->target.get()) {
            if (target.RenderTransform() == z->scale)
                return z.get();
        }

        auto target = ZoomTarget(button);
        if (!target)
            return nullptr;
        BindZoom(*z, target);
        return z.get();
    }

    auto target = ZoomTarget(button);
    if (!target)
        return nullptr;
    auto z = std::make_shared<ZoomEntry>();
    z->button = winrt::make_weak(button);
    BindZoom(*z, target);
    return (g_zooms[key] = z).get();
}

bool MagnifyTick(double dtMs) {
    bool busy = false;
    for (auto& [key, z] : g_zooms) {
        if (z->currentScale == z->targetScale)
            continue;
        z->currentScale =
            Motion::Approach(z->currentScale, z->targetScale, dtMs, Motion::kFollowTauMs);
        ApplyScale(*z);
        busy = busy || z->currentScale != z->targetScale;
    }
    return busy;
}

void EnsureMotion(DockState& st) {
    if (!Motion::Enabled()) {
        for (auto& [key, z] : g_zooms) {
            if (z->currentScale == z->targetScale)
                continue;
            z->currentScale = z->targetScale;
            ApplyScale(*z);
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
    std::erase_if(g_zooms, [](auto const& entry) { return !entry.second->button.get(); });
    for (auto& [key, z] : g_zooms)
        z->targetScale = 1.0;
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
        if (st->timer && st->tray.get() && st->startButton.get())
            st->timer.Stop();
    } catch (...) {}
}

void ApplyToFrame(FrameworkElement const& frame) {
    if (g_unloading || !frame || IsFrameApplied(frame))
        return;

    std::erase_if(g_states, [](auto const& st) { return !st->frame.get(); });
    std::erase_if(g_savedProps, [](auto const& s) { return !s.obj.get(); });

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

    if (frame)
        ApplyToFrame(frame);
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
    SendMessageTimeout(hTaskbarWnd, msg, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
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

    void* frameHeightAddress = nullptr;
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
         &frameHeightAddress,
         nullptr,
         true},
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
    if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks)))
        return false;
    DetectTaskbarHostElementOffset(frameHeightAddress);
    return true;
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskbarFrame::OnLoaded(struct winrt::Windows::Foundation::IInspectable const &,struct winrt::Windows::UI::Xaml::RoutedEventArgs const &))"},
         &TaskbarFrame_OnLoaded_Original,
         TaskbarFrame_OnLoaded_Hook,
         true},
    };
    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
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

PCWSTR BaseName(PCWSTR path) {
    PCWSTR name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

namespace Translucency {
struct Saved {
    int backdrop = DWMSBT_AUTO;
    MARGINS margins{};
};

constexpr WCHAR kAppliedProp[] = L"MinimalistTranslucent";
constexpr int kBackdropAcrylic = DWMSBT_TRANSIENTWINDOW;
constexpr WCHAR kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

using DrawTextWithGlow_t = HRESULT(WINAPI*)(HDC, LPWSTR, UINT, RECT*, DWORD, COLORREF, COLORREF,
                                            UINT, UINT, BOOL, DTT_CALLBACK_PROC, LPARAM);
using DrawThemeBackground_t = decltype(&DrawThemeBackground);
using DrawThemeBackgroundEx_t = decltype(&DrawThemeBackgroundEx);
using DrawThemeText_t = decltype(&DrawThemeText);
using DrawThemeTextEx_t = decltype(&DrawThemeTextEx);
using DwmExtendFrameIntoClientArea_t = decltype(&DwmExtendFrameIntoClientArea);
using DwmSetWindowAttribute_t = decltype(&DwmSetWindowAttribute);
using ExtTextOutW_t = decltype(&ExtTextOutW);
using FillRect_t = decltype(&FillRect);
using GetSysColor_t = decltype(&GetSysColor);
using GetSysColorBrush_t = decltype(&GetSysColorBrush);
using GetThemeColor_t = decltype(&GetThemeColor);
using NtUserCreateWindowEx_t = HWND(WINAPI*)(DWORD, PVOID, LPCWSTR, PVOID, DWORD, LONG, LONG,
                                             LONG, LONG, HWND, HMENU, HINSTANCE, LPVOID, DWORD,
                                             DWORD, DWORD, VOID*);

DrawTextWithGlow_t DrawTextWithGlow_Original;
DrawThemeBackground_t DrawThemeBackground_Original;
DrawThemeBackgroundEx_t DrawThemeBackgroundEx_Original;
DrawThemeText_t DrawThemeText_Original;
DrawThemeTextEx_t DrawThemeTextEx_Original;
DwmExtendFrameIntoClientArea_t DwmExtendFrameIntoClientArea_Original;
DwmSetWindowAttribute_t DwmSetWindowAttribute_Original;
ExtTextOutW_t ExtTextOutW_Original;
FillRect_t FillRect_Original;
GetSysColor_t GetSysColor_Original;
GetSysColorBrush_t GetSysColorBrush_Original;
GetThemeColor_t GetThemeColor_Original;
NtUserCreateWindowEx_t NtUserCreateWindowEx_Original;

std::array<HBRUSH, COLOR_MENUBAR + 1> g_brushes{};
std::mutex g_brushMutex;
std::atomic<bool> g_dark{false};
bool g_explorer = false;
std::unordered_map<HWND, Saved> g_saved;
std::mutex g_savedMutex;

std::array<BYTE, 256> BuildTextGamma() {
    std::array<BYTE, 256> lut{};
    for (int i = 0; i < 256; i++) {
        float a = i / 255.0f;
        lut[i] = (BYTE)(std::pow(a, 1.0f / (1.2f + 0.3f * a)) * 255.0f + 0.5f);
    }
    return lut;
}

std::array<BYTE, 256> const g_textGamma = BuildTextGamma();

bool HasClass(HWND hwnd, std::initializer_list<PCWSTR> classes) {
    WCHAR cls[64]{};
    if (!hwnd || !GetClassName(hwnd, cls, ARRAYSIZE(cls)))
        return false;
    for (PCWSTR name : classes) {
        if (wcscmp(cls, name) == 0)
            return true;
    }
    return false;
}

bool IsApplied(HWND hwnd) {
    return hwnd && GetProp(hwnd, kAppliedProp);
}

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked;
}

bool IsDarkMode() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                 nullptr, &value, &size);
    return value == 0;
}

bool IsSurface(HWND hwnd) {
    return HasClass(hwnd, {L"#32768", L"DropDown", L"ViewControlClass", L"tooltips_class32"});
}

bool IsGlassWindow(HWND hwnd) {
    if (!g_explorer || GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow() ||
        HasClass(hwnd, {L"Progman", L"Shell_SecondaryTrayWnd", L"Shell_TrayWnd", L"WorkerW"}))
        return false;
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) ||
        (IsCloaked(hwnd) && !HasClass(hwnd, {L"ApplicationFrameWindow"})) ||
        (!IsWindowEnabled(hwnd) && !IsWindowVisible(hwnd)))
        return false;
    if ((style & WS_POPUPWINDOW) == WS_POPUPWINDOW ||
        (style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW || (exStyle & WS_EX_DLGMODALFRAME))
        return true;
    return (style & WS_CAPTION) == WS_CAPTION && (style & WS_SYSMENU) &&
           (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME));
}

std::wstring ThemeClass(HTHEME theme) {
    using GetClass_t = HRESULT(WINAPI*)(HTHEME, LPWSTR, int);
    static auto getClass =
        (GetClass_t)GetProcAddress(GetModuleHandle(L"uxtheme.dll"), MAKEINTRESOURCEA(74));
    WCHAR name[255]{};
    if (!getClass || FAILED(getClass(theme, name, ARRAYSIZE(name))))
        return {};
    return name;
}

HRESULT ExtendFrame(HWND hwnd, MARGINS const& margins) {
    auto extend = DwmExtendFrameIntoClientArea_Original ? DwmExtendFrameIntoClientArea_Original
                                                        : DwmExtendFrameIntoClientArea;
    return extend(hwnd, &margins);
}

bool IsGlassDC(HDC hdc) {
    HWND hwnd = WindowFromDC(hdc);
    return !hwnd || IsApplied(GetAncestor(hwnd, GA_ROOT));
}

void Refresh(HWND root) {
    EnumChildWindows(
        root,
        [](HWND hwnd, LPARAM) -> BOOL {
            PostMessage(hwnd, WM_SYSCOLORCHANGE, 0, 0);
            PostMessage(hwnd, WM_THEMECHANGED, 0, 0);
            return TRUE;
        },
        0);
    PostMessage(root, WM_SYSCOLORCHANGE, 0, 0);
    PostMessage(root, WM_THEMECHANGED, 0, 0);
    RedrawWindow(root, nullptr, nullptr,
                 RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_INVALIDATE);
}

HRESULT SetAttribute(HWND hwnd, DWORD attribute, int value) {
    auto set = DwmSetWindowAttribute_Original ? DwmSetWindowAttribute_Original
                                              : DwmSetWindowAttribute;
    return set(hwnd, attribute, &value, sizeof(value));
}

void ApplySurface(HWND hwnd) {
    if (g_unloading || IsApplied(hwnd))
        return;
    SetProp(hwnd, kAppliedProp, (HANDLE)1);
    if (g_dark)
        SetAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, TRUE);
    DWM_BLURBEHIND blur{DWM_BB_ENABLE | DWM_BB_BLURREGION | DWM_BB_TRANSITIONONMAXIMIZED, TRUE,
                        CreateRectRgn(0, 0, -1, -1), TRUE};
    DwmEnableBlurBehindWindow(hwnd, &blur);
    DeleteObject(blur.hRgnBlur);
    DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, 0);
    SetAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, kBackdropAcrylic);
    SetAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND);
}

void ApplyWindow(HWND hwnd) {
    if (g_unloading || IsApplied(hwnd))
        return;
    SetProp(hwnd, kAppliedProp, (HANDLE)1);
    int backdrop = DWMSBT_AUTO;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                                        sizeof(backdrop)))) {
        std::lock_guard<std::mutex> lock(g_savedMutex);
        g_saved[hwnd].backdrop = backdrop;
    }
    if (g_dark)
        SetAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, TRUE);
    SetAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, kBackdropAcrylic);
    ExtendFrame(hwnd, {-1, -1, -1, -1});
}

void Apply(HWND hwnd) {
    if (IsSurface(hwnd))
        ApplySurface(hwnd);
    else if (IsGlassWindow(hwnd))
        ApplyWindow(hwnd);
}

void Restore(HWND hwnd) {
    if (!IsApplied(hwnd))
        return;
    RemoveProp(hwnd, kAppliedProp);
    if (IsSurface(hwnd)) {
        DWM_BLURBEHIND blur{DWM_BB_ENABLE, FALSE, nullptr, FALSE};
        DwmEnableBlurBehindWindow(hwnd, &blur);
        SetAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_NONE);
        return;
    }
    Saved saved;
    {
        std::lock_guard<std::mutex> lock(g_savedMutex);
        if (auto it = g_saved.find(hwnd); it != g_saved.end())
            saved = it->second;
    }
    SetAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, saved.backdrop);
    ExtendFrame(hwnd, saved.margins);
    Refresh(hwnd);
}

COLORREF GlassSysColor(int index) {
    switch (index) {
        case COLOR_BACKGROUND:
        case COLOR_BTNFACE:
        case COLOR_INACTIVEBORDER:
        case COLOR_INFOBK:
        case COLOR_MENU:
        case COLOR_MENUBAR:
        case COLOR_SCROLLBAR:
        case COLOR_WINDOW:
            return RGB(0, 0, 0);
        case COLOR_3DLIGHT:
            return RGB(4, 4, 4);
        case COLOR_APPWORKSPACE:
            return RGB(8, 8, 8);
        case COLOR_3DDKSHADOW:
            return RGB(16, 16, 16);
        case COLOR_ACTIVEBORDER:
        case COLOR_BTNSHADOW:
            return RGB(32, 32, 32);
        case COLOR_BTNHIGHLIGHT:
            return RGB(64, 64, 64);
        case COLOR_WINDOWFRAME:
            return RGB(96, 96, 96);
    }
    if (!g_dark)
        return GetSysColor_Original(index);
    switch (index) {
        case COLOR_GRAYTEXT:
            return RGB(128, 128, 128);
        case COLOR_INACTIVECAPTIONTEXT:
            return RGB(160, 160, 160);
        case COLOR_WINDOWTEXT:
            return RGB(240, 240, 240);
        case COLOR_BTNTEXT:
        case COLOR_CAPTIONTEXT:
        case COLOR_HIGHLIGHTTEXT:
        case COLOR_INFOTEXT:
        case COLOR_MENUTEXT:
            return RGB(255, 255, 255);
    }
    return GetSysColor_Original(index);
}

bool IsClearPart(std::wstring const& theme, int part, int state) {
    if (theme == L"Menu")
        return part == MENU_POPUPBACKGROUND || part == MENU_POPUPBORDERS ||
               part == MENU_POPUPGUTTER || part == MENU_POPUPCHECKBACKGROUND ||
               ((part == MENU_POPUPITEM || part == 27) && state != MPI_HOT) ||
               (g_explorer && (part == MENU_BARBACKGROUND ||
                               (part == MENU_BARITEM && state == MBI_NORMAL)));
    if (theme == L"Tooltip")
        return part == TTP_STANDARD || part == TTP_BALLOON || (g_explorer && part == TTP_BALLOONSTEM);
    if (!g_explorer)
        return false;
    if (theme == L"AeroWizard")
        return part == AW_TITLEBAR || part == AW_HEADERAREA || part == AW_CONTENTAREA ||
               part == AW_COMMANDAREA;
    if (theme == L"CommandModule")
        return part == 1 && state == 0;
    if (theme == L"CommonItemsDialog" || theme == L"PreviewPane")
        return part == 1;
    if (theme == L"ControlPanel")
        return part == CPANEL_CONTENTPANE || part == CPANEL_CONTENTPANELINE ||
               part == CPANEL_BANNERAREA || part == CPANEL_LARGECOMMANDAREA ||
               part == CPANEL_SMALLCOMMANDAREA;
    if (theme == L"Header")
        return part == 0 || (part == HP_HEADERITEM && (state == HIS_NORMAL ||
                                                       state == HIS_SORTEDNORMAL ||
                                                       state == HIS_ICONNORMAL));
    if (theme == L"Rebar")
        return (part == RP_BAND || part == RP_BACKGROUND) && state == 0;
    if (theme == L"Status")
        return part == 0;
    if (theme == L"Tab")
        return part == TABP_PANE;
    if (theme == L"TaskDialog")
        return part == TDLG_PRIMARYPANEL ||
               ((part == TDLG_CONTENTPANE || part == TDLG_FOOTNOTEPANE ||
                 part == TDLG_FOOTNOTESEPARATOR || part == TDLG_SECONDARYPANEL) &&
                state == 0);
    return false;
}

bool ExplorerThemeColor(std::wstring const& theme, int part, int state, int prop, HRESULT hr,
                        COLORREF* color) {
    bool dark = g_dark;
    bool groupHeader = theme == L"ListView" && part == LVP_GROUPHEADER;
    if (prop == TMT_TEXTCOLOR) {
        if (FAILED(hr))
            return false;
        bool grey = *color == 0x006D6D6D;
        int luma = (GetRValue(*color) * 2 + GetGValue(*color) * 5 + GetBValue(*color)) / 8;
        if (!dark && grey && theme == L"ItemsView" && ((part == 4 && state == 1) || part == 5))
            *color = RGB(0, 0, 0);
        else if (!dark && grey && theme == L"ListView" && part == LVP_LISTITEM)
            *color = RGB(0, 0, 0);
        else if (dark && (groupHeader || theme == L"Header" || theme == L"PreviewPane" ||
                          luma < 96))
            *color = RGB(255, 255, 255);
        return true;
    }
    if (groupHeader) {
        if (dark)
            *color = RGB(255, 255, 255);
        return true;
    }
    if (theme == L"PreviewPane" && prop == TMT_FILLCOLOR && part >= 5 && part <= 7) {
        *color = part == 6 ? RGB(192, 192, 192) : RGB(255, 255, 255);
        return true;
    }
    if (prop == TMT_FILLCOLOR || prop == TMT_FILLCOLORHINT) {
        *color = RGB(0, 0, 0);
        return true;
    }
    return false;
}

bool IsNearBlack(HBRUSH brush) {
    LOGBRUSH info{};
    if (!GetObject(brush, sizeof(info), &info) || info.lbStyle != BS_SOLID)
        return false;
    return GetRValue(info.lbColor) <= 0x20 && GetGValue(info.lbColor) <= 0x20 &&
           GetBValue(info.lbColor) <= 0x20;
}

bool ShouldComposeText(HDC hdc, UINT options, LPCRECT rect, LPCWSTR text, UINT count) {
    if (!hdc || !text || !count || !options || (GetTextAlign(hdc) & TA_UPDATECP) ||
        ((options & (ETO_OPAQUE | ETO_CLIPPED)) && (!rect || IsRectEmpty(rect))))
        return false;
    return IsGlassDC(hdc);
}

bool TextBounds(HDC hdc, POINT point, UINT options, LPCRECT clip, LPCWSTR text, UINT count,
                INT const* dx, RECT& bounds) {
    SIZE size{};
    if (!((options & ETO_GLYPH_INDEX) ? GetTextExtentPointI(hdc, (WORD*)text, count, &size)
                                      : GetTextExtentPoint32W(hdc, text, count, &size)))
        return false;
    if (dx) {
        int width = 0, height = 0;
        UINT stride = (options & ETO_PDY) ? 2 : 1;
        for (UINT i = 0; i < count; i++) {
            width += dx[i * stride];
            if (options & ETO_PDY)
                height += dx[i * stride + 1];
        }
        size.cx = std::max<LONG>(size.cx, width);
        size.cy += std::abs(height);
    }
    UINT align = GetTextAlign(hdc);
    UINT vertical = align & (TA_BOTTOM | TA_BASELINE);
    UINT horizontal = align & (TA_RIGHT | TA_CENTER);
    TEXTMETRIC metrics;
    if (vertical == TA_BASELINE && GetTextMetrics(hdc, &metrics))
        point.y -= metrics.tmAscent;
    else if (vertical == TA_BOTTOM)
        point.y -= size.cy;
    if (horizontal == TA_CENTER)
        point.x -= size.cx / 2;
    else if (horizontal == TA_RIGHT)
        point.x -= size.cx;
    SetRect(&bounds, point.x, point.y, point.x + size.cx, point.y + size.cy);
    if (clip && (options & ETO_CLIPPED))
        IntersectRect(&bounds, &bounds, clip);
    if (clip && (options & ETO_OPAQUE))
        UnionRect(&bounds, &bounds, clip);
    return !IsRectEmpty(&bounds);
}

bool PaintTextBackground(HDC hdc, LPCRECT rect, UINT options) {
    if (!(options & ETO_OPAQUE))
        return true;
    if (GetBkColor(hdc) != GetSysColor(COLOR_HIGHLIGHT)) {
        HBRUSH brush = CreateSolidBrush(GetBkColor(hdc));
        FillRect(hdc, rect, brush);
        DeleteObject(brush);
        return true;
    }
    BP_PAINTPARAMS params{sizeof(params)};
    HDC memDC = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(hdc, rect, BPBF_TOPDOWNDIB, &params, &memDC);
    if (!buffer)
        return false;
    FillRect(memDC, rect, GetSysColorBrush(COLOR_HIGHLIGHT));
    BufferedPaintMakeOpaque(buffer, rect);
    return SUCCEEDED(EndBufferedPaint(buffer, TRUE));
}

bool ComposeText(HDC hdc, HPAINTBUFFER buffer, RECT const& bounds) {
    RGBQUAD* pixels = nullptr;
    int stride = 0;
    if (FAILED(GetBufferedPaintBits(buffer, &pixels, &stride))) {
        EndBufferedPaint(buffer, FALSE);
        return false;
    }
    COLORREF color = GetTextColor(hdc);
    BYTE red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
    for (int y = 0; y < bounds.bottom - bounds.top; y++) {
        RGBQUAD* row = pixels + y * stride;
        for (int x = 0; x < bounds.right - bounds.left; x++) {
            RGBQUAD& px = row[x];
            if ((px.rgbBlue | px.rgbGreen | px.rgbRed) == 0)
                continue;
            BYTE alpha = g_textGamma[(px.rgbBlue + (px.rgbGreen << 1) + px.rgbRed) >> 2];
            px.rgbBlue = (BYTE)((blue * alpha) >> 8);
            px.rgbGreen = (BYTE)((green * alpha) >> 8);
            px.rgbRed = (BYTE)((red * alpha) >> 8);
            px.rgbReserved = alpha;
        }
    }
    return true;
}

HRESULT WINAPI DrawTextWithGlow_Hook(HDC hdc, LPWSTR text, UINT count, RECT* rect, DWORD flags,
                                     COLORREF textColor, COLORREF glowColor, UINT glowRadius,
                                     UINT glowIntensity, BOOL premultiply,
                                     DTT_CALLBACK_PROC callback, LPARAM param) {
    bool empty = rect->right == rect->left || rect->bottom == rect->top;
    if (glowRadius > 0 || (empty && !(flags & DT_CALCRECT)))
        return DrawTextWithGlow_Original(hdc, text, count, rect, flags, textColor, glowColor,
                                         glowRadius, glowIntensity, premultiply, callback, param);
    SetTextColor(hdc, textColor);
    SetBkColor(hdc, RGB(0, 0, 0));
    if (callback)
        return callback(hdc, text, count, rect, flags, param);
    return DrawTextW(hdc, text, count, rect, flags & ~DT_MODIFYSTRING) ? S_OK : E_FAIL;
}

HRESULT WINAPI DrawThemeBackground_Hook(HTHEME theme, HDC hdc, int part, int state,
                                        LPCRECT rect, LPCRECT clip) {
    if (!g_unloading && rect && IsClearPart(ThemeClass(theme), part, state)) {
        RECT area = *rect;
        if (clip)
            IntersectRect(&area, rect, clip);
        if (HWND hwnd = WindowFromDC(hdc); hwnd && IsSurface(hwnd))
            ApplySurface(hwnd);
        FillRect(hdc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return S_OK;
    }
    return DrawThemeBackground_Original(theme, hdc, part, state, rect, clip);
}

HRESULT WINAPI DrawThemeBackgroundEx_Hook(HTHEME theme, HDC hdc, int part, int state,
                                          LPCRECT rect, DTBGOPTS const* options) {
    if (!g_unloading && rect && IsClearPart(ThemeClass(theme), part, state)) {
        RECT area = *rect;
        if (options && (options->dwFlags & DTBG_CLIPRECT))
            IntersectRect(&area, rect, &options->rcClip);
        FillRect(hdc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return S_OK;
    }
    return DrawThemeBackgroundEx_Original(theme, hdc, part, state, rect, options);
}

HRESULT WINAPI DrawThemeTextEx_Hook(HTHEME theme, HDC hdc, int part, int state, LPCWSTR text,
                                    int count, DWORD flags, LPRECT rect, DTTOPTS const* options) {
    if (g_unloading || (options && (options->dwFlags & DTT_CALCRECT)))
        return DrawThemeTextEx_Original(theme, hdc, part, state, text, count, flags, rect,
                                        options);
    DTTOPTS themed = options ? *options : DTTOPTS{sizeof(DTTOPTS)};
    if (SUCCEEDED(GetThemeColor(theme, part, state, TMT_TEXTCOLOR, &themed.crText)))
        themed.dwFlags |= DTT_TEXTCOLOR;
    return DrawThemeTextEx_Original(theme, hdc, part, state, text, count, flags, rect, &themed);
}

HRESULT WINAPI DrawThemeText_Hook(HTHEME theme, HDC hdc, int part, int state, LPCWSTR text,
                                  int count, DWORD flags, DWORD, LPCRECT rect) {
    RECT area = *rect;
    return DrawThemeTextEx_Hook(theme, hdc, part, state, text, count, flags, &area, nullptr);
}

HRESULT WINAPI DwmExtendFrameIntoClientArea_Hook(HWND hwnd, MARGINS const* margins) {
    if (g_unloading || !margins || !IsApplied(hwnd) || IsSurface(hwnd))
        return DwmExtendFrameIntoClientArea_Original(hwnd, margins);
    {
        std::lock_guard<std::mutex> lock(g_savedMutex);
        g_saved[hwnd].margins = *margins;
    }
    return ExtendFrame(hwnd, {-1, -1, -1, -1});
}

HRESULT WINAPI DwmSetWindowAttribute_Hook(HWND hwnd, DWORD attribute, LPCVOID value,
                                          DWORD size) {
    if (g_unloading || !IsApplied(hwnd) ||
        (attribute != DWMWA_SYSTEMBACKDROP_TYPE && attribute != DWMWA_USE_HOSTBACKDROPBRUSH))
        return DwmSetWindowAttribute_Original(hwnd, attribute, value, size);
    if (attribute == DWMWA_SYSTEMBACKDROP_TYPE && value && size >= sizeof(int)) {
        std::lock_guard<std::mutex> lock(g_savedMutex);
        g_saved[hwnd].backdrop = *(int const*)value;
    }
    return SetAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, kBackdropAcrylic);
}

BOOL WINAPI ExtTextOutW_Hook(HDC hdc, int x, int y, UINT options, RECT const* rect,
                             LPCWSTR text, UINT count, INT const* dx) {
    RECT bounds{};
    if (g_unloading || !ShouldComposeText(hdc, options, rect, text, count) ||
        !TextBounds(hdc, {x, y}, options, rect, text, count, dx, bounds) ||
        !PaintTextBackground(hdc, rect, options))
        return ExtTextOutW_Original(hdc, x, y, options, rect, text, count, dx);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BP_PAINTPARAMS params{sizeof(params), BPPF_ERASE | BPPF_NOCLIP, nullptr, &blend};
    HDC memDC = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &bounds, BPBF_TOPDOWNDIB, &params, &memDC);
    if (!buffer)
        return ExtTextOutW_Original(hdc, x, y, options, rect, text, count, dx);
    SelectObject(memDC, GetCurrentObject(hdc, OBJ_FONT));
    SetTextAlign(memDC, GetTextAlign(hdc));
    SetLayout(memDC, GetLayout(hdc));
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    BOOL result = ExtTextOutW_Original(memDC, x, y, options & ~ETO_OPAQUE, rect, text, count, dx);
    if (!ComposeText(hdc, buffer, bounds) || FAILED(EndBufferedPaint(buffer, TRUE)))
        return ExtTextOutW_Original(hdc, x, y, options, rect, text, count, dx);
    return result;
}

BOOL WINAPI FillRect_Hook(HDC hdc, RECT const* rect, HBRUSH brush) {
    if (g_unloading)
        return FillRect_Original(hdc, rect, brush);
    ULONG_PTR index = (ULONG_PTR)brush - 1;
    if (index <= COLOR_MENUBAR)
        brush = GetSysColorBrush((int)index);
    else if (IsNearBlack(brush) && IsGlassDC(hdc))
        brush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    return FillRect_Original(hdc, rect, brush);
}

DWORD WINAPI GetSysColor_Hook(int index) {
    return g_unloading ? GetSysColor_Original(index) : GlassSysColor(index);
}

HBRUSH WINAPI GetSysColorBrush_Hook(int index) {
    if (g_unloading || index < 0 || index > COLOR_MENUBAR)
        return GetSysColorBrush_Original(index);
    std::lock_guard<std::mutex> lock(g_brushMutex);
    HBRUSH& brush = g_brushes[index];
    if (!brush || GetObjectType(brush) != OBJ_BRUSH)
        brush = CreateSolidBrush(GlassSysColor(index));
    return brush;
}

HRESULT WINAPI GetThemeColor_Hook(HTHEME theme, int part, int state, int prop, COLORREF* color) {
    HRESULT hr = GetThemeColor_Original(theme, part, state, prop, color);
    if (g_unloading || !color)
        return hr;
    std::wstring name = ThemeClass(theme);
    if (name == L"Menu" || name == L"Tooltip") {
        bool text = prop == TMT_TEXTCOLOR &&
                    (name == L"Tooltip" ? part == TTP_STANDARD || part == TTP_BALLOON
                                        : (part == MENU_POPUPITEM || part == 27 ||
                                           (g_explorer && part == MENU_BARITEM)) &&
                                              state != MPI_DISABLED && state != MPI_DISABLEDHOT);
        if (text && g_dark)
            *color = RGB(255, 255, 255);
        else if (name == L"Menu" && (prop == TMT_FILLCOLOR || prop == TMT_FILLCOLORHINT))
            *color = RGB(0, 0, 0);
        else
            return hr;
        return S_OK;
    }
    if (g_explorer && ExplorerThemeColor(name, part, state, prop, hr, color))
        return S_OK;
    return hr;
}

HWND WINAPI NtUserCreateWindowEx_Hook(DWORD exStyle, PVOID className, LPCWSTR versionedClass,
                                      PVOID windowName, DWORD style, LONG x, LONG y, LONG w,
                                      LONG h, HWND parent, HMENU menu, HINSTANCE instance,
                                      LPVOID param, DWORD showMode, DWORD unknown1,
                                      DWORD unknown2, VOID* unknown3) {
    HWND hwnd = NtUserCreateWindowEx_Original(exStyle, className, versionedClass, windowName,
                                              style, x, y, w, h, parent, menu, instance, param,
                                              showMode, unknown1, unknown2, unknown3);
    if (hwnd && !g_unloading)
        Apply(hwnd);
    return hwnd;
}

void ApplyExisting() {
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() && !IsApplied(hwnd)) {
                Apply(hwnd);
                if (IsApplied(hwnd) && !IsSurface(hwnd))
                    Refresh(hwnd);
            }
            return TRUE;
        },
        0);
}

void Install(bool explorer) {
    g_dark = IsDarkMode();
    g_explorer = explorer;
    if (auto create = (NtUserCreateWindowEx_t)GetProcAddress(GetModuleHandle(L"win32u.dll"),
                                                             "NtUserCreateWindowEx"))
        WindhawkUtils::SetFunctionHook(create, NtUserCreateWindowEx_Hook,
                                       &NtUserCreateWindowEx_Original);
    WindhawkUtils::SetFunctionHook(DrawThemeBackground, DrawThemeBackground_Hook,
                                   &DrawThemeBackground_Original);
    WindhawkUtils::SetFunctionHook(GetThemeColor, GetThemeColor_Hook, &GetThemeColor_Original);
    if (!explorer)
        return;
    if (auto glow = (DrawTextWithGlow_t)GetProcAddress(GetModuleHandle(L"uxtheme.dll"),
                                                       MAKEINTRESOURCEA(126)))
        WindhawkUtils::SetFunctionHook(glow, DrawTextWithGlow_Hook, &DrawTextWithGlow_Original);
    WindhawkUtils::SetFunctionHook(DrawThemeBackgroundEx, DrawThemeBackgroundEx_Hook,
                                   &DrawThemeBackgroundEx_Original);
    WindhawkUtils::SetFunctionHook(DrawThemeText, DrawThemeText_Hook, &DrawThemeText_Original);
    WindhawkUtils::SetFunctionHook(DrawThemeTextEx, DrawThemeTextEx_Hook,
                                   &DrawThemeTextEx_Original);
    WindhawkUtils::SetFunctionHook(DwmExtendFrameIntoClientArea,
                                   DwmExtendFrameIntoClientArea_Hook,
                                   &DwmExtendFrameIntoClientArea_Original);
    WindhawkUtils::SetFunctionHook(DwmSetWindowAttribute, DwmSetWindowAttribute_Hook,
                                   &DwmSetWindowAttribute_Original);
    WindhawkUtils::SetFunctionHook(ExtTextOutW, ExtTextOutW_Hook, &ExtTextOutW_Original);
    WindhawkUtils::SetFunctionHook(FillRect, FillRect_Hook, &FillRect_Original);
    WindhawkUtils::SetFunctionHook(GetSysColor, GetSysColor_Hook, &GetSysColor_Original);
    WindhawkUtils::SetFunctionHook(GetSysColorBrush, GetSysColorBrush_Hook,
                                   &GetSysColorBrush_Original);
}

void RefreshTheme(LPARAM lParam) {
    if (!lParam || wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") != 0)
        return;
    g_dark = IsDarkMode();
    std::lock_guard<std::mutex> lock(g_brushMutex);
    g_brushes.fill(nullptr);
}

void Uninstall() {
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId())
                Restore(hwnd);
            return TRUE;
        },
        0);
    std::lock_guard<std::mutex> lock(g_savedMutex);
    g_saved.clear();
}
}

namespace TopBar {
enum Action { kNotifications, kQuickSettings };

struct Item {
    RECT rc;
    Action action;
};

HANDLE g_thread = nullptr;
std::atomic<HWND> g_wnd{nullptr};
std::vector<Item> g_items;
bool g_appBar = false;
bool g_fullscreen = false;
constexpr UINT kAppBarMsg = WM_APP + 42;
constexpr WCHAR kClassName[] = L"MinimalistTopBar";
constexpr UINT_PTR kClockTimerId = 3;
constexpr UINT_PTR kMotionTimerId = 2;
constexpr GUID kPowerSavingGuid = {
    0xE00958C0, 0xC213, 0x4ACE, {0xAC, 0x77, 0xFE, 0xCC, 0xED, 0x2E, 0xEE, 0xA5}};
constexpr double kPulseHoldMs = 1000.0;
constexpr double kPulseInMs = 320.0;
constexpr double kPulseOutMs = 450.0;
constexpr double kPulseScale = 1.25;
constexpr UINT_PTR kPulseTimerId = 4;
constexpr WCHAR kRadioKey[] = L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement";
constexpr UINT kRadioMsg = WM_APP + 43;
constexpr BYTE kVignettePeak = 64;
enum Edge { kEdgeBottom, kEdgeTop };

struct Vignette {
    HBITMAP bitmap = nullptr;
    HDC dc = nullptr;
    Edge edge = kEdgeBottom;
    HGDIOBJ old = nullptr;
    RECT rect{};
    HWND wnd = nullptr;
};

Vignette g_vignetteBottom;
Vignette g_vignetteTop;

namespace Bluetooth = winrt::Windows::Devices::Bluetooth;
namespace Connectivity = winrt::Windows::Networking::Connectivity;
namespace Enumeration = winrt::Windows::Devices::Enumeration;
namespace Radios = winrt::Windows::Devices::Radios;

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

void ApplyChrome(HWND hwnd) {
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
    SetWindowPos(hwnd, g_fullscreen ? HWND_BOTTOM : HWND_TOPMOST, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
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
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRadioKey, L"SystemRadioState", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

struct RadioSnapshot {
    bool known = false;
    bool anyOn = false;
    bool bluetoothOn = false;
};

struct RadioIcons {
    bool airplane = false;
    bool bluetooth = false;
    bool bluetoothConnected = false;
    bool hasWifi = false;
    bool wired = false;
    int wifiLevel = 0;

    bool operator==(RadioIcons const&) const = default;
};

std::atomic<int> g_bluetoothWatchPending{0};
HANDLE g_radioChangeEvent = nullptr;
RadioIcons g_radioIcons;
std::mutex g_radioMutex;
HANDLE g_radioStopEvent = nullptr;
HANDLE g_radioThread = nullptr;

std::vector<winrt::hstring> BluetoothSelectors() {
    auto connected = Bluetooth::BluetoothConnectionStatus::Connected;
    return {Bluetooth::BluetoothDevice::GetDeviceSelectorFromConnectionStatus(connected),
            Bluetooth::BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(connected)};
}

bool AnyBluetoothConnected(std::vector<winrt::hstring> const& selectors) {
    for (auto const& selector : selectors) {
        auto op = Enumeration::DeviceInformation::FindAllAsync(selector);
        if (op.wait_for(std::chrono::seconds(2)) !=
            winrt::Windows::Foundation::AsyncStatus::Completed) {
            op.Cancel();
            continue;
        }
        if (op.GetResults().Size() > 0)
            return true;
    }
    return false;
}

std::vector<Enumeration::DeviceWatcher> StartBluetoothWatchers(
    std::vector<winrt::hstring> const& selectors) {
    g_bluetoothWatchPending = (int)selectors.size();
    std::vector<Enumeration::DeviceWatcher> watchers;
    for (auto const& selector : selectors) {
        auto watcher = Enumeration::DeviceInformation::CreateWatcher(selector);
        auto notify = [](auto&&, auto&&) {
            if (g_bluetoothWatchPending <= 0)
                SetEvent(g_radioChangeEvent);
        };
        watcher.Added(notify);
        watcher.Removed(notify);
        watcher.EnumerationCompleted([](auto&&, auto&&) { g_bluetoothWatchPending--; });
        watcher.Start();
        watchers.push_back(watcher);
    }
    return watchers;
}

bool WiredConnected() {
    for (auto const& profile : Connectivity::NetworkInformation::GetConnectionProfiles()) {
        auto adapter = profile.NetworkAdapter();
        if (adapter && adapter.IanaInterfaceType() == 6 &&
            profile.GetNetworkConnectivityLevel() != Connectivity::NetworkConnectivityLevel::None)
            return true;
    }
    return false;
}

int WifiLevel(HANDLE wlan, bool& present) {
    present = false;
    if (!wlan)
        return 0;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(wlan, nullptr, &list) != ERROR_SUCCESS)
        return 0;
    present = list->dwNumberOfItems > 0;
    int level = 0;
    for (DWORD i = 0; i < list->dwNumberOfItems && !level; i++) {
        PWLAN_CONNECTION_ATTRIBUTES connection = nullptr;
        DWORD size = 0;
        if (WlanQueryInterface(wlan, &list->InterfaceInfo[i].InterfaceGuid,
                               wlan_intf_opcode_current_connection, nullptr, &size,
                               (PVOID*)&connection, nullptr) != ERROR_SUCCESS)
            continue;
        ULONG quality = connection->wlanAssociationAttributes.wlanSignalQuality;
        level = 1 + (quality >= 34) + (quality >= 67);
        WlanFreeMemory(connection);
    }
    WlanFreeMemory(list);
    return level;
}

HANDLE OpenWlan() {
    HANDLE wlan = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &wlan) != ERROR_SUCCESS)
        return nullptr;
    WlanRegisterNotification(
        wlan, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, FALSE,
        [](PWLAN_NOTIFICATION_DATA, PVOID) { SetEvent(g_radioChangeEvent); }, nullptr, nullptr,
        nullptr);
    return wlan;
}

void CloseWlan(HANDLE wlan) {
    if (!wlan)
        return;
    WlanRegisterNotification(wlan, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr,
                             nullptr, nullptr);
    WlanCloseHandle(wlan, nullptr);
}

bool WaitForRadioChange(HANDLE (&waits)[2]) {
    DWORD result = WaitForMultipleObjects(2, waits, FALSE, 60000);
    if (result == WAIT_OBJECT_0)
        return false;
    return result != WAIT_OBJECT_0 + 1 || WaitForSingleObject(g_radioStopEvent, 200) != WAIT_OBJECT_0;
}

DWORD WINAPI RadioThreadProc(void*) {
    bool apartment = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
    } catch (...) {}

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRadioKey, 0, KEY_NOTIFY, &key) != ERROR_SUCCESS)
        key = nullptr;

    std::vector<std::pair<Radios::Radio, winrt::event_token>> subscriptions;
    auto unsubscribe = [&] {
        for (auto& [radio, token] : subscriptions) {
            try {
                radio.StateChanged(token);
            } catch (...) {}
        }
        subscriptions.clear();
    };

    std::vector<winrt::hstring> selectors;
    std::vector<Enumeration::DeviceWatcher> watchers;
    try {
        selectors = BluetoothSelectors();
        watchers = StartBluetoothWatchers(selectors);
    } catch (...) {}

    HANDLE wlan = OpenWlan();

    winrt::event_token networkToken{};
    bool networkWatched = false;
    try {
        networkToken = Connectivity::NetworkInformation::NetworkStatusChanged(
            [](IInspectable const&) { SetEvent(g_radioChangeEvent); });
        networkWatched = true;
    } catch (...) {}

    HANDLE waits[] = {g_radioStopEvent, g_radioChangeEvent};
    do {
        if (key)
            RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, g_radioChangeEvent,
                                    TRUE);
        unsubscribe();

        RadioSnapshot snap;
        try {
            auto op = Radios::Radio::GetRadiosAsync();
            if (op.wait_for(std::chrono::seconds(2)) ==
                winrt::Windows::Foundation::AsyncStatus::Completed) {
                for (auto const& radio : op.GetResults()) {
                    snap.known = true;
                    if (radio.State() == Radios::RadioState::On) {
                        snap.anyOn = true;
                        if (radio.Kind() == Radios::RadioKind::Bluetooth)
                            snap.bluetoothOn = true;
                    }
                    subscriptions.emplace_back(
                        radio, radio.StateChanged([](Radios::Radio const&, IInspectable const&) {
                            SetEvent(g_radioChangeEvent);
                        }));
                }
            } else {
                op.Cancel();
            }
        } catch (...) {}

        RadioIcons icons;
        icons.airplane = IsAirplaneMode() || (snap.known && !snap.anyOn);
        icons.bluetooth = !icons.airplane && snap.bluetoothOn;
        if (icons.bluetooth) {
            try {
                icons.bluetoothConnected = AnyBluetoothConnected(selectors);
            } catch (...) {}
        }
        if (!icons.airplane)
            icons.wifiLevel = WifiLevel(wlan, icons.hasWifi);
        try {
            icons.wired = WiredConnected();
        } catch (...) {}
        bool changed;
        {
            std::lock_guard<std::mutex> lock(g_radioMutex);
            changed = g_radioIcons != icons;
            g_radioIcons = icons;
        }
        if (changed) {
            if (HWND bar = g_wnd.load())
                PostMessage(bar, kRadioMsg, 0, 0);
        }

    } while (WaitForRadioChange(waits));

    for (auto const& watcher : watchers) {
        try {
            watcher.Stop();
        } catch (...) {}
    }
    watchers.clear();
    if (networkWatched) {
        try {
            Connectivity::NetworkInformation::NetworkStatusChanged(networkToken);
        } catch (...) {}
    }
    CloseWlan(wlan);
    unsubscribe();
    if (key)
        RegCloseKey(key);
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
    g_radioStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_radioChangeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_radioThread = CreateThread(nullptr, 0, RadioThreadProc, nullptr, 0, nullptr);
}

void StopRadioWorker() {
    if (!g_radioThread)
        return;
    SetEvent(g_radioStopEvent);
    WaitForSingleObject(g_radioThread, 4000);
    CloseHandle(g_radioThread);
    CloseHandle(g_radioStopEvent);
    CloseHandle(g_radioChangeEvent);
    g_radioThread = nullptr;
    g_radioStopEvent = nullptr;
    g_radioChangeEvent = nullptr;
}

RadioIcons ReadRadioIcons() {
    std::lock_guard<std::mutex> lock(g_radioMutex);
    return g_radioIcons;
}

std::atomic<bool> g_energySaver{false};
std::atomic<bool> g_showPercentage{false};

bool HasBattery(SYSTEM_POWER_STATUS const& ps) {
    return ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100;
}

bool SaverActive(SYSTEM_POWER_STATUS const& ps) {
    return g_energySaver || (HasBattery(ps) && (ps.SystemStatusFlag & 1));
}

struct PaintKey {
    std::wstring date;
    std::wstring time;
    int batteryPercent = -1;
    int acLine = -1;
    RadioIcons radios;
    bool saver = false;
    bool percentage = false;
};

PaintKey CurrentPaintKey() {
    PaintKey key;
    key.percentage = g_showPercentage;
    key.date = DateText();
    key.time = TimeText();
    key.radios = ReadRadioIcons();
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        key.batteryPercent = ps.BatteryLifePercent;
        key.acLine = ps.ACLineStatus;
    }
    key.saver = SaverActive(ps);
    return key;
}

bool SamePaintKey(PaintKey const& a, PaintKey const& b) {
    return a.date == b.date && a.time == b.time && a.batteryPercent == b.batteryPercent &&
           a.acLine == b.acLine && a.radios == b.radios && a.saver == b.saver &&
           a.percentage == b.percentage;
}

PaintKey g_paintKey;
bool g_paintKeyValid = false;

void Paint(HWND hwnd);
void PresentLayered(HWND hwnd, bool alphaOnly = false);

std::unique_ptr<Gdiplus::Font> g_clockFont;
float g_clockFontSize = 0.0f;

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
        e = t < 0.5 ? 1.0 - 4.0 * t * t * t : std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
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

    g_surfaceDC = CreateCompatibleDC(g_screenDC);
    if (!g_surfaceDC)
        return false;
    g_surfaceBmp = CreateDib(g_surfaceDC, w, h, &g_surfaceBits);
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

void ReleaseVignette(Vignette& v) {
    if (v.dc) {
        if (v.old)
            SelectObject(v.dc, v.old);
        DeleteDC(v.dc);
    }
    if (v.bitmap)
        DeleteObject(v.bitmap);
    v.dc = nullptr;
    v.bitmap = nullptr;
    v.old = nullptr;
    v.rect = {};
}

bool TopVignetteSuppressed(HWND) {
    return g_fullscreen;
}

bool BottomVignetteSuppressed(HWND tray) {
    if (g_fullscreen || !tray || !IsWindowVisible(tray))
        return true;
    APPBARDATA abd{sizeof(abd)};
    if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE)
        return true;
    QUERY_USER_NOTIFICATION_STATE state;
    return SUCCEEDED(SHQueryUserNotificationState(&state)) &&
           (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE);
}

Edge VignetteEdge(RECT const& r) {
    RECT mon = PrimaryMonitorRect();
    return r.top + r.bottom < mon.top + mon.bottom ? kEdgeTop : kEdgeBottom;
}

void FillVignette(DWORD* bits, int w, int h, Edge edge) {
    for (int y = 0; y < h; y++) {
        int i = edge == kEdgeTop ? h - 1 - y : y;
        std::fill_n(bits + (size_t)y * w, w, (DWORD)(kVignettePeak * (i + 1) / h) << 24);
    }
}

void PresentVignette(Vignette& v, HWND anchor, bool (*suppressed)(HWND), bool alphaOnly) {
    if (!v.wnd || !g_screenDC)
        return;
    BYTE alpha = (BYTE)std::lround(std::clamp(g_winAlpha, 0.0, 1.0) * 255.0);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    POINT src{0, 0};

    auto update = [&](RECT const& r) {
        POINT dst{r.left, r.top};
        SIZE size{r.right - r.left, r.bottom - r.top};
        UpdateLayeredWindow(v.wnd, g_screenDC, &dst, &size, v.dc, &src, 0, &blend, ULW_ALPHA);
    };

    if (alphaOnly && v.bitmap && IsWindowVisible(v.wnd)) {
        update(v.rect);
        return;
    }

    RECT r{};
    if (!anchor || suppressed(anchor) || !GetWindowRect(anchor, &r) || r.right <= r.left ||
        r.bottom <= r.top) {
        ShowWindow(v.wnd, SW_HIDE);
        return;
    }

    Edge edge = VignetteEdge(r);
    if (!v.bitmap || !EqualRect(&r, &v.rect) || edge != v.edge) {
        ReleaseVignette(v);
        void* bits = nullptr;
        v.dc = CreateCompatibleDC(g_screenDC);
        if (v.dc)
            v.bitmap = CreateDib(v.dc, r.right - r.left, r.bottom - r.top, &bits);
        if (!v.bitmap || !bits) {
            ReleaseVignette(v);
            ShowWindow(v.wnd, SW_HIDE);
            return;
        }
        v.old = SelectObject(v.dc, v.bitmap);
        FillVignette((DWORD*)bits, r.right - r.left, r.bottom - r.top, edge);
        v.rect = r;
        v.edge = edge;
    }

    SetWindowPos(v.wnd, anchor, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    update(r);
}

void PresentVignettes(bool alphaOnly = false) {
    PresentVignette(g_vignetteBottom, alphaOnly ? nullptr : FindWindow(L"Shell_TrayWnd", nullptr),
                    BottomVignetteSuppressed, alphaOnly);
    PresentVignette(g_vignetteTop, g_wnd.load(), TopVignetteSuppressed, alphaOnly);
}

void PresentLayered(HWND hwnd, bool alphaOnly) {
    PresentVignettes(alphaOnly);
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

struct BarState {
    bool airplane = false;
    Gdiplus::Color batteryColor = Gdiplus::Color(255, 255, 255, 255);
    bool bluetooth = false;
    bool bluetoothConnected = false;
    bool charging = false;
    std::wstring dateText;
    int level = 100;
    std::wstring percentText;
    bool showPercentage = false;
    bool showWired = false;
    std::wstring timeText;
    int wifiLevel = 0;
    bool wired = false;
};

struct BarLayout {
    float airplaneL = 0, airplaneW = 0;
    float batteryL = 0, batteryW = 0;
    float bluetoothL = 0, bluetoothW = 0;
    float clockL = 0;
    float cy = 0;
    float dateL = 0, dateW = 0;
    float networkL = 0, networkW = 0;
    float percentL = 0, percentW = 0;
    float timeL = 0, timeW = 0;
    float u = 1.0f;
};

struct TextStyle {
    Gdiplus::Font& font;
    Gdiplus::StringFormat& fmt;
    Gdiplus::Graphics& g;
    float h;

    float Measure(std::wstring const& t) const {
        Gdiplus::RectF box;
        g.MeasureString(t.c_str(), -1, &font, Gdiplus::PointF(0, 0), &fmt, &box);
        return box.Width;
    }

    void Draw(std::wstring const& t, float l, float w, float dy,
              Gdiplus::Brush const& brush) const {
        g.DrawString(t.c_str(), -1, &font, Gdiplus::RectF(l, dy, w + 2, h), &fmt, &brush);
    }
};

struct DrawContext {
    Gdiplus::Color color;
    float dy;
    Gdiplus::Color faint;
    Gdiplus::Graphics& g;
    BarLayout const& layout;
    BarState const& state;
    TextStyle const& text;
};

void RoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) / 2);
    path.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    path.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    path.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    path.CloseFigure();
}

void DrawAirplane(DrawContext const& c) {
    if (!c.state.airplane)
        return;
    BarLayout const& l = c.layout;
    static const float k[][2] = {
        {0, -7},     {1.3f, -4},  {1.3f, -1.5f}, {7, 2.5f},  {7, 4},         {1.3f, 2},
        {1.3f, 5},   {3, 6.5f},   {3, 7.5f},     {0, 6.8f},  {-3, 7.5f},    {-3, 6.5f},
        {-1.3f, 5},  {-1.3f, 2},  {-7, 4},       {-7, 2.5f}, {-1.3f, -1.5f}, {-1.3f, -4}};
    float ax = l.airplaneL + l.airplaneW / 2, ay = l.cy + c.dy;
    Gdiplus::PointF pts[18];
    for (int i = 0; i < 18; i++)
        pts[i] = Gdiplus::PointF(ax + k[i][0] * 0.86f * l.u, ay + k[i][1] * 0.86f * l.u);
    Gdiplus::SolidBrush brush(c.color);
    c.g.FillPolygon(&brush, pts, 18);
}

void DrawBattery(DrawContext const& c) {
    using namespace Gdiplus;
    Graphics& g = c.g;
    BarLayout const& l = c.layout;
    BarState const& s = c.state;
    float u = l.u, dy = c.dy;
    SolidBrush faintBrush(c.faint);

    if (g_batteryScale > 1.0001) {
        float zx = l.batteryL + (l.batteryW - 2.0f * u) / 2, zy = l.cy + dy;
        g.TranslateTransform(zx, zy);
        g.ScaleTransform((float)g_batteryScale, (float)g_batteryScale);
        g.TranslateTransform(-zx, -zy);
    }
    float bodyW = l.batteryW - 2.0f * u, bodyH = 11.0f * u;
    float x = l.batteryL, y = l.cy - bodyH / 2 + dy;
    GraphicsPath body;
    RoundRect(body, x, y, bodyW, bodyH, 3.5f * u);
    g.FillPath(&faintBrush, &body);
    float inset = 1.5f * u;
    float fillW = (bodyW - 2 * inset) * s.level / 100.0f;
    if (fillW > 0) {
        GraphicsPath fill;
        RoundRect(fill, x + inset, y + inset, fillW, bodyH - 2 * inset, 2.2f * u);
        SolidBrush levelBrush(c.color.GetA() == 255 ? s.batteryColor : c.color);
        g.FillPath(&levelBrush, &fill);
    }
    if (s.charging && c.color.GetA() == 255) {
        float bx = x + bodyW / 2, by = y + bodyH / 2;
        static const float k[][2] = {{1.3f, -4.2f}, {-2.3f, 0.7f}, {-0.2f, 0.7f},
                                     {-1.3f, 4.2f}, {2.3f, -0.7f}, {0.2f, -0.7f}};
        PointF bolt[6];
        for (int i = 0; i < 6; i++)
            bolt[i] = PointF(bx + k[i][0] * 1.25f * u, by + k[i][1] * 1.25f * u);
        Pen boltEdge(Gdiplus::Color(140, 0, 0, 0), 0.8f * u);
        boltEdge.SetLineJoin(LineJoinRound);
        SolidBrush boltFill(Gdiplus::Color(255, 255, 255, 255));
        g.DrawPolygon(&boltEdge, bolt, 6);
        g.FillPolygon(&boltFill, bolt, 6);
    }
    GraphicsPath cap;
    RoundRect(cap, x + bodyW + 0.7f * u, l.cy - 2.2f * u + dy, 1.3f * u, 4.4f * u, 0.65f * u);
    g.FillPath(&faintBrush, &cap);
    g.ResetTransform();

    if (s.showPercentage) {
        SolidBrush brush(c.color);
        c.text.Draw(s.percentText, l.percentL, l.percentW, dy, brush);
    }
}

void DrawBluetooth(DrawContext const& c) {
    using namespace Gdiplus;
    if (!c.state.bluetooth)
        return;
    BarLayout const& l = c.layout;
    float u = l.u;
    Pen pen(c.state.bluetoothConnected ? c.color : c.faint, 1.4f * u);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    float bx = l.bluetoothL + l.bluetoothW / 2, by = l.cy + c.dy;
    PointF pts[] = {{bx - 3.4f * u, by - 3.1f * u}, {bx + 3.3f * u, by + 3.1f * u},
                    {bx, by + 6.0f * u},            {bx, by - 6.0f * u},
                    {bx + 3.3f * u, by - 3.1f * u}, {bx - 3.4f * u, by + 3.1f * u}};
    c.g.DrawLines(&pen, pts, 6);
}

void DrawClock(DrawContext const& c) {
    Gdiplus::SolidBrush brush(c.color);
    BarLayout const& l = c.layout;
    c.text.Draw(c.state.dateText, l.dateL, l.dateW, c.dy, brush);
    c.text.Draw(c.state.timeText, l.timeL, l.timeW, c.dy, brush);
}

void DrawNetwork(DrawContext const& c) {
    using namespace Gdiplus;
    BarState const& s = c.state;
    if (s.airplane)
        return;
    Graphics& g = c.g;
    BarLayout const& l = c.layout;
    float u = l.u;

    if (s.showWired) {
        Pen pen(s.wired ? c.color : c.faint, 1.5f * u);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        float ex = l.networkL, ey = l.cy - 5.5f * u + c.dy;
        PointF plug[] = {{ex, ey},
                         {ex + 15.0f * u, ey},
                         {ex + 15.0f * u, ey + 8.0f * u},
                         {ex + 11.0f * u, ey + 8.0f * u},
                         {ex + 11.0f * u, ey + 11.0f * u},
                         {ex + 4.0f * u, ey + 11.0f * u},
                         {ex + 4.0f * u, ey + 8.0f * u},
                         {ex, ey + 8.0f * u}};
        g.DrawPolygon(&pen, plug, 8);
        for (int i = 0; i < 3; i++) {
            float px = ex + (4.5f + 3.0f * i) * u;
            g.DrawLine(&pen, px, ey + 2.2f * u, px, ey + 5.0f * u);
        }
        return;
    }

    Pen pen(c.color, 2.1f * u);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    Pen dimPen(c.faint, 2.1f * u);
    dimPen.SetStartCap(LineCapRound);
    dimPen.SetEndCap(LineCapRound);
    SolidBrush brush(c.color);
    SolidBrush faintBrush(c.faint);
    float ox = l.networkL + l.networkW / 2, oy = l.cy + 6.0f * u + c.dy;
    g.DrawArc(s.wifiLevel >= 3 ? &pen : &dimPen, ox - 10.0f * u, oy - 10.0f * u, 20.0f * u,
              20.0f * u, 225.0f, 90.0f);
    g.DrawArc(s.wifiLevel >= 2 ? &pen : &dimPen, ox - 6.4f * u, oy - 6.4f * u, 12.8f * u,
              12.8f * u, 225.0f, 90.0f);
    float r0 = 3.4f * u;
    g.FillPie(s.wifiLevel >= 1 ? &brush : &faintBrush, ox - r0, oy - r0, 2 * r0, 2 * r0, 225.0f,
              90.0f);
}

BarLayout LayoutBar(BarState const& s, TextStyle const& text, float u, int w, int h,
                    std::vector<Item>& items) {
    BarLayout l;
    l.u = u;
    l.cy = h / 2.0f;
    l.dateW = text.Measure(s.dateText);
    l.timeW = text.Measure(s.timeText);
    l.percentW = s.showPercentage ? text.Measure(s.percentText) : 0.0f;

    float right = w - EdgePadding(u);
    l.timeL = right - l.timeW;
    l.dateL = l.timeL - 11.5f * u - l.dateW;
    l.clockL = l.dateL;

    float cursor = l.clockL - 23.0f * u;
    l.airplaneW = 12.0f * u;
    if (s.airplane) {
        l.airplaneL = cursor - l.airplaneW;
        cursor = l.airplaneL - 22.0f * u;
    }
    l.bluetoothW = 8.0f * u;
    if (s.bluetooth) {
        l.bluetoothL = cursor - l.bluetoothW;
        cursor = l.bluetoothL - 22.0f * u;
    }
    l.networkW = 15.0f * u;
    l.networkL = cursor - l.networkW;
    l.batteryW = 23.0f * u;
    l.batteryL = (s.airplane ? cursor : l.networkL - 22.0f * u) - l.batteryW;
    l.percentL = l.batteryL - 5.0f * u - l.percentW;

    auto add = [&](float left, float rightEdge, float padL, float padR, Action action) {
        items.push_back({{(LONG)(left - padL), 0, (LONG)(rightEdge + padR), h}, action});
    };
    if (s.airplane)
        add(l.airplaneL, l.airplaneL + l.airplaneW, 8 * u, 11 * u, kQuickSettings);
    if (s.bluetooth)
        add(l.bluetoothL, l.bluetoothL + l.bluetoothW, 11 * u, 11 * u, kQuickSettings);
    add(s.showPercentage ? l.percentL : l.batteryL, l.batteryL + l.batteryW, 8 * u, 11 * u,
        kQuickSettings);
    if (!s.airplane)
        add(l.networkL, l.networkL + l.networkW, 11 * u, 10.5f * u, kQuickSettings);
    add(l.clockL, right, 11.5f * u, 8 * u, kNotifications);
    return l;
}

BarState ReadBarState() {
    BarState s;
    RadioIcons radios = ReadRadioIcons();
    s.airplane = radios.airplane;
    s.bluetooth = radios.bluetooth;
    s.bluetoothConnected = radios.bluetoothConnected;
    s.dateText = DateText();
    s.showWired = radios.wired || !radios.hasWifi;
    s.timeText = TimeText();
    s.wifiLevel = radios.wifiLevel;
    s.wired = radios.wired;

    SYSTEM_POWER_STATUS ps{};
    bool hasBattery = GetSystemPowerStatus(&ps) && HasBattery(ps);
    s.level = hasBattery ? ps.BatteryLifePercent : 100;
    s.charging = hasBattery && ps.ACLineStatus == 1;
    s.showPercentage = hasBattery && g_showPercentage;
    s.percentText = std::to_wstring(s.level) + L"%";
    if (SaverActive(ps))
        s.batteryColor = Gdiplus::Color(255, 255, 214, 10);
    else if (s.level <= 20)
        s.batteryColor = Gdiplus::Color(255, 255, 69, 58);
    else if (s.charging)
        s.batteryColor = Gdiplus::Color(255, 52, 199, 89);
    return s;
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

    std::vector<Item> items;
    {
        using namespace Gdiplus;
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)g_surfaceBits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);

        if (!g_clockFont || g_clockFontSize != 14.0f * s) {
            g_clockFont = std::make_unique<Font>(L"Segoe UI Variable Text", 14.0f * s,
                                                 FontStyleRegular, UnitPixel);
            if (!g_clockFont->IsAvailable())
                g_clockFont =
                    std::make_unique<Font>(L"Segoe UI", 14.0f * s, FontStyleRegular, UnitPixel);
            g_clockFontSize = 14.0f * s;
        }

        BarState state = ReadBarState();
        TextStyle text{*g_clockFont, fmt, g, (float)h};
        BarLayout layout = LayoutBar(state, text, s, w, h, items);

        SolidBrush hitBrush(Gdiplus::Color(3, 0, 0, 0));
        for (auto const& item : items)
            g.FillRectangle(&hitBrush, RectF((REAL)item.rc.left, (REAL)item.rc.top,
                                             (REAL)(item.rc.right - item.rc.left),
                                             (REAL)(item.rc.bottom - item.rc.top)));

        auto drawAll = [&](Gdiplus::Color color, float dy) {
            Gdiplus::Color faint(BYTE(color.GetA() * 0.4), color.GetR(), color.GetG(),
                                 color.GetB());
            DrawContext c{color, dy, faint, g, layout, state, text};
            DrawBattery(c);
            DrawBluetooth(c);
            DrawAirplane(c);
            DrawNetwork(c);
            DrawClock(c);
        };
        drawAll(Gdiplus::Color(70, 0, 0, 0), 1.0f * s);
        drawAll(Gdiplus::Color(255, 255, 255, 255), 0);
    }

    g_items = std::move(items);

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
        PresentLayered(hwnd, true);

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
            return 0;
        }
        case kRadioMsg:
            RefreshBar(hwnd);
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE) {
                auto* setting = (POWERBROADCAST_SETTING*)lParam;
                if (setting && IsEqualGUID(setting->PowerSetting, kPowerSavingGuid) &&
                    setting->DataLength >= sizeof(DWORD))
                    g_energySaver = *(DWORD*)setting->Data != 0;
            }
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
            if (wParam == ABN_FULLSCREENAPP) {
                g_fullscreen = lParam != 0;
                SetWindowPos(hwnd, g_fullscreen ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
            } else if (wParam == ABN_POSCHANGED) {
                PositionBar(hwnd);
            }
            PresentVignettes();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            PositionBar(hwnd);
            g_paintKeyValid = false;
            Paint(hwnd);
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((LPCWSTR)lParam, L"intl") == 0) {
                g_paintKeyValid = false;
                Paint(hwnd);
            }
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
        for (Vignette* v : {&g_vignetteBottom, &g_vignetteTop})
            v->wnd = CreateWindowEx(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                L"STATIC", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        g_wnd = hwnd;
        HPOWERNOTIFY powerNotify =
            RegisterPowerSettingNotification(hwnd, &kPowerSavingGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = hwnd;
        abd.uCallbackMessage = kAppBarMsg;
        g_appBar = SHAppBarMessage(ABM_NEW, &abd) != 0;
        ApplyChrome(hwnd);
        PositionBar(hwnd);
        g_winAlpha = Motion::Enabled() ? 0.0 : 1.0;
        g_winAlphaTarget = 1.0;
        g_closing = false;
        g_motionRunning = false;
        Paint(hwnd);
        g_paintKey = CurrentPaintKey();
        g_paintKeyValid = true;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ArmClockTimer(hwnd);
        StartBarMotion();

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (powerNotify)
            UnregisterPowerSettingNotification(powerNotify);
    }

    for (Vignette* v : {&g_vignetteBottom, &g_vignetteTop}) {
        if (v->wnd)
            DestroyWindow(v->wnd);
        v->wnd = nullptr;
        ReleaseVignette(*v);
    }
    g_wnd = nullptr;
    g_items.clear();
    StopRadioWorker();
    g_clockFont.reset();
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

void ReloadSettings() {
    g_showPercentage = Wh_GetIntSetting(L"showBatteryPercentage") != 0;
    if (HWND hwnd = g_wnd.load())
        PostMessage(hwnd, kRadioMsg, 0, 0);
}

void Stop() {
    if (!g_thread)
        return;
    if (HWND hwnd = g_wnd.load())
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(g_thread, 5000) == WAIT_TIMEOUT)
        TerminateThread(g_thread, 0);
    CloseHandle(g_thread);
    g_thread = nullptr;
}
}

namespace Genie {
using Clock = std::chrono::steady_clock;

constexpr int kDurationMs = 125;
constexpr float kFadeSpan = 0.125f;
constexpr int kFirstFrameWaitMs = 200;
constexpr long long kMaxPixels = 64LL * 1024 * 1024;
constexpr int kMinSizePx = 40;
constexpr float kNeckMax = 60.0f;
constexpr float kNeckMin = 12.0f;
constexpr float kNeckRatio = 0.03f;
constexpr float kSpread = 0.65f;

struct Job {
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int dockX = 0;
    HANDLE firstFrame = nullptr;
    int h = 0;
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT rect{};
    bool rising = false;
    int w = 0;

    ~Job() {
        if (bitmap)
            DeleteObject(bitmap);
        if (firstFrame)
            CloseHandle(firstFrame);
    }
};

struct Plan {
    bool above = false;
    RECT bounds{};
    float dockX = 0.0f;
    float dockY = 0.0f;
    float neck = 0.0f;
};

struct Snapshot {
    HBITMAP bitmap;
    void* bits;
    int h;
    int w;
};

std::unordered_set<HWND> g_active;
std::unordered_map<HWND, int> g_iconX;
std::mutex g_mutex;
std::unordered_map<HWND, Snapshot> g_snapshots;
std::vector<HANDLE> g_workers;
std::mutex g_workersMutex;

class Canvas {
public:
    BYTE* bits = nullptr;
    int h = 0;
    int left = 0;
    int top = 0;
    int w = 0;

    ~Canvas() {
        if (m_dc) {
            if (m_old)
                SelectObject(m_dc, m_old);
            DeleteDC(m_dc);
        }
        if (m_bitmap)
            DeleteObject(m_bitmap);
        if (m_screen)
            ReleaseDC(nullptr, m_screen);
        if (m_ghost)
            DestroyWindow(m_ghost);
    }

    bool Create(RECT const& r) {
        left = r.left;
        top = r.top;
        w = std::max<int>(1, r.right - r.left);
        h = std::max<int>(1, r.bottom - r.top);
        m_ghost = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT, L"STATIC",
            nullptr, WS_POPUP, left, top, w, h, nullptr, nullptr, nullptr, nullptr);
        m_screen = GetDC(nullptr);
        if (m_screen) {
            m_bitmap = CreateDib(m_screen, w, h, (void**)&bits);
            m_dc = CreateCompatibleDC(m_screen);
        }
        if (m_bitmap && m_dc)
            m_old = SelectObject(m_dc, m_bitmap);
        return m_ghost && m_screen && m_bitmap && bits && m_dc && m_old;
    }

    void Present(BYTE alpha) {
        POINT dst{left, top};
        SIZE size{w, h};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(m_ghost, m_screen, &dst, &size, m_dc, &src, 0, &blend, ULW_ALPHA);
    }

    void Show() {
        ShowWindow(m_ghost, SW_SHOWNOACTIVATE);
    }

private:
    HDC m_dc = nullptr;
    HWND m_ghost = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_old = nullptr;
    HDC m_screen = nullptr;
};

class Pacer {
public:
    Pacer() {
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) &&
            info.rateRefresh.uiNumerator)
            m_intervalMs = 500.0 * info.rateRefresh.uiDenominator / info.rateRefresh.uiNumerator;
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!m_timer)
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    }

    ~Pacer() {
        if (m_timer)
            CloseHandle(m_timer);
    }

    void Wait(Clock::time_point frameStart) {
        if (!m_timer) {
            Sleep(1);
            return;
        }
        double spentMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        double waitMs = m_intervalMs - spentMs;
        if (waitMs <= 0.1) {
            SwitchToThread();
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
        SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(m_timer, 100);
    }

private:
    double m_intervalMs = 1000.0 / 240.0;
    HANDLE m_timer = nullptr;
};

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

void SetCloak(HWND hwnd, BOOL cloak) {
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

void SetTransitions(HWND hwnd, bool enable) {
    BOOL disable = !enable;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

void Revert(HWND hwnd, bool rising) {
    if (rising)
        SetCloak(hwnd, FALSE);
    SetTransitions(hwnd, true);
}

bool ShouldAnimate(HWND hwnd) {
    if (!hwnd || g_unloading || !Motion::Enabled())
        return false;
    if (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD)
        return false;
    RECT r;
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (!GetWindowPlacement(hwnd, &wp))
            return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(hwnd, &r)) {
        return false;
    }
    return r.right - r.left >= kMinSizePx && r.bottom - r.top >= kMinSizePx;
}

HWND FindTaskbar(HMONITOR monitor) {
    HWND primary = FindWindow(L"Shell_TrayWnd", nullptr);
    if (!monitor || MonitorFromWindow(primary, MONITOR_DEFAULTTOPRIMARY) == monitor)
        return primary;
    for (HWND w = nullptr; (w = FindWindowEx(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr));) {
        if (MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == monitor)
            return w;
    }
    return primary;
}

std::wstring ProcessName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!process)
        return {};
    WCHAR path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageName(process, 0, path, &size)) {
        std::wstring_view full(path, size);
        auto base = full.substr(full.find_last_of(L'\\') + 1);
        name = std::wstring(base.substr(0, base.find(L'.')));
    }
    CloseHandle(process);
    return Lower(name);
}

int ScoreButton(std::wstring const& name, std::wstring const& title, std::wstring const& process) {
    if (name.empty())
        return 0;
    auto has = [&](std::wstring const& s) {
        return !s.empty() && name.find(s) != std::wstring::npos;
    };
    int score = 0;
    if (title == name)
        score += 1000;
    if (!title.empty()) {
        if (title.find(name) != std::wstring::npos)
            score += 500;
        if (name.find(title) != std::wstring::npos)
            score += 500;
    }
    if (has(process))
        score += 400;
    if (process == L"chrome" && has(L"google chrome"))
        score += 900;
    if (process == L"msedge" && has(L"microsoft edge"))
        score += 900;

    std::wstring word;
    auto flush = [&] {
        if (has(word) && word.size() >= 4)
            score += 50;
        word.clear();
    };
    for (wchar_t c : title) {
        if (iswalnum(c))
            word += c;
        else
            flush();
    }
    flush();

    for (PCWSTR unwanted : {L"start", L"search", L"task view", L"widgets"}) {
        if (has(unwanted))
            score -= 500;
    }
    return score;
}

bool QueryIconX(HWND app, HMONITOR monitor, int& x) {
    winrt::com_ptr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.put()))) &&
        FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.put()))))
        return false;

    HWND tray = FindTaskbar(monitor);
    winrt::com_ptr<IUIAutomationElement> root;
    if (!tray || FAILED(automation->ElementFromHandle(tray, root.put())))
        return false;

    VARIANT type{};
    type.vt = VT_I4;
    winrt::com_ptr<IUIAutomationCondition> button, listItem, either;
    type.lVal = UIA_ButtonControlTypeId;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, button.put());
    type.lVal = UIA_ListItemControlTypeId;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, listItem.put());
    if (!button || !listItem ||
        FAILED(automation->CreateOrCondition(button.get(), listItem.get(), either.put())))
        return false;

    winrt::com_ptr<IUIAutomationElementArray> items;
    if (FAILED(root->FindAll(TreeScope_Descendants, either.get(), items.put())) || !items)
        return false;

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(monitor, &mi);
    WCHAR text[512]{};
    GetWindowText(app, text, ARRAYSIZE(text));
    std::wstring title = Lower(text);
    std::wstring process = ProcessName(app);

    int count = 0;
    items->get_Length(&count);
    int best = 0;
    for (int i = 0; i < count; i++) {
        winrt::com_ptr<IUIAutomationElement> item;
        BSTR name = nullptr;
        if (FAILED(items->GetElement(i, item.put())) || FAILED(item->get_CurrentName(&name)) ||
            !name)
            continue;
        int score = ScoreButton(Lower(name), title, process);
        SysFreeString(name);
        RECT r;
        if (score > best && SUCCEEDED(item->get_CurrentBoundingRectangle(&r)) &&
            r.right > r.left && r.left < mi.rcMonitor.right - 50) {
            best = score;
            x = (r.left + r.right) / 2;
        }
    }
    return best > 0;
}

int DockX(HWND hwnd, HMONITOR monitor, RECT const& mon) {
    int x = (mon.left + mon.right) / 2;
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO cursorMon{sizeof(cursorMon)};
    if (!GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &cursorMon))
        SystemParametersInfo(SPI_GETWORKAREA, 0, &cursorMon.rcWork, 0);

    bool found = false;
    if (!PtInRect(&cursorMon.rcWork, pt)) {
        x = pt.x;
        found = true;
    } else {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            found = QueryIconX(hwnd, monitor, x);
            if (SUCCEEDED(hr))
                CoUninitialize();
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (found)
        g_iconX[hwnd] = x;
    else if (auto it = g_iconX.find(hwnd); it != g_iconX.end())
        x = it->second;
    return x;
}

void CopySnapshot(DWORD const* src, int rawW, int rawH, int offsetX, int offsetY, Job& job) {
    DWORD* dst = (DWORD*)job.bits;
    int inBounds = 0, transparent = 0;
    for (int y = 0; y < job.h; y++) {
        for (int x = 0; x < job.w; x++) {
            int sx = x + offsetX, sy = y + offsetY;
            DWORD p = 0;
            if (sx >= 0 && sx < rawW && sy >= 0 && sy < rawH) {
                p = src[sy * rawW + sx];
                inBounds++;
                if ((p >> 24) == 0) {
                    transparent++;
                    p = 0;
                }
            }
            dst[y * job.w + x] = p;
        }
    }
    if (inBounds == 0 || transparent * 2 <= inBounds)
        return;
    for (int y = 0; y < job.h; y++) {
        for (int x = 0; x < job.w; x++) {
            int sx = x + offsetX, sy = y + offsetY;
            if (sx < 0 || sx >= rawW || sy < 0 || sy >= rawH)
                continue;
            DWORD p = src[sy * rawW + sx];
            if ((p >> 24) == 0)
                dst[y * job.w + x] = 0xFF000000u | (p & 0x00FFFFFFu);
        }
    }
}

bool Capture(HDC screen, RECT const& win, Job& job) {
    int rawW = win.right - win.left, rawH = win.bottom - win.top;
    HDC dc = CreateCompatibleDC(screen);
    if (!dc)
        return false;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDib(screen, rawW, rawH, &bits);
    bool ok = bitmap && bits;
    if (ok) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        PrintWindow(job.hwnd, dc, PW_RENDERFULLCONTENT);
        GdiFlush();
        CopySnapshot((DWORD const*)bits, rawW, rawH, job.rect.left - win.left,
                     job.rect.top - win.top, job);
        SelectObject(dc, old);
    }
    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}

bool TakeCached(Job& job) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_snapshots.find(job.hwnd);
    if (it == g_snapshots.end())
        return false;
    Snapshot snap = it->second;
    g_snapshots.erase(it);
    bool fits = snap.w == job.w && snap.h == job.h;
    if (fits)
        memcpy(job.bits, snap.bits, (size_t)job.w * job.h * 4);
    DeleteObject(snap.bitmap);
    return fits;
}

void Cache(HDC screen, Job const& job) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto it = g_snapshots.find(job.hwnd); it != g_snapshots.end()) {
        DeleteObject(it->second.bitmap);
        g_snapshots.erase(it);
    }
    if (g_unloading)
        return;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDib(screen, job.w, job.h, &bits);
    if (!bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        return;
    }
    memcpy(bits, job.bits, (size_t)job.w * job.h * 4);
    g_snapshots[job.hwnd] = {bitmap, bits, job.h, job.w};
}

float Morph(float v, float t) {
    float m = std::clamp(t * (1.0f + kSpread) - (1.0f - v) * kSpread, 0.0f, 1.0f);
    return m * m * (3.0f - 2.0f * m);
}

bool MakePlan(Job const& job, Plan& plan) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfo(job.monitor, &mi))
        return false;
    RECT const& mon = mi.rcMonitor;
    RECT const& win = job.rect;

    int dockX = std::clamp<int>(job.dockX, mon.left, mon.right);
    float dockY = (float)mon.bottom;
    RECT tray;
    if (GetWindowRect(FindTaskbar(job.monitor), &tray) && tray.bottom > tray.top &&
        tray.top + tray.bottom < mon.top + mon.bottom)
        dockY = (float)mon.top;

    plan.above = dockY < (float)(win.top + win.bottom) * 0.5f;
    dockY = plan.above ? std::min(dockY, (float)win.top) : std::max(dockY, (float)win.bottom);
    plan.dockX = (float)dockX;
    plan.dockY = dockY;
    plan.neck = std::clamp(job.w * kNeckRatio, kNeckMin, kNeckMax);

    RECT b;
    b.left = std::min<int>(win.left, dockX) - job.w / 2;
    b.right = std::max<int>(win.right, dockX) + job.w / 2;
    b.top = std::min<int>(win.top, (int)dockY);
    b.bottom = std::max<int>(win.bottom, (int)dockY);

    b.left = std::min<int>(std::max<int>(b.left, mon.left), std::min<int>(win.left, dockX - 16));
    b.right = std::max<int>(std::min<int>(b.right, mon.right), std::max<int>(win.right, dockX + 16));
    b.top = std::min<int>(std::max<int>(b.top, mon.top), win.top);
    b.bottom = std::max<int>(std::min<int>(b.bottom, mon.bottom), win.bottom);

    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN), vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN), vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vWidth > 0 && vHeight > 0) {
        b.left = std::max<int>(b.left, vLeft);
        b.top = std::max<int>(b.top, vTop);
        b.right = std::min<int>(b.right, vLeft + vWidth);
        b.bottom = std::min<int>(b.bottom, vTop + vHeight);
    }
    plan.bounds = b;
    return b.right > b.left && b.bottom > b.top &&
           (long long)(b.right - b.left) * (b.bottom - b.top) <= kMaxPixels;
}

void RenderFrame(Job const& job, Plan const& plan, Canvas& canvas, std::vector<float>& rows,
                 float t) {
    int const W = job.w, H = job.h;
    float const centerX = job.rect.left + W * 0.5f;
    memset(canvas.bits, 0, (size_t)canvas.w * canvas.h * 4);

    for (int k = 0; k <= H; k++) {
        float v = (float)k / H;
        float ideal = (float)job.rect.top + H * v;
        rows[k] = ideal + (plan.dockY - ideal) * Morph(plan.above ? 1.0f - v : v, t);
    }

    int seg = 0;
    for (int y = 0; y < canvas.h; y++) {
        float screenY = (float)(y + canvas.top) + 0.5f;
        if (screenY < rows[0] || screenY >= rows[H])
            continue;
        while (seg < H - 1 && rows[seg + 1] <= screenY)
            seg++;
        float segH = rows[seg + 1] - rows[seg];
        float v = ((float)seg + (segH > 1e-4f ? (screenY - rows[seg]) / segH : 0.0f)) / H;
        float morph = Morph(plan.above ? 1.0f - v : v, t);
        float width = std::max(1.0f, W + (plan.neck - W) * morph);
        float left = centerX + (plan.dockX - centerX) * morph - width * 0.5f - canvas.left;

        int srcRow = std::clamp((int)(v * H), 0, H - 1);
        DWORD const* src = (DWORD const*)job.bits + (size_t)srcRow * W;
        DWORD* dst = (DWORD*)(canvas.bits + (size_t)y * canvas.w * 4);
        int from = std::max(0, (int)left), to = std::min(canvas.w, (int)(left + width) + 1);
        for (int x = from; x < to; x++) {
            float u = ((float)x + 0.5f - left) / width;
            if (u >= 0.0f && u < 1.0f)
                dst[x] = src[std::min((int)(u * W), W - 1)];
        }
    }
}

void Play(Job const& job, Plan const& plan, Canvas& canvas) {
    Pacer pacer;
    std::vector<float> rows(job.h + 1);
    auto start = Clock::now();
    bool first = true;
    for (;;) {
        auto frameStart = Clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(frameStart - start).count();
        bool last = elapsedMs >= kDurationMs;
        float progress = last ? 1.0f : (float)(elapsedMs / kDurationMs);
        float t = job.rising ? 1.0f - progress : progress;
        float fade = std::clamp((1.0f - t) / kFadeSpan, 0.0f, 1.0f);

        RenderFrame(job, plan, canvas, rows, t);
        canvas.Present((BYTE)(255.0f * fade));
        if (first)
            canvas.Show();
        if (last || g_unloading)
            break;
        if (first) {
            DwmFlush();
            first = false;
            if (job.firstFrame)
                SetEvent(job.firstFrame);
        }
        pacer.Wait(frameStart);
    }
}

void Finish(Job const& job) {
    if (job.rising)
        SetCloak(job.hwnd, FALSE);
    SetTransitions(job.hwnd, true);
    if (job.firstFrame)
        SetEvent(job.firstFrame);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active.erase(job.hwnd);
}

DWORD WINAPI Run(void* param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Plan plan;
    Canvas canvas;
    if (MakePlan(*job, plan) && canvas.Create(plan.bounds))
        Play(*job, plan, canvas);
    Finish(*job);
    return 0;
}

bool Spawn(void* param) {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    if (g_unloading)
        return false;
    std::erase_if(g_workers, [](HANDLE h) {
        if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
            return false;
        CloseHandle(h);
        return true;
    });
    HANDLE thread = CreateThread(nullptr, 0, Run, param, 0, nullptr);
    if (!thread)
        return false;
    g_workers.push_back(thread);
    return true;
}

void Sweep() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_snapshots.begin(); it != g_snapshots.end();) {
        if (IsWindow(it->first)) {
            ++it;
            continue;
        }
        DeleteObject(it->second.bitmap);
        it = g_snapshots.erase(it);
    }
    std::erase_if(g_iconX, [](auto const& entry) { return !IsWindow(entry.first); });
}

bool Reserve(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_unloading && g_active.insert(hwnd).second;
}

bool Launch(HWND hwnd, bool rising) {
    RECT win;
    if (!GetWindowRect(hwnd, &win))
        return false;
    RECT rect = win, frame;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        rect = frame;
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    int rawW = win.right - win.left, rawH = win.bottom - win.top;
    if (w <= 0 || h <= 0 || rawW <= 0 || rawH <= 0 || (long long)w * h > kMaxPixels ||
        (long long)rawW * rawH > kMaxPixels)
        return false;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfo(monitor, &mi))
        return false;

    auto job = std::make_unique<Job>();
    job->h = h;
    job->hwnd = hwnd;
    job->monitor = monitor;
    job->rect = rect;
    job->rising = rising;
    job->w = w;
    job->dockX = DockX(hwnd, monitor, mi.rcMonitor);

    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    job->bitmap = CreateDib(screen, w, h, &job->bits);
    bool ready = job->bitmap && job->bits;
    if (ready && rising)
        ready = TakeCached(*job) || Capture(screen, win, *job);
    else if (ready) {
        ready = Capture(screen, win, *job);
        if (ready)
            Cache(screen, *job);
    }
    ReleaseDC(nullptr, screen);
    if (!ready)
        return false;

    HANDLE shown = nullptr;
    if (!rising) {
        shown = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (shown && !DuplicateHandle(GetCurrentProcess(), shown, GetCurrentProcess(),
                                      &job->firstFrame, 0, FALSE, DUPLICATE_SAME_ACCESS))
            job->firstFrame = nullptr;
    }

    bool wait = shown && job->firstFrame;
    Job* raw = job.release();
    if (!Spawn(raw)) {
        delete raw;
        if (shown)
            CloseHandle(shown);
        return false;
    }
    if (shown) {
        if (wait)
            WaitForSingleObject(shown, kFirstFrameWaitMs);
        CloseHandle(shown);
    }
    return true;
}

bool Start(HWND hwnd, bool rising) {
    Sweep();
    if (!Reserve(hwnd)) {
        bool owned;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            owned = g_active.count(hwnd) != 0;
        }
        if (rising)
            SetCloak(hwnd, FALSE);
        if (!owned)
            SetTransitions(hwnd, true);
        return false;
    }
    if (Launch(hwnd, rising))
        return true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active.erase(hwnd);
    }
    Revert(hwnd, rising);
    return false;
}

void Minimize(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || !ShouldAnimate(hwnd))
        return;
    SetTransitions(hwnd, false);
    Start(hwnd, false);
}

template <typename F>
auto Restore(HWND hwnd, F&& call) {
    if (!IsIconic(hwnd) || !ShouldAnimate(hwnd))
        return call();
    SetTransitions(hwnd, false);
    SetCloak(hwnd, TRUE);
    auto result = call();
    Start(hwnd, true);
    return result;
}

bool IsMinimize(int cmd) {
    return cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE;
}

void Forget(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto it = g_snapshots.find(hwnd); it != g_snapshots.end()) {
        DeleteObject(it->second.bitmap);
        g_snapshots.erase(it);
    }
    g_iconX.erase(hwnd);
}

void BeginUnload() {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    g_unloading = true;
}

void Shutdown() {
    BeginUnload();
    std::vector<HANDLE> workers;
    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        workers.swap(g_workers);
    }
    for (HANDLE h : workers) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& entry : g_snapshots)
        DeleteObject(entry.second.bitmap);
    g_snapshots.clear();
    g_iconX.clear();
}
}

using CloseWindow_t = decltype(&CloseWindow);
CloseWindow_t CloseWindow_Original;
BOOL WINAPI CloseWindow_Hook(HWND hwnd) {
    Genie::Minimize(hwnd);
    return CloseWindow_Original(hwnd);
}

using DefWindowProcW_t = decltype(&DefWindowProcW);
DefWindowProcW_t DefWindowProcW_Original;
LRESULT WINAPI DefWindowProcW_Hook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETTINGCHANGE) {
        Translucency::RefreshTheme(lParam);
    } else if (msg == WM_DESTROY) {
        Genie::Forget(hwnd);
    } else if (msg == WM_SYSCOMMAND) {
        UINT command = wParam & 0xFFF0;
        if (command == SC_MINIMIZE)
            Genie::Minimize(hwnd);
        else if (command == SC_RESTORE)
            return Genie::Restore(
                hwnd, [&] { return DefWindowProcW_Original(hwnd, msg, wParam, lParam); });
    }
    return DefWindowProcW_Original(hwnd, msg, wParam, lParam);
}

using SetWindowPlacement_t = decltype(&SetWindowPlacement);
SetWindowPlacement_t SetWindowPlacement_Original;
BOOL WINAPI SetWindowPlacement_Hook(HWND hwnd, const WINDOWPLACEMENT* placement) {
    if (placement && Genie::IsMinimize(placement->showCmd))
        Genie::Minimize(hwnd);
    return SetWindowPlacement_Original(hwnd, placement);
}

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original;
BOOL WINAPI ShowWindow_Hook(HWND hwnd, int cmd) {
    if (Genie::IsMinimize(cmd))
        Genie::Minimize(hwnd);
    else if (cmd == SW_RESTORE || cmd == SW_SHOWNORMAL)
        return Genie::Restore(hwnd, [&] { return ShowWindow_Original(hwnd, cmd); });
    return ShowWindow_Original(hwnd, cmd);
}

using ShowWindowAsync_t = decltype(&ShowWindowAsync);
ShowWindowAsync_t ShowWindowAsync_Original;
BOOL WINAPI ShowWindowAsync_Hook(HWND hwnd, int cmd) {
    if (Genie::IsMinimize(cmd))
        Genie::Minimize(hwnd);
    return ShowWindowAsync_Original(hwnd, cmd);
}

void HookGenie() {
    WindhawkUtils::SetFunctionHook(CloseWindow, CloseWindow_Hook, &CloseWindow_Original);
    WindhawkUtils::SetFunctionHook(DefWindowProcW, DefWindowProcW_Hook, &DefWindowProcW_Original);
    WindhawkUtils::SetFunctionHook(SetWindowPlacement, SetWindowPlacement_Hook,
                                   &SetWindowPlacement_Original);
    WindhawkUtils::SetFunctionHook(ShowWindow, ShowWindow_Hook, &ShowWindow_Original);
    WindhawkUtils::SetFunctionHook(ShowWindowAsync, ShowWindowAsync_Hook,
                                   &ShowWindowAsync_Original);
}

bool g_shellActive = false;

bool IsExplorerProcess() {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileName(nullptr, path, ARRAYSIZE(path)))
        return false;
    return _wcsicmp(BaseName(path), L"explorer.exe") == 0;
}

bool InitShell() {
    if (!HookTaskbarDllSymbols())
        return false;

    if (HMODULE m = GetTaskbarViewModuleHandle()) {
        g_taskbarViewDllLoaded = true;
        HookTaskbarViewDllSymbols(m);
    } else {
        HMODULE kb = GetModuleHandle(L"kernelbase.dll");
        auto pLoadLibraryExW =
            kb ? (LoadLibraryExW_t)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
        if (pLoadLibraryExW)
            WindhawkUtils::SetFunctionHook(pLoadLibraryExW, LoadLibraryExW_Hook,
                                           &LoadLibraryExW_Original);
    }
    return true;
}

void Wh_ModSettingsChanged() {
    if (g_shellActive)
        TopBar::ReloadSettings();
}

BOOL Wh_ModInit() {
    HookGenie();
    Translucency::Install(IsExplorerProcess());
    g_shellActive = IsExplorerProcess() && InitShell();
    if (g_shellActive)
        TopBar::ReloadSettings();
    return TRUE;
}

void Wh_ModAfterInit() {
    Translucency::ApplyExisting();
    if (!g_shellActive)
        return;
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
    Genie::BeginUnload();
    if (!g_shellActive)
        return;
    TopBar::Stop();
    RunOnTaskbarThread();
}

void Wh_ModUninit() {
    Translucency::Uninstall();
    Genie::Shutdown();
}
