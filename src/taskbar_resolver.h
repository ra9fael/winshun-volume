// taskbar_resolver.h — builds the TaskbarSnapshot the mouse hook consults.
//
// A background thread owns UI Automation (slow by design). It refreshes the
// snapshot on a slow fallback timer AND shortly after window/taskbar changes
// reported by SetWinEventHook, so a closed app does not leave stale button
// rectangles behind. The hook only ever copies the snapshot — it never calls
// UIA.
//
// Mapping rule (see resolveButtonTarget): a taskbar button is matched to a
// process by its UIA AutomationId (AppUserModelID), the executable-path form
// of the app id, the executable name, and finally the window title. Zero
// matches → no target; several distinct processes → ambiguous (event
// swallowed, reason shown). The UIA element's own PID is Explorer's and is
// never used as a target.
#pragma once

#include "mouse_logic.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace tv {

class TaskbarResolver {
public:
    TaskbarResolver() = default;
    ~TaskbarResolver() { stop(); }

    TaskbarResolver(const TaskbarResolver&) = delete;
    TaskbarResolver& operator=(const TaskbarResolver&) = delete;

    bool start();
    void stop();

    // Cheap, lock-guarded copy for the hook thread.
    TaskbarSnapshot snapshot() const;
    // Forces a synchronous refresh; returns the fresh snapshot (tests).
    TaskbarSnapshot refreshNow();

    bool running() const { return m_thread.joinable() && m_running.load(); }

private:
    void run();
    // Coalesces bursts of window/taskbar events into one near-future refresh.
    // `delayMs` lets a new app window settle before the buttons are read.
    void scheduleRefresh(ULONGLONG delayMs);
    static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime);
    static TaskbarResolver* s_live;

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    HWINEVENTHOOK m_winEvent = nullptr;
    bool m_dirty = false; // resolver thread only
    std::atomic<bool> m_pending { false }; // a rebuild is scheduled; read by the hook
    ULONGLONG m_firstEventTick = 0; // when the pending refresh was first asked for
    ULONGLONG m_lastEventTick = 0; // when the last event asked for a refresh
    ULONGLONG m_refreshDelay = 180;
    mutable std::mutex m_mutex;
    TaskbarSnapshot m_snapshot;
};

} // namespace tv
