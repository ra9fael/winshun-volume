// mouse_logic.h — pure decision logic for the TaskbarVolume plugin.
//
// Everything here is side-effect free so it can be unit tested exhaustively
// without installing a global mouse hook, enumerating audio sessions or
// touching UI Automation. The hook thread and the protocol layer only glue
// real events into these functions.
//
// English-only comments per project convention.
#pragma once

#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tv {

constexpr int kWheelDelta = 120; // one standard wheel notch

// ---- events -------------------------------------------------------------------

enum class WheelDirection { None, Up, Down };
enum class ClickPhase { None, Up, Down };

// One low-level mouse event, normalized. Modifiers come from
// GetAsyncKeyState at hook time; `injected` from LLMHF_INJECTED.
struct MouseEvent {
    POINT pt{};
    bool ctrl = false;
    bool alt = false;
    bool win = false;
    bool shift = false;
    bool injected = false;
    WheelDirection wheel = WheelDirection::None;
    ClickPhase click = ClickPhase::None;
    int wheelDelta = 0; // raw signed delta; only meaningful when wheel != None
};

// ---- the taskbar snapshot ------------------------------------------------------

enum class Zone { None, AppButton, VolumeButton };

// A taskbar app button together with the audio target the resolver found.
// `pid` empty and `ambiguous` false means "no audio session / no window match";
// `ambiguous` true means several candidate windows share the title — never
// guess. The UIA element's own ProcessId always belongs to Explorer and is
// never stored here.
struct TaskbarButton {
    RECT rect{};
    std::wstring title;
    std::optional<DWORD> pid;
    bool ambiguous = false;
};

// A read-only, cheap-to-copy snapshot of the taskbar the hook can consult.
struct TaskbarSnapshot {
    std::vector<TaskbarButton> appButtons;
    RECT volumeRect{};
    bool hasVolume = false;
    // A window/taskbar change is waiting for the next rebuild: the app-button
    // rectangles may already be wrong, so gestures over them pass through
    // instead of acting on the previous layout. The volume rect is stable and
    // keeps working.
    bool appButtonsStale = false;

    Zone zoneAt(POINT pt, std::size_t* index = nullptr) const;
};

// ---- button → process mapping --------------------------------------------------

// One visible top-level window, as collected once per refresh. A taskbar
// button is mapped to a process by, in order: its explicit AppUserModelID,
// the implicit "{GUID}\relative\exe" app id, the executable name / window
// title. The rules are pure data so they can be unit tested.
struct WindowInfo {
    std::wstring title;
    std::wstring exePath;
    std::wstring appId; // explicit AUMID of the owning process; empty if none
    DWORD pid = 0;
};

// Maps one taskbar button to the process that owns it. `buttonAppId` is the
// UIA AutomationId with its "Appid: " prefix removed; `buttonName` is the
// accessibility name ("Chrome - 1 running window", "微信"). Sets `ambiguous`
// when several distinct processes match; returns nullopt then, and when
// nothing matches.
std::optional<DWORD> resolveButtonTarget(const std::wstring& buttonAppId,
    const std::wstring& buttonName, const std::vector<WindowInfo>& windows,
    bool& ambiguous);

// ---- decisions -----------------------------------------------------------------

enum class MouseActionKind {
    None,           // pass the event through untouched
    AppWheel,       // adjust the app's session volume by `steps`
    AppClickToggle, // toggle the app's session mute (fired on button-up)
    MasterWheel,    // adjust the master volume by `steps`
    MasterClickToggle,
    Blocked, // swallow and show `reason`; a recognized but unusable target
};

struct MouseDecision {
    bool swallow = false;
    MouseActionKind action = MouseActionKind::None;
    std::size_t buttonIndex = 0; // valid for AppWheel / AppClickToggle
    int steps = 0;               // signed; valid for wheel actions
    std::string reason;          // stable key for Blocked, e.g. "ambiguous-title"

    static MouseDecision pass() { return {}; }
    static MouseDecision blocked(std::string reasonKey)
    {
        MouseDecision d;
        d.swallow = true;
        d.action = MouseActionKind::Blocked;
        d.reason = std::move(reasonKey);
        return d;
    }
};

// Decide one wheel event or one left-button-down. Left-button-up is decided
// by ClickTracker (pairing needs state). Events that do not belong to the
// plugin (no Ctrl, with Alt/Win, injected, outside the taskbar, plain clicks)
// pass through with swallow = false.
MouseDecision decideMouse(const MouseEvent& e, const TaskbarSnapshot& s);

// Click down/up pairing. The plugin swallows both halves of a recognized
// Ctrl+click so the taskbar never sees a synthetic half-click.
class ClickTracker {
public:
    // Call with the decision already returned by decideMouse() for the down.
    // A blocked down is tracked too: its up must be swallowed as well so the
    // taskbar never sees a lone button-up.
    void beginDown(const MouseDecision& d, const TaskbarSnapshot& s, std::size_t buttonIndex);

    // Decide the matching up. Returns a swallow-only decision when the
    // pointer left the button (never act on a moved click), the toggle
    // decision when it is still inside, or swallow-only for a blocked down.
    // Safe to call without a pending down.
    MouseDecision onUp(POINT pt);

    bool pending() const { return m_pending; }
    void reset()
    {
        m_pending = false;
        m_blocked = false;
    }

private:
    bool m_pending = false;
    bool m_blocked = false; // the down was swallowed without a possible action
    RECT m_targetRect{};
    std::size_t m_buttonIndex = 0;
    bool m_overVolume = false;
};

// ---- wheel accumulation ---------------------------------------------------------

// High-precision touchpads emit many small deltas (e.g. 8 per pixel); apply
// one adjustment per full notch and keep the remainder for later.
class WheelAccumulator {
public:
    // Feed one raw delta; returns the number of full steps to apply now
    // (positive = up). A direction change discards the stale remainder.
    int add(int delta);

    int remainder() const { return m_remainder; }
    void reset() { m_remainder = 0; }

private:
    int m_remainder = 0;
};

// ---- volume math ----------------------------------------------------------------

int clampPercent(int v); // clamp to 0..100
// current volume + steps * percentPerStep, clamped to 0..100.
int steppedPercent(int current, int steps, int percentPerStep);

} // namespace tv
