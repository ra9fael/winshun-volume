// test_volume_ops.cpp — the queued operations the worker executes, tested
// against a fake backend: stepping, clamping, mute toggling and the failure /
// "no session" wording, in both languages. No audio device, no COM.
#include "audio_controller.h"
#include "mouse_logic.h"
#include "test_harness.h"

#include <map>
#include <optional>

using namespace tv;

namespace {

struct FakeSession {
    int percent = 50;
    bool muted = false;
};

// One in-memory session per pid, plus a master value. Reads fail only when
// failReads is set (a device that went away); writes fail only when failWrites
// is set (the audio engine refused the write).
class FakeBackend final : public IAudioBackend {
public:
    std::map<DWORD, FakeSession> sessions;
    int master = 40;
    bool masterMutedValue = false;
    bool failWrites = false;
    bool failReads = false;
    std::wstring deviceName; // what defaultDeviceName() reports

    std::wstring defaultDeviceName() override { return deviceName; }

    std::optional<int> sessionPercent(DWORD pid) override
    {
        if (failReads)
            return std::nullopt;
        const auto it = sessions.find(pid);
        return it == sessions.end() ? std::nullopt : std::optional<int>(it->second.percent);
    }

    bool setSessionPercent(DWORD pid, int percent) override
    {
        if (failWrites)
            return false;
        const auto it = sessions.find(pid);
        if (it == sessions.end())
            return false;
        it->second.percent = clampPercent(percent);
        return true;
    }

    std::optional<bool> sessionMuted(DWORD pid) override
    {
        if (failReads)
            return std::nullopt;
        const auto it = sessions.find(pid);
        return it == sessions.end() ? std::nullopt : std::optional<bool>(it->second.muted);
    }

    bool setSessionMute(DWORD pid, bool mute, bool& nowMuted) override
    {
        if (failWrites)
            return false;
        const auto it = sessions.find(pid);
        if (it == sessions.end())
            return false;
        it->second.muted = mute;
        nowMuted = mute;
        return true;
    }

    std::optional<int> masterPercent() override
    {
        return failReads ? std::nullopt : std::optional<int>(master);
    }

    bool setMasterPercent(int percent) override
    {
        if (failWrites)
            return false;
        master = clampPercent(percent);
        return true;
    }

    std::optional<bool> masterMuted() override
    {
        return failReads ? std::nullopt : std::optional<bool>(masterMutedValue);
    }

    bool setMasterMute(bool mute, bool& nowMuted) override
    {
        if (failWrites)
            return false;
        masterMutedValue = mute;
        nowMuted = mute;
        return true;
    }

    std::vector<SessionInfo> listSessions() override { return {}; }
};

VolumeOp appWheel(int steps, int percentPerStep = 2)
{
    VolumeOp op;
    op.kind = VolumeOp::Kind::AppWheel;
    op.pid = 4242;
    op.steps = steps;
    op.percentPerStep = percentPerStep;
    op.buttonTitle = L"Chrome - 2 running windows pinned";
    return op;
}

VolumeOp appClick()
{
    VolumeOp op;
    op.kind = VolumeOp::Kind::AppClickToggle;
    op.pid = 4242;
    op.buttonTitle = L"Chrome - 2 running windows pinned";
    return op;
}

VolumeOp masterWheel(int steps)
{
    VolumeOp op;
    op.kind = VolumeOp::Kind::MasterWheel;
    op.steps = steps;
    op.buttonTitle = L"系统音量";
    return op;
}

VolumeOp masterClick()
{
    VolumeOp op;
    op.kind = VolumeOp::Kind::MasterClickToggle;
    op.buttonTitle = L"系统音量";
    return op;
}

VolumeOp blocked(const char* reason)
{
    VolumeOp op;
    op.kind = VolumeOp::Kind::BlockedReason;
    op.reason = reason;
    op.buttonTitle = L"Chrome";
    return op;
}

bool contains(const std::wstring& haystack, const wchar_t* needle)
{
    return haystack.find(needle) != std::wstring::npos;
}

} // namespace

TEST("op: app wheel steps up from the session's current value")
{
    FakeBackend b;
    b.sessions[4242] = { 30, false };
    const VolumeFeedback fb = executeVolumeOp(b, appWheel(1), true);
    CHECK(fb.ok);
    CHECK(fb.hasValue);
    CHECK_EQ(fb.percent, 32);
    CHECK_EQ(fb.label, std::wstring(L"Chrome"));
    CHECK_EQ(b.sessions[4242].percent, 32);
}

TEST("op: app wheel steps down")
{
    FakeBackend b;
    b.sessions[4242] = { 30, false };
    const VolumeFeedback fb = executeVolumeOp(b, appWheel(-1), true);
    CHECK(fb.ok);
    CHECK_EQ(fb.percent, 28);
    CHECK_EQ(b.sessions[4242].percent, 28);
}

TEST("op: app wheel clamps at 100 and 0")
{
    FakeBackend top;
    top.sessions[4242] = { 99, false };
    const VolumeFeedback up = executeVolumeOp(top, appWheel(1), true);
    CHECK(up.ok);
    CHECK_EQ(up.percent, 100);

    FakeBackend bottom;
    bottom.sessions[4242] = { 1, false };
    const VolumeFeedback down = executeVolumeOp(bottom, appWheel(-1), true);
    CHECK(down.ok);
    CHECK_EQ(down.percent, 0);
}

