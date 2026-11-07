// audio_controller.h — per-application and master volume control via
// Windows Core Audio. The IAudioBackend interface exists so component tests
// can substitute fake sessions; the real implementation (and every COM
// header) stays inside audio_controller.cpp behind a factory.
#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tv {

struct SessionInfo {
    std::wstring id;   // session instance identifier (stable across restarts)
    std::wstring name; // display name, or the exe base name as a fallback
    std::wstring exePath; // full path of the session's process (may be empty)
    DWORD pid = 0;
    int percent = 0;
    bool muted = false;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // Prepares the backend (COM + device binding). Default: already ready.
    virtual bool init() { return true; }

    // All functions read the state back after writing and report the actual
    // result; a successful Set* call alone is not success.
    virtual std::optional<int> sessionPercent(DWORD pid) = 0;
    virtual bool setSessionPercent(DWORD pid, int percent) = 0;
    virtual std::optional<bool> sessionMuted(DWORD pid) = 0;
    virtual bool setSessionMute(DWORD pid, bool mute, bool& nowMuted) = 0;
    virtual std::optional<int> masterPercent() = 0;
    virtual bool setMasterPercent(int percent) = 0;
    virtual std::optional<bool> masterMuted() = 0;
    virtual bool setMasterMute(bool mute, bool& nowMuted) = 0;

    // Sessions of the current default output device. System sounds and the
    // plugin's own session are excluded.
    virtual std::vector<SessionInfo> listSessions() = 0;
};

// The real backend. One instance per thread; call init() on that thread.
std::unique_ptr<IAudioBackend> createBackend();

// One queued action for the audio worker.
struct VolumeOp {
    enum class Kind { None, AppWheel, AppClickToggle, MasterWheel, MasterClickToggle, BlockedReason };
    Kind kind = Kind::None;
    DWORD pid = 0;
    int steps = 0;
    int percentPerStep = 2;
    std::wstring buttonTitle; // for overlay text
    std::string reason;       // stable key, for BlockedReason
};

// What one operation produced, for both the overlay and the log.
struct VolumeFeedback {
    bool ok = false;
    bool hasValue = false; // percent is meaningful (wheel operations)
    int percent = 0;
    std::wstring label;   // short app name / "System volume"
    std::wstring message; // error text, blocked reason, or mute result (localized)
};

// Executes one operation against a backend. Volume reads/writes match a
// session by process id OR by executable path: Chromium-based browsers play
// audio in a renderer/utility process whose pid differs from the window's.
VolumeFeedback executeVolumeOp(IAudioBackend& backend, const VolumeOp& op, bool chinese);

} // namespace tv
