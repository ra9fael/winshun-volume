// mouse_hook.h — the WH_MOUSE_LL hook on its own message-pumping thread.
//
// Contract (see docs/plugins.md plan):
//  * The callback does O(µs) work only: read the point, check modifiers,
//    consult a pre-built snapshot, decide. UI Automation and Core Audio are
//    NEVER called here; actions are handed to a sink that posts to a worker.
//  * Only Ctrl+wheel and Ctrl+left-click are intercepted; everything else —
//    injected input, Alt/Win/Shift combos, plain clicks, points outside the
//    taskbar — passes through untouched.
//  * Partial high-precision wheel deltas are passed through immediately and
//    accumulated; the volume changes one step per full notch.
//  * Click pairing remembers the resolved target at down time, so the up
//    cannot act on a changed snapshot.
#pragma once

#include "mouse_logic.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace tv {

class MouseHook {
public:
    // Sink runs on the hook thread but must only enqueue (fast). For app
    // actions pid is the resolved target; for master actions it is 0 and
    // title is a display name; for Blocked it carries the button title.
    using ActionSink = std::function<void(const MouseDecision&, const MouseEvent&, DWORD pid, const std::wstring& title)>;
    using SnapshotSource = std::function<TaskbarSnapshot()>;

    MouseHook() = default;
    ~MouseHook() { stop(); }
    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    bool start(SnapshotSource getSnapshot, ActionSink onAction);
    void stop();
    bool active() const { return m_running.load(); }

private:
    void run();
    static LRESULT CALLBACK hookProcStatic(int code, WPARAM wp, LPARAM lp);
    LRESULT hookProc(int code, WPARAM wp, LPARAM lp);
    // Cheap gate before touching the snapshot copy.
    static bool fastGate(const MSLLHOOKSTRUCT* m);

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    SnapshotSource m_getSnapshot;
    ActionSink m_onAction;

    // Hook-thread-only state.
    WheelAccumulator m_accum;
    ClickTracker m_tracker;
    std::optional<DWORD> m_pendingPid;
    std::wstring m_pendingTitle;
};

} // namespace tv
