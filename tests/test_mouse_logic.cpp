// test_mouse_logic.cpp — exhaustive tests for the pure decision core:
// zone hits, wheel/click decisions, injected/modifier passthrough, click
// pairing, wheel accumulation and volume math. No global Windows state.
#include "mouse_logic.h"
#include "settings.h"
#include "test_harness.h"

#include <winshun.h>

using namespace tv;

namespace {

RECT rect(long l, long t, long r, long b)
{
    RECT rc { l, t, r, b };
    return rc;
}

POINT ptAt(const RECT& rc)
{
    return { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
}

TaskbarSnapshot snapshotWith(POINT pt, const std::wstring& title = L"Chrome")
{
    TaskbarSnapshot s;
    TaskbarButton b;
    b.rect = rect(static_cast<long>(pt.x) - 20, static_cast<long>(pt.y) - 20,
        static_cast<long>(pt.x) + 20, static_cast<long>(pt.y) + 20);
    b.title = title;
    b.pid = 4242;
    s.appButtons.push_back(b);
    return s;
}

} // namespace

// ---- zone hits -----------------------------------------------------------------

TEST("zone: point inside an app button hits AppButton")
{
    TaskbarSnapshot s;
    TaskbarButton b;
    b.rect = rect(100, 600, 140, 640);
    b.pid = 1;
    s.appButtons.push_back(b);
    std::size_t idx = 99;
    CHECK_EQ(s.zoneAt(POINT { 120, 620 }, &idx), Zone::AppButton);
    CHECK_EQ(idx, std::size_t { 0 });
}

TEST("zone: point outside everything hits None")
{
    TaskbarSnapshot s;
    TaskbarButton b;
    b.rect = rect(100, 600, 140, 640);
    s.appButtons.push_back(b);
    s.hasVolume = true;
    s.volumeRect = rect(1500, 600, 1540, 640);
    CHECK_EQ(s.zoneAt(POINT { 0, 0 }), Zone::None);
    CHECK_EQ(s.zoneAt(POINT { 120, 599 }), Zone::None);   // one above the top edge
    CHECK_EQ(s.zoneAt(POINT { 99, 620 }), Zone::None);    // outside the left edge
    CHECK_EQ(s.zoneAt(POINT { 1540, 640 }), Zone::None);  // exclusive right/bottom
}

TEST("zone: volume rect wins only where no app button matches first")
{
    TaskbarSnapshot s;
    s.hasVolume = true;
    s.volumeRect = rect(1500, 600, 1540, 640);
    CHECK_EQ(s.zoneAt(POINT { 1520, 620 }), Zone::VolumeButton);
}

TEST("zone: stale app buttons are skipped, the volume rect still works")
{
    TaskbarSnapshot s;
    TaskbarButton b;
    b.rect = rect(100, 600, 140, 640);
    b.pid = 1;
    s.appButtons.push_back(b);
    s.hasVolume = true;
    s.volumeRect = rect(1500, 600, 1540, 640);
    s.appButtonsStale = true;
    CHECK_EQ(s.zoneAt(POINT { 120, 620 }), Zone::None);
    CHECK_EQ(s.zoneAt(POINT { 1520, 620 }), Zone::VolumeButton);
}

// ---- wheel decisions -----------------------------------------------------------

TEST("wheel: ctrl+wheel on app button adjusts that app")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent e;
    e.pt = p;
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::AppWheel);
    CHECK_EQ(d.steps, 1);
}

TEST("wheel: negative delta yields one step down")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent e;
    e.pt = p;
    e.ctrl = true;
    e.wheel = WheelDirection::Down;
    e.wheelDelta = -kWheelDelta;
    MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.steps, -1);
}

TEST("wheel: without ctrl the event passes untouched")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent e;
    e.pt = p;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    CHECK(!decideMouse(e, s).swallow);
}

