// test_sessions.cpp — the rule that picks which session's value is shown and
// stepped from. Pure data: no sound device, no COM.
#include "session_pick.h"
#include "test_harness.h"

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
