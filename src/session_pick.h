// session_pick.h — the session data and how sessions are ordered for reading.
//
// Pure values and comparisons only (no Core Audio): the rule that decides
// "which session's percentage do I show and step from" is unit-tested without
// a sound device.
#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace tv {

enum class SessionState {
    Inactive = 0, // the session exists but no stream is playing right now
    Active = 1,   // a stream is playing: this is what the user hears
};

struct SessionInfo {
    std::wstring id;         // session instance identifier
    std::wstring name;       // display name, or the exe base name as a fallback
    std::wstring exePath;    // full path of the session's process (may be empty)
    std::wstring deviceName; // friendly name of the render endpoint
    DWORD pid = 0;
    int percent = 0;
    bool muted = false;
    SessionState state = SessionState::Inactive;
    // False when the session lives on an output device that is not the current
    // default (a per-app output choice, or a stream pinned to another device).
    bool onDefaultDevice = true;
};

// Lower is better. An active stream wins over the default device, because a
// silent session on the default device is not what the user is listening to;
// the default device only breaks ties between sessions that are equally alive.
inline int sessionRank(const SessionInfo& s)
{
    return (s.state == SessionState::Active ? 0 : 2) + (s.onDefaultDevice ? 0 : 1);
}

inline bool sessionBefore(const SessionInfo& a, const SessionInfo& b)
{
    return sessionRank(a) < sessionRank(b);
}

// Orders sessions so the first one an app owns is the one being heard. Reads
// and the `vol` list both take the first match, so this is the whole rule.
inline void sortSessionsForRead(std::vector<SessionInfo>& sessions)
{
    std::stable_sort(sessions.begin(), sessions.end(), sessionBefore);
}

} // namespace tv
