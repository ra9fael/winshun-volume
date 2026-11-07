// mouse_logic.cpp — see mouse_logic.h.
#include "mouse_logic.h"

#include <cwctype>
#include <set>

namespace tv {

Zone TaskbarSnapshot::zoneAt(POINT pt, std::size_t* index) const
{
    if (!appButtonsStale) {
        for (std::size_t i = 0; i < appButtons.size(); ++i) {
            if (PtInRect(&appButtons[i].rect, pt)) {
                if (index)
                    *index = i;
                return Zone::AppButton;
            }
        }
    }
    if (hasVolume && PtInRect(&volumeRect, pt))
        return Zone::VolumeButton;
    return Zone::None;
}

namespace {

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(::towlower(c));
    return s;
}

bool pathLike(const std::wstring& appId)
{
    return appId.find(L'\\') != std::wstring::npos;
}

// "chrome.exe" → "chrome"; other names unchanged.
std::wstring stripExe(std::wstring name)
{
    if (name.size() > 4 && lower(name).compare(name.size() - 4, 4, L".exe") == 0)
        name.resize(name.size() - 4);
    return name;
}

std::wstring baseName(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// "Microsoft.Windows.Explorer" → "Explorer". Packaged ids ("…!App") and
// path-like ids have no useful last segment; very short segments ("app")
// are too generic to match by executable name.
std::wstring lastDotSegment(const std::wstring& id)
{
    if (id.empty() || id.find(L'!') != std::wstring::npos || id.find(L'\\') != std::wstring::npos)
        return {};
    const std::size_t dot = id.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= id.size())
        return {};
    const std::wstring segment = id.substr(dot + 1);
    return segment.size() >= 4 ? segment : std::wstring();
}

// "Chrome - 1 running window" → "Chrome"; the suffix is localized, so every
// comparison is also tried against the full name.
std::wstring buttonBase(const std::wstring& name)
{
    const std::size_t dash = name.find(L" - ");
    return dash == std::wstring::npos ? name : name.substr(0, dash);
}

bool equalsCi(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && !b.empty() && lower(a) == lower(b);
}

// True when `full` ends with `tail` on a path-component boundary, so
// "C:\Program Files\Notepad++\notepad++.exe" ends with
// "Notepad++\notepad++.exe" but not with "pad++.exe".
bool endsWithPath(const std::wstring& full, const std::wstring& tail)
{
    if (full.empty() || tail.empty() || tail.size() > full.size())
        return false;
    const std::wstring f = lower(full);
    const std::wstring t = lower(tail);
    if (f.compare(f.size() - t.size(), t.size(), t) != 0)
        return false;
    if (f.size() == t.size())
        return true;
    const wchar_t before = f[f.size() - t.size() - 1];
    return before == L'\\' || before == L'/';
}

std::optional<DWORD> decidePids(const std::set<DWORD>& pids, bool& ambiguous)
{
    if (pids.empty())
        return std::nullopt;
    if (pids.size() > 1) {
        ambiguous = true;
        return std::nullopt;
    }
    return *pids.begin();
}

// Shared target check for a hit app button. A target that cannot be mapped
// safely is swallowed and explained instead of guessed; the button index is
// kept so the caller can name the app in the overlay.
MouseDecision appTargetDecision(const TaskbarButton& b, std::size_t index, MouseActionKind kind, int steps)
{
    if (b.ambiguous || !b.pid.has_value()) {
        MouseDecision d = MouseDecision::blocked(b.ambiguous ? "ambiguous-title" : "no-session");
        d.buttonIndex = index;
        return d;
    }
    MouseDecision d;
    d.swallow = true;
    d.action = kind;
    d.buttonIndex = index;
    d.steps = steps;
    return d;
}

} // namespace

std::optional<DWORD> resolveButtonTarget(const std::wstring& buttonAppId,
    const std::wstring& buttonName, const std::vector<WindowInfo>& windows,
    bool& ambiguous)
{
    ambiguous = false;
    if (windows.empty())
        return std::nullopt;

    auto collect = [&](auto&& predicate) {
        std::set<DWORD> pids;
        for (const WindowInfo& w : windows) {
            if (w.pid != 0 && predicate(w))
                pids.insert(w.pid);
        }
        return pids;
    };

    // 1. Explicit AppUserModelID ("MSEdge", "ByteDance.TraeCN",
    //    "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App").
    if (!buttonAppId.empty() && !pathLike(buttonAppId) && buttonAppId[0] != L'{') {
        const auto pids = collect([&](const WindowInfo& w) { return equalsCi(w.appId, buttonAppId); });
        if (const auto pid = decidePids(pids, ambiguous))
            return pid;
        if (ambiguous)
            return std::nullopt;
    }

    // 2. Implicit "{GUID}\relative\exe" or absolute-path app id: compare the
    //    executable path tail. This covers unpackaged win32 apps.
    if (!buttonAppId.empty() && pathLike(buttonAppId)) {
        std::wstring tail = buttonAppId;
        if (tail[0] == L'{') {
            const std::size_t brace = tail.find(L'}');
            if (brace != std::wstring::npos)
                tail = tail.substr(brace + 1);
            if (!tail.empty() && (tail[0] == L'\\' || tail[0] == L'/'))
                tail.erase(0, 1);
        }
        if (!tail.empty() && tail.size() >= 3) {
            const auto pids = collect([&](const WindowInfo& w) { return endsWithPath(w.exePath, tail); });
            if (const auto pid = decidePids(pids, ambiguous))
                return pid;
            if (ambiguous)
                return std::nullopt;
        }
    }

    // 3a. Button display name vs executable name ("TRAE CN" ↔ "Trae CN.exe",
    //     "skills-manager" ↔ "skills-manager.exe").
    if (!buttonName.empty()) {
        const std::wstring base = buttonBase(buttonName);
        const auto pids = collect([&](const WindowInfo& w) {
            return equalsCi(base, stripExe(baseName(w.exePath)));
        });
        if (const auto pid = decidePids(pids, ambiguous))
            return pid;
        if (ambiguous)
            return std::nullopt;
    }

    // 3b. App id vs executable name ("MSEdge" ↔ "msedge.exe"; the dotted
    //     last segment "…Explorer" ↔ "explorer.exe").
    if (!buttonAppId.empty()) {
        const std::wstring last = lastDotSegment(buttonAppId);
        const auto pids = collect([&](const WindowInfo& w) {
            const std::wstring exe = stripExe(baseName(w.exePath));
            return equalsCi(buttonAppId, exe) || equalsCi(last, exe);
        });
        if (const auto pid = decidePids(pids, ambiguous))
            return pid;
        if (ambiguous)
            return std::nullopt;
    }

    // 4. Window title as the last resort (apps whose button name is the
    //    title, followed by the localized running-window suffix).
    if (!buttonName.empty()) {
        const std::wstring base = buttonBase(buttonName);
        const std::wstring lowerBase = lower(base);
        const auto pids = collect([&](const WindowInfo& w) {
            if (equalsCi(buttonName, w.title) || equalsCi(base, w.title))
                return true;
            const std::wstring lowerTitle = lower(w.title);
            return !lowerBase.empty() && lowerTitle.size() > lowerBase.size()
                && lowerTitle.compare(0, lowerBase.size(), lowerBase) == 0
                && lowerTitle[lowerBase.size()] == L'-';
        });
        if (const auto pid = decidePids(pids, ambiguous))
            return pid;
    }
    return std::nullopt;
}