TEST("wheel: injected input is never swallowed")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent e;
    e.pt = p;
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    e.injected = true;
    MouseDecision d = decideMouse(e, s);
    CHECK(!d.swallow);
    CHECK_EQ(d.action, MouseActionKind::None);
}

TEST("wheel: alt/win/shift combos pass through")
{
    POINT p = { 300, 620 };
    for (const bool alt : { true, false })
        for (const bool win : { true, false })
            for (const bool shift : { true, false }) {
                if (!alt && !win && !shift)
                    continue;
                TaskbarSnapshot s = snapshotWith(p);
                MouseEvent e;
                e.pt = p;
                e.ctrl = true;
                e.alt = alt;
                e.win = win;
                e.shift = shift;
                e.wheel = WheelDirection::Up;
                e.wheelDelta = kWheelDelta;
                CHECK(!decideMouse(e, s).swallow);
            }
}

TEST("wheel: outside the taskbar passes through")
{
    TaskbarSnapshot s = snapshotWith(POINT { 300, 620 });
    MouseEvent e;
    e.pt = POINT { 960, 300 }; // somewhere over a normal window
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    CHECK(!decideMouse(e, s).swallow);
}

TEST("wheel: ambiguous title is swallowed but blocked with a reason")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    s.appButtons[0].pid.reset();
    s.appButtons[0].ambiguous = true;
    MouseEvent e;
    e.pt = p;
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::Blocked);
    CHECK_EQ(d.reason, std::string("ambiguous-title"));
}

TEST("wheel: resolved button with no audio session is blocked with a reason")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    s.appButtons[0].pid.reset();
    MouseEvent e;
    e.pt = p;
    e.ctrl = true;
    e.wheel = WheelDirection::Down;
    e.wheelDelta = -kWheelDelta;
    MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::Blocked);
    CHECK_EQ(d.reason, std::string("no-session"));
}

TEST("wheel: volume button adjusts master volume")
{
    TaskbarSnapshot s;
    s.hasVolume = true;
    s.volumeRect = rect(1500, 600, 1540, 640);
    MouseEvent e;
    e.pt = ptAt(s.volumeRect);
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::MasterWheel);
    CHECK_EQ(d.steps, 1);
}

// ---- click decisions ------------------------------------------------------------

TEST("click: ctrl+down on app button is swallowed, plain down is not")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent down;
    down.pt = p;
    down.ctrl = true;
    down.click = ClickPhase::Down;
    MouseDecision d = decideMouse(down, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::AppClickToggle);

    MouseEvent plain = down;
    plain.ctrl = false;
    CHECK(!decideMouse(plain, s).swallow);
}

TEST("click: tracker fires the toggle only when the pointer stayed inside")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent down;
    down.pt = p;
    down.ctrl = true;
    down.click = ClickPhase::Down;
    MouseDecision d = decideMouse(down, s);
    ClickTracker t;
    t.beginDown(d, s, d.buttonIndex);
    CHECK(t.pending());

    MouseDecision up = t.onUp(p);
    CHECK(up.swallow);
    CHECK_EQ(up.action, MouseActionKind::AppClickToggle);
    CHECK(!t.pending());
}

TEST("click: pointer moved off the button swallows the up but never toggles")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent down;
    down.pt = p;
    down.ctrl = true;
    down.click = ClickPhase::Down;
    ClickTracker t;
    t.beginDown(decideMouse(down, s), s, 0);

    MouseDecision up = t.onUp(POINT { 900, 400 });
    CHECK(up.swallow);
    CHECK_EQ(up.action, MouseActionKind::None);
}

TEST("click: stray up without a pending down passes through")
{
    ClickTracker t;
    MouseDecision up = t.onUp(POINT { 300, 620 });
    CHECK(!up.swallow);
    CHECK_EQ(up.action, MouseActionKind::None);
}

TEST("click: tracker reset on disable drops the pending click safely")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent down;
    down.pt = p;
    down.ctrl = true;
    down.click = ClickPhase::Down;
    ClickTracker t;
    t.beginDown(decideMouse(down, s), s, 0);
    t.reset();
    MouseDecision up = t.onUp(p);
    CHECK(!up.swallow);
}