TEST("op: an out-of-range percentPerStep is clamped, not trusted")
{
    FakeBackend b;
    b.sessions[4242] = { 50, false };
    const VolumeFeedback fb = executeVolumeOp(b, appWheel(1, 999), true);
    CHECK_EQ(fb.percent, 60); // 999 clamps to 10
}

TEST("op: a failed app write is reported without a value")
{
    FakeBackend b;
    b.sessions[4242] = { 30, false };
    b.failWrites = true;
    const VolumeFeedback zh = executeVolumeOp(b, appWheel(1), true);
    CHECK(!zh.ok);
    CHECK(!zh.hasValue);
    CHECK(contains(zh.message, L"调整失败"));
    CHECK(contains(zh.message, L"Chrome"));
    const VolumeFeedback en = executeVolumeOp(b, appWheel(1), false);
    CHECK(contains(en.message, L"Failed: Chrome"));
}

TEST("op: a silent app explains instead of pretending to write")
{
    FakeBackend b;
    const VolumeFeedback zh = executeVolumeOp(b, appWheel(1), true);
    CHECK(zh.ok); // nothing to do, but the overlay must say why
    CHECK(!zh.hasValue);
    CHECK(contains(zh.message, L"没有音频会话"));
    CHECK(contains(zh.message, L"Chrome"));
    const VolumeFeedback en = executeVolumeOp(b, appWheel(1), false);
    CHECK(contains(en.message, L"no audio session"));
}

TEST("op: app click mutes and reports the new state")
{
    FakeBackend b;
    b.sessions[4242] = { 30, false };
    const VolumeFeedback fb = executeVolumeOp(b, appClick(), true);
    CHECK(fb.ok);
    CHECK(!fb.hasValue);
    CHECK(contains(fb.message, L"已静音"));
    CHECK(b.sessions[4242].muted);
    const VolumeFeedback en = executeVolumeOp(b, appClick(), false);
    CHECK(contains(en.message, L"unmuted"));
    CHECK(!b.sessions[4242].muted);
}

TEST("op: an app whose mute state cannot be read explains instead of toggling")
{
    FakeBackend b;
    b.failReads = true;
    const VolumeFeedback zh = executeVolumeOp(b, appClick(), true);
    CHECK(zh.ok);
    CHECK(!zh.hasValue);
    CHECK(contains(zh.message, L"没有音频会话"));
}

TEST("op: master wheel steps the system volume")
{
    FakeBackend b;
    const VolumeFeedback fb = executeVolumeOp(b, masterWheel(1), true);
    CHECK(fb.ok);
    CHECK(fb.hasValue);
    CHECK_EQ(fb.percent, 42);
    CHECK_EQ(fb.label, std::wstring(L"系统音量")); // unnamed device falls back
    CHECK_EQ(b.master, 42);
}

TEST("op: the master panel names the output device")
{
    FakeBackend b;
    b.deviceName = L"Headphones (SHEYIN X5)";
    const VolumeFeedback zh = executeVolumeOp(b, masterWheel(1), true);
    CHECK_EQ(zh.label, std::wstring(L"Headphones (SHEYIN X5)"));
    const VolumeFeedback en = executeVolumeOp(b, masterWheel(-1), false);
    CHECK_EQ(en.label, std::wstring(L"Headphones (SHEYIN X5)"));
    b.masterMutedValue = true;
    const VolumeFeedback unmute = executeVolumeOp(b, masterClick(), true);
    CHECK(contains(unmute.message, L"Headphones (SHEYIN X5)"));
    CHECK(contains(unmute.message, L"取消静音"));
}

TEST("op: master click toggles the system mute")
{
    FakeBackend b;
    const VolumeFeedback fb = executeVolumeOp(b, masterClick(), true);
    CHECK(fb.ok);
    CHECK(contains(fb.message, L"已静音"));
    CHECK(b.masterMutedValue);
    const VolumeFeedback en = executeVolumeOp(b, masterClick(), false);
    CHECK(contains(en.message, L"unmuted"));
    CHECK(!b.masterMutedValue);
}

TEST("op: an unreadable master state is reported, never guessed")
{
    FakeBackend b;
    b.failReads = true;
    const VolumeFeedback wheel = executeVolumeOp(b, masterWheel(1), true);
    CHECK(!wheel.ok);
    CHECK(!wheel.hasValue);
    CHECK(contains(wheel.message, L"无法读取系统音量"));
    const VolumeFeedback click = executeVolumeOp(b, masterClick(), true);
    CHECK(!click.ok);
    CHECK(contains(click.message, L"无法读取静音状态"));
}

TEST("op: blocked reasons become localized messages")
{
    FakeBackend b;
    const VolumeFeedback ambiguous = executeVolumeOp(b, blocked("ambiguous-title"), true);
    CHECK(ambiguous.ok); // the event was swallowed on purpose
    CHECK(!ambiguous.hasValue);
    CHECK(contains(ambiguous.message, L"标题不唯一"));
    const VolumeFeedback noSession = executeVolumeOp(b, blocked("no-session"), true);
    CHECK(contains(noSession.message, L"没有音频会话"));
    const VolumeFeedback unknown = executeVolumeOp(b, blocked("something-else"), false);
    CHECK(contains(unknown.message, L"Nothing to control here"));
}