MouseDecision decideMouse(const MouseEvent& e, const TaskbarSnapshot& s)
{
    // Never interfere with simulated input, mixed modifier combos, or plain
    // (Ctrl-free) gestures — those belong to the taskbar.
    if (e.injected || e.alt || e.win || e.shift || !e.ctrl)
        return MouseDecision::pass();

    const bool isWheel = e.wheel != WheelDirection::None;
    const bool isClickDown = e.click == ClickPhase::Down;
    if (!isWheel && !isClickDown)
        return MouseDecision::pass(); // Up events go through ClickTracker.

    std::size_t index = 0;
    const Zone zone = s.zoneAt(e.pt, &index);
    switch (zone) {
    case Zone::None:
        return MouseDecision::pass();
    case Zone::AppButton:
        if (isWheel)
            return appTargetDecision(s.appButtons[index], index, MouseActionKind::AppWheel, e.wheelDelta >= 0 ? 1 : -1);
        return appTargetDecision(s.appButtons[index], index, MouseActionKind::AppClickToggle, 0);
    case Zone::VolumeButton: {
        MouseDecision d;
        d.swallow = true;
        d.action = isWheel ? MouseActionKind::MasterWheel : MouseActionKind::MasterClickToggle;
        d.steps = isWheel ? (e.wheelDelta >= 0 ? 1 : -1) : 0;
        return d;
    }
    }
    return MouseDecision::pass();
}

void ClickTracker::beginDown(const MouseDecision& d, const TaskbarSnapshot& s, std::size_t buttonIndex)
{
    if (!d.swallow)
        return; // a pass-through down: nothing to pair
    if (d.action != MouseActionKind::AppClickToggle && d.action != MouseActionKind::MasterClickToggle
        && d.action != MouseActionKind::Blocked)
        return;
    m_pending = true;
    m_blocked = d.action == MouseActionKind::Blocked;
    m_buttonIndex = buttonIndex;
    m_overVolume = d.action == MouseActionKind::MasterClickToggle;
    if (m_blocked)
        return; // swallow the up, never act
    if (m_overVolume)
        m_targetRect = s.volumeRect;
    else if (buttonIndex < s.appButtons.size())
        m_targetRect = s.appButtons[buttonIndex].rect;
    else
        m_pending = false; // snapshot changed between decision and pairing
}

MouseDecision ClickTracker::onUp(POINT pt)
{
    if (!m_pending)
        return MouseDecision::pass();
    m_pending = false;
    // The down was already swallowed; always swallow the matching up so the
    // taskbar never receives a half-click. Act only when the pointer stayed
    // inside the button — a moved click is a cancel, never a toggle.
    MouseDecision d;
    d.swallow = true;
    if (m_blocked) {
        m_blocked = false;
        return d;
    }
    if (!PtInRect(&m_targetRect, pt))
        return d; // swallowed, no action
    d.action = m_overVolume ? MouseActionKind::MasterClickToggle : MouseActionKind::AppClickToggle;
    d.buttonIndex = m_buttonIndex;
    return d;
}

int WheelAccumulator::add(int delta)
{
    if (delta == 0)
        return 0;
    if (m_remainder != 0 && ((delta < 0) != (m_remainder < 0)))
        m_remainder = 0; // direction reversed: the old remainder is stale
    m_remainder += delta;
    int steps = 0;
    while (m_remainder >= kWheelDelta) {
        ++steps;
        m_remainder -= kWheelDelta;
    }
    while (m_remainder <= -kWheelDelta) {
        --steps;
        m_remainder += kWheelDelta;
    }
    return steps;
}

int clampPercent(int v)
{
    if (v < 0)
        return 0;
    if (v > 100)
        return 100;
    return v;
}

int steppedPercent(int current, int steps, int percentPerStep)
{
    if (percentPerStep < 1)
        percentPerStep = 1;
    if (percentPerStep > 10)
        percentPerStep = 10;
    return clampPercent(current + steps * percentPerStep);
}

} // namespace tv
