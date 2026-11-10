// mouse_hook.cpp — low-level mouse hook implementation.
#include "mouse_hook.h"

#include "winshun.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace tv {

namespace {
MouseHook* g_hook = nullptr; // exactly one instance per process
} // namespace

bool MouseHook::start(SnapshotSource getSnapshot, ActionSink onAction)
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    m_getSnapshot = std::move(getSnapshot);
    m_onAction = std::move(onAction);
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void MouseHook::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

void MouseHook::run()
{
    m_threadId = ::GetCurrentThreadId();
    // Force the thread message queue into existence so stop() can post to it.
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // Hook-thread-only state; reset here instead of in stop() so the stopping
    // thread never touches it while the hook is running.
    m_accum.reset();
    m_tracker.reset();
    m_pendingPid.reset();
    m_pendingTitle.clear();
    g_hook = this;

    // The installing thread must pump messages for the hook to fire. A failed
    // installation retries with backoff instead of giving up: a remote-session
    // reattach or a desktop switch can make SetWindowsHookExW fail once and
    // succeed seconds later, and a permanently gesture-less plugin is worse
    // than a slow retry.
    static constexpr int kBackoffMs[] = { 300, 1000, 5000, 30000 };
    HHOOK hook = nullptr;
    int attempt = 0;
    while (m_running.load() && !hook) {
        if (attempt > 0) {
            const int wait = kBackoffMs[std::min<std::size_t>(
                static_cast<std::size_t>(attempt) - 1, std::size(kBackoffMs) - 1)];
            winshun::debug("TaskbarVolume: WH_MOUSE_LL install failed, retrying in "
                + std::to_string(wait) + " ms");
            // Sleep in slices so stop() is not delayed by a 30 s backoff.
            for (int slept = 0; slept < wait && m_running.load(); slept += 100)
                ::Sleep(100);
        }
        hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::hookProcStatic,
            ::GetModuleHandleW(nullptr), 0);
        ++attempt;
    }
    if (!hook) {
        winshun::debug("TaskbarVolume: WH_MOUSE_LL not installed; gestures disabled");
    } else {
        if (attempt > 1)
            winshun::debug("TaskbarVolume: WH_MOUSE_LL installed after "
                + std::to_string(attempt) + " attempts");
        while (m_running.load()) {
            const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0)
                break;
        }
        ::UnhookWindowsHookEx(hook);
    }
    g_hook = nullptr;
    m_running.store(false);
}

LRESULT CALLBACK MouseHook::hookProcStatic(int code, WPARAM wp, LPARAM lp)
{
    if (g_hook && code >= 0) {
        const LRESULT handled = g_hook->hookProc(code, wp, lp);
        if (handled)
            return handled; // swallowed
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

bool MouseHook::fastGate(const MSLLHOOKSTRUCT* m)
{
    if (!m || (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)))
        return false; // simulated input is never ours
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0)
        return false; // without Ctrl this plugin has no business
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        return false;
    if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        return false;
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        return false;
    return true;
}

LRESULT MouseHook::hookProc(int /*code*/, WPARAM wp, LPARAM lp)
{
    const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (!m)
        return 0;

    switch (wp) {
    case WM_MOUSEWHEEL: {
        if (!fastGate(m))
            return 0; // pass plain/injected/modifier wheels through instantly
        const int raw = static_cast<short>(HIWORD(m->mouseData));
        const int steps = m_accum.add(raw);
        if (steps == 0)
            return 0; // partial high-res delta: pass through and keep counting

        MouseEvent e;
        e.pt = m->pt;
        e.ctrl = true;
        e.wheel = steps > 0 ? WheelDirection::Up : WheelDirection::Down;
        e.wheelDelta = steps * kWheelDelta;

        const TaskbarSnapshot s = m_getSnapshot();
        MouseDecision d = decideMouse(e, s);
        if (!d.swallow)
            return 0;
        if (m_onAction) {
            if (d.action == MouseActionKind::AppWheel && d.buttonIndex < s.appButtons.size()) {
                const TaskbarButton& b = s.appButtons[d.buttonIndex];
                if (b.pid)
                    m_onAction(d, e, *b.pid, b.title);
            } else if (d.action == MouseActionKind::MasterWheel) {
                m_onAction(d, e, 0, L"系统音量");
            } else if (d.action == MouseActionKind::Blocked) {
                const std::wstring title = d.buttonIndex < s.appButtons.size()
                    ? s.appButtons[d.buttonIndex].title
                    : std::wstring();
                m_onAction(d, e, 0, title); // show the reason overlay
            }
        }
        return 1;
    }
    case WM_LBUTTONDOWN: {
        m_accum.reset();
        // A new down means any earlier up never reached us; drop the stale
        // pairing so this click cannot inherit it.
        m_tracker.reset();
        m_pendingPid.reset();
        m_pendingTitle.clear();
        if (!fastGate(m))
            return 0;
        MouseEvent e;
        e.pt = m->pt;
        e.ctrl = true;
        e.click = ClickPhase::Down;

        const TaskbarSnapshot s = m_getSnapshot();
        MouseDecision d = decideMouse(e, s);
        if (!d.swallow)
            return 0;

        // Remember the resolved target now; the up fires against it even if
        // the background refresh replaces the snapshot in between.
        const std::wstring buttonTitle = d.buttonIndex < s.appButtons.size()
            ? s.appButtons[d.buttonIndex].title
            : std::wstring();
        if (d.action == MouseActionKind::AppClickToggle && d.buttonIndex < s.appButtons.size()) {
            const TaskbarButton& b = s.appButtons[d.buttonIndex];
            if (b.pid) {
                m_pendingPid = *b.pid;
                m_pendingTitle = b.title;
            }
        } else if (d.action == MouseActionKind::MasterClickToggle) {
            m_pendingPid = 0;
            m_pendingTitle = L"系统音量";
        } else if (d.action == MouseActionKind::Blocked && m_onAction) {
            m_onAction(d, e, 0, buttonTitle); // reason overlay at down; no toggle at up
        }

        // Blocked downs are tracked too: their up must be swallowed as well.
        m_tracker.beginDown(d, s, d.buttonIndex);
        return 1;
    }
    case WM_LBUTTONUP: {
        MouseEvent e;
        e.pt = m->pt;
        e.click = ClickPhase::Up;
        const MouseDecision d = m_tracker.onUp(e.pt);
        if (!d.swallow)
            return 0; // no pending recognized down: native behavior
        if (m_onAction && m_pendingPid.has_value()
            && (d.action == MouseActionKind::AppClickToggle
                || d.action == MouseActionKind::MasterClickToggle)) {
            m_onAction(d, e, *m_pendingPid, m_pendingTitle);
        }
        m_pendingPid.reset();
        m_pendingTitle.clear();
        return 1;
    }
    default:
        return 0;
    }
}

} // namespace tv