TEST("click: volume button click toggles master mute on up")
{
    TaskbarSnapshot s;
    s.hasVolume = true;
    s.volumeRect = rect(1500, 600, 1540, 640);
    MouseEvent down;
    down.pt = ptAt(s.volumeRect);
    down.ctrl = true;
    down.click = ClickPhase::Down;
    MouseDecision d = decideMouse(down, s);
    CHECK_EQ(d.action, MouseActionKind::MasterClickToggle);
    ClickTracker t;
    t.beginDown(d, s, 0);
    MouseDecision up = t.onUp(down.pt);
    CHECK(up.swallow);
    CHECK_EQ(up.action, MouseActionKind::MasterClickToggle);
}

TEST("click: plain taskbar click (no ctrl) is never touched")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    MouseEvent down;
    down.pt = p;
    down.click = ClickPhase::Down;
    CHECK(!decideMouse(down, s).swallow);
    MouseEvent up = down;
    up.click = ClickPhase::Up;
    CHECK(!decideMouse(up, s).swallow);
}

// ---- wheel accumulator ----------------------------------------------------------

TEST("accumulator: single standard notch yields one step")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(kWheelDelta), 1);
    CHECK_EQ(a.add(-kWheelDelta), -1);
}

TEST("accumulator: two half notches yield one step and keep the remainder")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(60), 0);
    CHECK_EQ(a.add(60), 1);
    CHECK_EQ(a.remainder(), 0);
}

TEST("accumulator: high-resolution stream accumulates into periodic steps")
{
    WheelAccumulator a;
    int steps = 0;
    for (int i = 0; i < 1000; ++i)
        steps += a.add(8); // 1000 * 8 = 8000 = 66.67 notches
    CHECK_EQ(steps, 66);
    CHECK_EQ(a.remainder(), 80); // 8000 - 66 * 120
}

TEST("accumulator: direction change discards the stale remainder")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(100), 0);
    CHECK_EQ(a.add(-100), 0); // +100 discarded, not -200 total
    CHECK_EQ(a.add(-20), -1);
}

TEST("accumulator: large burst delta yields several steps at once")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(5 * kWheelDelta), 5);
    CHECK_EQ(a.add(-2 * kWheelDelta), -2);
}

TEST("accumulator: zero delta is a no-op")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(0), 0);
    CHECK_EQ(a.add(60), 0);
    CHECK_EQ(a.add(0), 0);
    CHECK_EQ(a.add(60), 1);
}

// ---- volume math ----------------------------------------------------------------

TEST("volume: clamping at both ends")
{
    CHECK_EQ(clampPercent(-5), 0);
    CHECK_EQ(clampPercent(0), 0);
    CHECK_EQ(clampPercent(55), 55);
    CHECK_EQ(clampPercent(100), 100);
    CHECK_EQ(clampPercent(105), 100);
}

TEST("volume: stepping clamps at 0 and 100")
{
    CHECK_EQ(steppedPercent(100, 1, 2), 100);
    CHECK_EQ(steppedPercent(0, -1, 2), 0);
    CHECK_EQ(steppedPercent(99, 1, 2), 100);
    CHECK_EQ(steppedPercent(1, -1, 2), 0);
    CHECK_EQ(steppedPercent(50, 3, 2), 56);
    CHECK_EQ(steppedPercent(50, -3, 2), 44);
}

TEST("volume: out-of-range percentPerStep is clamped, not trusted")
{
    CHECK_EQ(steppedPercent(50, 1, 0), 51);
    CHECK_EQ(steppedPercent(50, 1, 99), 60);
    CHECK_EQ(steppedPercent(50, -1, -5), 49);
}

// ---- settings -------------------------------------------------------------------

