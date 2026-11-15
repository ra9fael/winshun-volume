// test_sessions.cpp — the rule that picks which session's value is shown and
// stepped from. Pure data: no sound device, no COM.
#include "session_pick.h"
#include "test_harness.h"

#include <map>

using namespace tv;

namespace {

SessionInfo session(SessionState state, bool onDefault, int percent = 50)
{
    SessionInfo s;
    s.state = state;
    s.onDefaultDevice = onDefault;
    s.percent = percent;
    s.pid = 4242;
    return s;
}

} // namespace

TEST("session: an active stream on the default device ranks first")
{
    CHECK_EQ(sessionRank(session(SessionState::Active, true)), 0);
    CHECK(sessionBefore(session(SessionState::Active, true), session(SessionState::Inactive, true)));
}

TEST("session: playing on another device still beats a silent session on the default")
{
    // The reported bug: Edge plays on the headset, a stale session sits on the
    // default device. The stale one must never provide the value to step from.
    const SessionInfo playing = session(SessionState::Active, false);
    const SessionInfo stale = session(SessionState::Inactive, true);
    CHECK(sessionRank(playing) < sessionRank(stale));
    CHECK(sessionBefore(playing, stale));
}

TEST("session: rank grows as a session becomes less audible")
{
    CHECK(sessionRank(session(SessionState::Active, true)) < sessionRank(session(SessionState::Active, false)));
    CHECK(sessionRank(session(SessionState::Active, false)) < sessionRank(session(SessionState::Inactive, true)));
    CHECK(sessionRank(session(SessionState::Inactive, true)) < sessionRank(session(SessionState::Inactive, false)));
}

TEST("session: sorting puts the audible session first and keeps equal ranks in order")
{
    std::vector<SessionInfo> list;
    list.push_back(session(SessionState::Inactive, false, 10));
    list.push_back(session(SessionState::Inactive, true, 20));
    list.push_back(session(SessionState::Active, false, 30));
    list.push_back(session(SessionState::Active, true, 40));
    list.push_back(session(SessionState::Inactive, true, 50));

    sortSessionsForRead(list);

    CHECK_EQ(list.size(), std::size_t { 5 });
    CHECK_EQ(list[0].percent, 40); // active, default
    CHECK_EQ(list[1].percent, 30); // active, other device
    CHECK_EQ(list[2].percent, 20); // inactive, default (stable: 20 before 50)
    CHECK_EQ(list[3].percent, 50);
    CHECK_EQ(list[4].percent, 10); // inactive, other device
}

TEST("session: a default-constructed session is treated as silent on the default device")
{
    const SessionInfo s;
    CHECK_EQ(sessionRank(s), 2); // not the preferred one, but never the last resort
    CHECK(s.onDefaultDevice);
    CHECK_EQ(s.percent, 0);
    CHECK_EQ(s.pid, DWORD { 0 });
}

TEST("session: sorting an empty or single-session list is safe")
{
    std::vector<SessionInfo> empty;
    sortSessionsForRead(empty);
    CHECK(empty.empty());

    std::vector<SessionInfo> one;
    one.push_back(session(SessionState::Inactive, false, 7));
    sortSessionsForRead(one);
    CHECK_EQ(one.size(), std::size_t { 1 });
    CHECK_EQ(one[0].percent, 7);
}

// ---- matching a session to an app -------------------------------------------------

TEST("match: the same process always matches")
{
    std::map<DWORD, DWORD> parents;
    CHECK(sessionMatchesApp(4242, L"", 4242, L"", parents));
    CHECK(sessionMatchesApp(4242, L"C:\\a\\x.exe", 4242, L"C:\\b\\y.exe", parents));
}

TEST("match: the same executable covers Chromium's audio subprocess")
{
    std::map<DWORD, DWORD> parents;
    CHECK(sessionMatchesApp(500, L"C:\\Program Files\\Edge\\msedge.exe", 111,
        L"C:\\Program Files\\Edge\\msedge.exe", parents));
    CHECK(!sessionMatchesApp(500, L"C:\\Program Files\\Edge\\msedge.exe", 111,
        L"C:\\Program Files\\Other\\other.exe", parents));
    // Path comparison is case-insensitive: QueryFullProcessImageNameW case
    // depends on how the process was started.
    CHECK(sessionMatchesApp(500, L"C:\\PrograM FileS\\edge\\MsEdge.Exe", 111,
        L"c:\\program files\\EDGE\\msedge.exe", parents));
}

TEST("match: a WebView2 helper is its host's descendant")
{
    // ms-teams.exe -> msedgewebview2.exe (browser) -> msedgewebview2.exe (audio)
    std::map<DWORD, DWORD> parents { { 30, 20 }, { 20, 10 } };
    CHECK(sessionMatchesApp(30, L"C:\\wb\\msedgewebview2.exe", 10,
        L"C:\\teams\\ms-teams.exe", parents));
}

TEST("match: the walk is capped, so unrelated descendants stay unrelated")
{
    std::map<DWORD, DWORD> parents { { 40, 30 }, { 30, 20 }, { 20, 10 }, { 10, 5 } };
    // Three steps up from 40 reach 10: inside the cap.
    CHECK(sessionMatchesApp(40, L"", 10, L"", parents));
    // Four steps would reach 5: beyond the cap, no match.
    CHECK(!sessionMatchesApp(40, L"", 5, L"", parents));
    // A dead-end chain (parent pid no longer in the snapshot) matches nothing.
    std::map<DWORD, DWORD> orphan { { 40, 9999 } };
    CHECK(!sessionMatchesApp(40, L"", 10, L"", orphan));
}

TEST("match: an empty parent map means the first two rules only")
{
    const std::map<DWORD, DWORD> none;
    CHECK(sessionMatchesApp(500, L"C:\\a\\x.exe", 111, L"C:\\a\\x.exe", none));
    CHECK(!sessionMatchesApp(500, L"C:\\wb\\msedgewebview2.exe", 10, L"C:\\teams\\ms-teams.exe", none));
}

TEST("match: no relation at all matches nothing")
{
    std::map<DWORD, DWORD> parents { { 30, 20 }, { 20, 10 } };
    CHECK(!sessionMatchesApp(30, L"C:\\x\\a.exe", 77, L"C:\\y\\b.exe", parents));
    CHECK(!sessionMatchesApp(30, L"", 77, L"", parents));
}