TEST("settings: defaults when the object is empty or not an object")
{
    const Settings a = parseSettings(winshun::Json(winshun::Json::Object {}));
    CHECK_EQ(a.percentPerStep, 2);
    CHECK(a.showOverlay);
    CHECK_EQ(a.overlayMs, 600);
    CHECK(a.enableMouse);

    const Settings b = parseSettings(winshun::Json("nonsense"));
    CHECK_EQ(b.percentPerStep, 2);
}

TEST("settings: valid values are kept")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("percentPerStep", 5);
    s.set("showOverlay", false);
    s.set("overlayMs", 1200);
    s.set("enableMouse", false);
    const Settings out = parseSettings(s);
    CHECK_EQ(out.percentPerStep, 5);
    CHECK(!out.showOverlay);
    CHECK_EQ(out.overlayMs, 1200);
    CHECK(!out.enableMouse);
}

TEST("settings: wrong types and out-of-range values fall back to defaults")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("percentPerStep", "fast");
    s.set("overlayMs", 99999);
    const Settings out = parseSettings(s);
    CHECK_EQ(out.percentPerStep, 2);
    CHECK_EQ(out.overlayMs, 600);

    winshun::Json low(winshun::Json::Object {});
    low.set("percentPerStep", 0);
    CHECK_EQ(parseSettings(low).percentPerStep, 2);
    winshun::Json high(winshun::Json::Object {});
    high.set("percentPerStep", 11);
    CHECK_EQ(parseSettings(high).percentPerStep, 2);
}

TEST("settings: overlay anchor and offsets are validated")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("overlayAnchor", "top-right");
    s.set("overlayOffsetX", 12);
    s.set("overlayOffsetY", -34);
    const Settings out = parseSettings(s);
    CHECK_EQ(out.overlayAnchor, OverlayAnchor::TopRight);
    CHECK_EQ(out.overlayOffsetX, 12);
    CHECK_EQ(out.overlayOffsetY, -34);

    winshun::Json bad(winshun::Json::Object {});
    bad.set("overlayAnchor", "nope");
    bad.set("overlayOffsetY", 99999);
    const Settings fallback = parseSettings(bad);
    CHECK_EQ(fallback.overlayAnchor, OverlayAnchor::Center);
    CHECK_EQ(fallback.overlayOffsetY, 4000);

    winshun::Json mouse(winshun::Json::Object {});
    mouse.set("overlayAnchor", "mouse");
    CHECK_EQ(parseSettings(mouse).overlayAnchor, OverlayAnchor::Mouse);
}

// ---- button → process mapping ----------------------------------------------------

namespace {

WindowInfo makeWindow(const wchar_t* title, const wchar_t* exe, const wchar_t* appId, DWORD pid)
{
    WindowInfo w;
    w.title = title;
    w.exePath = exe;
    w.appId = appId;
    w.pid = pid;
    return w;
}

} // namespace

TEST("mapping: explicit AUMID resolves the packaged app")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe",
            L"C:\\Program Files\\WindowsApps\\Microsoft.WindowsTerminal_x\\WindowsTerminal.exe",
            L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", 11544),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App",
        L"Terminal - 1 running window pinned", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 11544 });
    CHECK(!ambiguous);
}

TEST("mapping: implicit {GUID} app id matches the executable path tail")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"*new 3 - Notepad++", L"C:\\Program Files\\Notepad++\\notepad++.exe", L"", 37640),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(
        L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Notepad++\\notepad++.exe",
        L"Notepad++ - 1 running window", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 37640 });
    CHECK(!ambiguous);
}

TEST("mapping: absolute app id separates WeChat from its plugin process")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"微信", L"C:\\Program Files\\Tencent\\Weixin\\Weixin.exe", L"", 11936),
        makeWindow(L"微信",
            L"C:\\Users\\x\\AppData\\Roaming\\Tencent\\xwechat\\xplugin\\Plugins\\RadiumWMPF\\WeChatAppEx.exe",
            L"", 8652),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(
        L"C:\\Users\\x\\AppData\\Roaming\\Tencent\\xwechat\\xplugin\\Plugins\\RadiumWMPF\\WeChatAppEx.exe",
        L"WeChatAppEx - 1 running window", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 8652 });
    CHECK(!ambiguous);
}

TEST("mapping: button name matches the executable name (Electron apps)")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"Agent", L"C:\\Users\\x\\AppData\\Local\\Programs\\Trae CN\\Trae CN.exe",
            L"", 42260),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(L"ByteDance.TraeCN",
        L"TRAE CN - 1 running window", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 42260 });
    CHECK(!ambiguous);
}

TEST("mapping: short app id matches msedge.exe for Edge")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"Issues - Microsoft Edge", L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
            L"", 27824),
        makeWindow(L"Another page - Microsoft Edge",
            L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", L"", 27824),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(L"MSEdge",
        L"Microsoft Edge - 2 running windows pinned", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 27824 });
    CHECK(!ambiguous); // two windows of one process are not ambiguous
}

TEST("mapping: dotted AUMID segment matches explorer.exe")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"dist - File Explorer", L"C:\\Windows\\explorer.exe", L"", 33732),
        makeWindow(L"Program Manager", L"C:\\Windows\\explorer.exe", L"", 33732),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(L"Microsoft.Windows.Explorer",
        L"File Explorer - 1 running window pinned", windows, ambiguous);
    CHECK(pid.has_value());
    CHECK_EQ(*pid, DWORD { 33732 });
    CHECK(!ambiguous);
}

TEST("mapping: several distinct processes are ambiguous, never guessed")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"one", L"C:\\Tools\\Clash\\Clash.exe", L"", 11),
        makeWindow(L"two", L"D:\\Apps\\Clash\\Clash.exe", L"", 22),
    };
    bool ambiguous = false;
    const auto pid = resolveButtonTarget(L"", L"Clash - 2 running windows", windows, ambiguous);
    CHECK(!pid.has_value());
    CHECK(ambiguous);
}

TEST("mapping: an unknown button resolves to nothing")
{
    const std::vector<WindowInfo> windows = {
        makeWindow(L"Other", L"C:\\Tools\\other.exe", L"", 31),
    };
    bool ambiguous = false;
    CHECK(!resolveButtonTarget(L"", L"Unknown - 1 running window", windows, ambiguous).has_value());
    CHECK(!ambiguous);
}

TEST("click: a blocked down still pairs and swallows its up")
{
    POINT p = { 300, 620 };
    TaskbarSnapshot s = snapshotWith(p);
    s.appButtons[0].pid.reset();
    MouseEvent down;
    down.pt = p;
    down.ctrl = true;
    down.click = ClickPhase::Down;
    const MouseDecision d = decideMouse(down, s);
    CHECK_EQ(d.action, MouseActionKind::Blocked);

    ClickTracker t;
    t.beginDown(d, s, d.buttonIndex);
    CHECK(t.pending());
    const MouseDecision up = t.onUp(p);
    CHECK(up.swallow);
    CHECK_EQ(up.action, MouseActionKind::None);
}

TEST("wheel: a blocked decision keeps the button index for the overlay title")
{
    TaskbarSnapshot s;
    TaskbarButton first;
    first.rect = rect(100, 600, 140, 640);
    first.title = L"First";
    first.pid = 1;
    TaskbarButton second;
    second.rect = rect(200, 600, 240, 640);
    second.title = L"Second";
    s.appButtons.push_back(first);
    s.appButtons.push_back(second);

    MouseEvent e;
    e.pt = ptAt(second.rect);
    e.ctrl = true;
    e.wheel = WheelDirection::Up;
    e.wheelDelta = kWheelDelta;
    const MouseDecision d = decideMouse(e, s);
    CHECK(d.swallow);
    CHECK_EQ(d.action, MouseActionKind::Blocked);
    CHECK_EQ(d.buttonIndex, std::size_t { 1 });
}

int registerCount = 0; // keeps translation unit alive; tests self-register
