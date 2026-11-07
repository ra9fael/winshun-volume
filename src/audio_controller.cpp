// audio_controller.cpp — Core Audio implementation. One worker thread owns
// the backend: the mouse hook never touches COM, it only queues VolumeOps.
#include "audio_controller.h"
#include "mouse_logic.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>

#include <cmath>
#include <map>
#include <memory>

namespace tv {

// ---- COM helpers -----------------------------------------------------------------

namespace {

template <typename T>
void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring exePathOf(DWORD pid)
{
    std::wstring path;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buffer[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(sizeof buffer / sizeof buffer[0]);
        if (::QueryFullProcessImageNameW(h, 0, buffer, &size) && size > 0)
            path.assign(buffer, size);
        ::CloseHandle(h);
    }
    return path;
}

std::wstring baseNameOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A session belongs to the requested app when it is the same process or when
// the session's process runs the same executable. The latter is what makes
// Chrome/Edge work: their audio session lives in a renderer or utility
// process, never in the window's browser process.
bool sameApplication(DWORD sessionPid, const std::wstring& sessionExe, DWORD targetPid,
    const std::wstring& targetExe)
{
    if (sessionPid == targetPid)
        return true;
    if (targetExe.empty() || sessionExe.empty())
        return false;
    return ::CompareStringOrdinal(sessionExe.c_str(), -1, targetExe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

// ---- the real backend -------------------------------------------------------------

class CoreAudioBackend final : public IAudioBackend {
public:
    ~CoreAudioBackend() override
    {
        safeRelease(m_manager);
        safeRelease(m_endpoint);
        safeRelease(m_device);
        if (m_coInit)
            ::CoUninitialize();
    }

    bool init() override
    {
        if (!m_coInit) {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr))
                return false; // includes RPC_E_CHANGED_MODE
            m_coInit = true;
        }
        return ensureBound();
    }

    std::optional<int> sessionPercent(DWORD pid) override
    {
        const std::wstring targetExe = exePathOf(pid);
        for (const SessionInfo& s : listSessions())
            if (sameApplication(s.pid, s.exePath, pid, targetExe))
                return s.percent;
        return std::nullopt;
    }

    std::optional<bool> sessionMuted(DWORD pid) override
    {
        const std::wstring targetExe = exePathOf(pid);
        for (const SessionInfo& s : listSessions())
            if (sameApplication(s.pid, s.exePath, pid, targetExe))
                return s.muted;
        return std::nullopt;
    }

    std::optional<int> masterPercent() override
    {
        if (!ensureBound() || !m_endpoint)
            return std::nullopt;
        float v = 0.f;
        if (FAILED(withRebind([&] { return m_endpoint->GetMasterVolumeLevelScalar(&v); })))
            return std::nullopt;
        return static_cast<int>(std::lround(v * 100.f));
    }

    std::optional<bool> masterMuted() override
    {
        if (!ensureBound() || !m_endpoint)
            return std::nullopt;
        BOOL muted = FALSE;
        if (FAILED(withRebind([&] { return m_endpoint->GetMute(&muted); })))
            return std::nullopt;
        return muted != 0;
    }

    bool setMasterPercent(int percent) override
    {
        if (!ensureBound() || !m_endpoint)
            return false;
        const float target = clampPercent(percent) / 100.f;
        if (FAILED(withRebind(
                [&] { return m_endpoint->SetMasterVolumeLevelScalar(target, nullptr); })))
            return false;
        if (!m_endpoint)
            return false; // the device went away during a rebind
        float readBack = 0.f;
        if (FAILED(withRebind([&] { return m_endpoint->GetMasterVolumeLevelScalar(&readBack); })))
            return false;
        return std::fabs(readBack - target) < 0.005f;
    }

    bool setMasterMute(bool mute, bool& nowMuted) override
    {
        if (!ensureBound() || !m_endpoint)
            return false;
        if (FAILED(withRebind([&] { return m_endpoint->SetMute(mute ? TRUE : FALSE, nullptr); })))
            return false;
        if (!m_endpoint)
            return false;
        BOOL readBack = FALSE;
        if (FAILED(withRebind([&] { return m_endpoint->GetMute(&readBack); })))
            return false;
        nowMuted = readBack != 0;
        return (readBack != 0) == mute;
    }

    std::vector<SessionInfo> listSessions() override
    {
        std::vector<SessionInfo> out;
        if (!ensureBound() || !ensureSessionManager())
            return out;

        IAudioSessionEnumerator* sessions = nullptr;
        HRESULT hr = withRebind([&] {
            if (!ensureSessionManager())
                return static_cast<HRESULT>(E_FAIL);
            return m_manager->GetSessionEnumerator(&sessions);
        });
        if (FAILED(hr) || !sessions)
            return out;

        int count = 0;
        sessions->GetCount(&count);
        std::map<DWORD, std::wstring> exeCache;
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(sessions->GetSession(i, &control)) || !control)
                continue;
            IAudioSessionControl2* control2 = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                    reinterpret_cast<void**>(&control2)))) {
                AudioSessionState state = AudioSessionStateInactive;
                control2->GetState(&state);
                if (state != AudioSessionStateExpired) {
                    DWORD pid = 0;
                    control2->GetProcessId(&pid);
                    // System sounds (no owning process) and our own session
                    // are never controllable targets.
                    if (pid != 0 && pid != ::GetCurrentProcessId()) {
                        SessionInfo info;
                        info.pid = pid;

                        // Stable id: the session instance identifier.
                        LPWSTR id = nullptr;
                        if (SUCCEEDED(control2->GetSessionIdentifier(&id)) && id) {
                            info.id = id;
                            ::CoTaskMemFree(id);
                        }

                        LPWSTR display = nullptr;
                        if (SUCCEEDED(control->GetDisplayName(&display)) && display) {
                            info.name = display;
                            ::CoTaskMemFree(display);
                        }
                        auto exeIt = exeCache.find(pid);
                        if (exeIt == exeCache.end())
                            exeIt = exeCache.emplace(pid, exePathOf(pid)).first;
                        info.exePath = exeIt->second;
                        if (info.name.empty())
                            info.name = baseNameOf(info.exePath);

                        ISimpleAudioVolume* simple = nullptr;
                        if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&simple)))) {
                            float v = 0.f;
                            if (SUCCEEDED(simple->GetMasterVolume(&v)))
                                info.percent = static_cast<int>(std::lround(v * 100.f));
                            BOOL muted = FALSE;
                            simple->GetMute(&muted);
                            info.muted = muted != 0;
                            simple->Release();
                        }
                        out.push_back(std::move(info));
                    }
                }
                control2->Release();
            }
            control->Release();
        }
        sessions->Release();
        return out;
    }

    bool setSessionPercent(DWORD pid, int percent) override
    {
        if (!ensureBound() || !ensureSessionManager())
            return false;
        const float target = clampPercent(percent) / 100.f;
        bool any = false;

        IAudioSessionEnumerator* sessions = nullptr;
        HRESULT enumHr = withRebind([&] {
            if (!ensureSessionManager())
                return static_cast<HRESULT>(E_FAIL);
            return m_manager->GetSessionEnumerator(&sessions);
        });
        if (FAILED(enumHr) || !sessions)
            return false;
        int count = 0;
        sessions->GetCount(&count);
        const std::wstring targetExe = exePathOf(pid);
        std::map<DWORD, std::wstring> exeCache;
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(sessions->GetSession(i, &control)) || !control)
                continue;
            IAudioSessionControl2* control2 = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                    reinterpret_cast<void**>(&control2)))) {
                DWORD sessionPid = 0;
                control2->GetProcessId(&sessionPid);
                AudioSessionState state = AudioSessionStateInactive;
                control2->GetState(&state);
                if (state != AudioSessionStateExpired) {
                    auto exeIt = exeCache.find(sessionPid);
                    if (exeIt == exeCache.end())
                        exeIt = exeCache.emplace(sessionPid, exePathOf(sessionPid)).first;
                    if (sameApplication(sessionPid, exeIt->second, pid, targetExe)) {
                        ISimpleAudioVolume* simple = nullptr;
                        if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&simple)))) {
                            if (SUCCEEDED(simple->SetMasterVolume(target, nullptr))) {
                                float readBack = 0.f;
                                if (SUCCEEDED(simple->GetMasterVolume(&readBack))
                                    && std::fabs(readBack - target) < 0.005f)
                                    any = true;
                            }
                            simple->Release();
                        }
                    }
                }
                control2->Release();
            }
            control->Release();
        }
        sessions->Release();
        return any;
    }

    bool setSessionMute(DWORD pid, bool mute, bool& nowMuted) override
    {
        if (!ensureBound() || !ensureSessionManager())
            return false;
        bool any = false;
        IAudioSessionEnumerator* sessions = nullptr;
        HRESULT enumHr = withRebind([&] {
            if (!ensureSessionManager())
                return static_cast<HRESULT>(E_FAIL);
            return m_manager->GetSessionEnumerator(&sessions);
        });
        if (FAILED(enumHr) || !sessions)
            return false;
        int count = 0;
        sessions->GetCount(&count);
        const std::wstring targetExe = exePathOf(pid);
        std::map<DWORD, std::wstring> exeCache;
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(sessions->GetSession(i, &control)) || !control)
                continue;
            IAudioSessionControl2* control2 = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                    reinterpret_cast<void**>(&control2)))) {
                DWORD sessionPid = 0;
                control2->GetProcessId(&sessionPid);
                AudioSessionState state = AudioSessionStateInactive;
                control2->GetState(&state);
                if (state != AudioSessionStateExpired) {
                    auto exeIt = exeCache.find(sessionPid);
                    if (exeIt == exeCache.end())
                        exeIt = exeCache.emplace(sessionPid, exePathOf(sessionPid)).first;
                    if (sameApplication(sessionPid, exeIt->second, pid, targetExe)) {
                        ISimpleAudioVolume* simple = nullptr;
                        if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&simple)))) {
                            if (SUCCEEDED(simple->SetMute(mute ? TRUE : FALSE, nullptr))) {
                                BOOL readBack = FALSE;
                                if (SUCCEEDED(simple->GetMute(&readBack))) {
                                    nowMuted = readBack != 0;
                                    if ((readBack != 0) == mute)
                                        any = true;
                                }
                            }
                            simple->Release();
                        }
                    }
                }
                control2->Release();
            }
            control->Release();
        }
        sessions->Release();
        return any;
    }

private:
    // Rebinds after device loss. Returns false when there is no default render
    // device any more; callers must not retry against the released pointers.
    bool rebind()
    {
        safeRelease(m_manager);
        safeRelease(m_endpoint);
        safeRelease(m_device);
        return ensureBound();
    }

    bool ensureBound()
    {
        if (m_device && m_endpoint)
            return true;
        safeRelease(m_manager);
        safeRelease(m_endpoint);
        safeRelease(m_device);

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr))
            return false;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        enumerator->Release();
        if (FAILED(hr) || !m_device)
            return false;

        hr = m_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&m_endpoint));
        if (FAILED(hr)) {
            safeRelease(m_device);
            return false;
        }
        return true;
    }

    // IAudioSessionManager2 is optional (master volume works without it) and
    // activated lazily so a missing one does not rebind the device every call.
    bool ensureSessionManager()
    {
        if (m_manager)
            return true;
        if (!m_device || !m_endpoint)
            return false;
        HRESULT hr = m_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&m_manager));
        if (FAILED(hr)) {
            m_manager = nullptr;
            return false;
        }
        return true;
    }

    // Rebinds once after device invalidation, then retries `fn` exactly once;
    // a second failure (or a failed rebind) surfaces to the caller.
    template <typename F>
    auto withRebind(F&& fn) -> decltype(fn())
    {
        HRESULT hr = fn();
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
            if (!rebind())
                return hr; // no new binding: never retry against null pointers
            hr = fn();
        }
        return hr;
    }

    IMMDevice* m_device = nullptr;
    IAudioEndpointVolume* m_endpoint = nullptr;
    IAudioSessionManager2* m_manager = nullptr;
    bool m_coInit = false;
};

std::unique_ptr<IAudioBackend> createBackend()
{
    return std::make_unique<CoreAudioBackend>();
}

// ---- op execution ------------------------------------------------------------------

namespace {

// "Chrome · no audio session" (or zh) without a dangling separator when the
// button title is empty.
std::wstring titled(const std::wstring& title, const wchar_t* zh, const wchar_t* en, bool chinese)
{
    const std::wstring suffix = chinese ? zh : en;
    if (title.empty())
        return suffix;
    return title + L" · " + suffix;
}

// A taskbar button name is "Microsoft Edge - 2 running windows pinned"; the
// overlay only needs "Microsoft Edge".
std::wstring shortLabel(std::wstring name)
{
    const std::size_t dash = name.find(L" - ");
    if (dash != std::wstring::npos)
        name.resize(dash);
    constexpr wchar_t kPinned[] = L" pinned";
    constexpr std::size_t kPinnedLen = sizeof kPinned / sizeof kPinned[0] - 1;
    if (name.size() > kPinnedLen
        && ::CompareStringOrdinal(name.c_str() + name.size() - kPinnedLen,
               static_cast<int>(kPinnedLen), kPinned, static_cast<int>(kPinnedLen), TRUE) == CSTR_EQUAL)
        name.resize(name.size() - kPinnedLen);
    return name;
}

} // namespace

VolumeFeedback executeVolumeOp(IAudioBackend& backend, const VolumeOp& op, bool chinese)
{
    VolumeFeedback fb;
    switch (op.kind) {
    case VolumeOp::Kind::AppWheel: {
        const std::optional<int> current = backend.sessionPercent(op.pid);
        if (!current) {
            // Windows only exposes per-app volume for live sessions; a silent
            // app has none until it plays something. Say so plainly instead of
            // pretending a read failed.
            fb.ok = true;
            fb.message = titled(shortLabel(op.buttonTitle), L"没有音频会话", L"no audio session", chinese);
            return fb;
        }
        const int next = steppedPercent(*current, op.steps, op.percentPerStep);
        fb.ok = backend.setSessionPercent(op.pid, next);
        if (!fb.ok) {
            fb.message = chinese ? L"调整失败：" + shortLabel(op.buttonTitle)
                                 : L"Failed: " + shortLabel(op.buttonTitle);
            return fb;
        }
        fb.hasValue = true;
        fb.percent = next;
        fb.label = shortLabel(op.buttonTitle);
        return fb;
    }
    case VolumeOp::Kind::AppClickToggle: {
        // Read the previous state first, then set the opposite — a toggle,
        // not a forced mute.
        const std::optional<bool> wasMuted = backend.sessionMuted(op.pid);
        if (!wasMuted) {
            fb.ok = true; // no live session: nothing to toggle
            fb.message = titled(shortLabel(op.buttonTitle), L"没有音频会话", L"no audio session", chinese);
            return fb;
        }
        bool nowMuted = false;
        fb.ok = backend.setSessionMute(op.pid, !*wasMuted, nowMuted);
        if (!fb.ok) {
            fb.message = chinese ? L"切换静音失败：" + shortLabel(op.buttonTitle)
                                 : L"Failed to toggle mute: " + shortLabel(op.buttonTitle);
            return fb;
        }
        fb.message = shortLabel(op.buttonTitle)
            + (chinese ? (nowMuted ? L" · 已静音" : L" · 取消静音")
                       : (nowMuted ? L" · muted" : L" · unmuted"));
        return fb;
    }
    case VolumeOp::Kind::MasterWheel: {
        const std::optional<int> current = backend.masterPercent();
        if (!current) {
            fb.message = chinese ? L"无法读取系统音量" : L"Cannot read the system volume";
            return fb;
        }
        const int next = steppedPercent(*current, op.steps, op.percentPerStep);
        fb.ok = backend.setMasterPercent(next);
        if (!fb.ok) {
            fb.message = chinese ? L"调整系统音量失败" : L"Failed to change the system volume";
            return fb;
        }
        fb.hasValue = true;
        fb.percent = next;
        fb.label = chinese ? L"系统音量" : L"System volume";
        return fb;
    }
    case VolumeOp::Kind::MasterClickToggle: {
        const std::optional<bool> wasMuted = backend.masterMuted();
        if (!wasMuted) {
            fb.message = chinese ? L"无法读取静音状态" : L"Cannot read the mute state";
            return fb;
        }
        bool nowMuted = false;
        fb.ok = backend.setMasterMute(!*wasMuted, nowMuted);
        if (!fb.ok) {
            fb.message = chinese ? L"切换系统静音失败" : L"Failed to toggle the system mute";
            return fb;
        }
        const std::wstring label = chinese ? L"系统音量" : L"System volume";
        if (chinese)
            fb.message = nowMuted ? label + L" · 已静音" : label + L" · 取消静音";
        else
            fb.message = nowMuted ? label + L" · muted" : label + L" · unmuted";
        return fb;
    }
    case VolumeOp::Kind::BlockedReason:
        fb.ok = true; // the event was intentionally swallowed; nothing to do
        if (op.reason == "ambiguous-title")
            fb.message = chinese ? L"无法确定目标应用：标题不唯一"
                                 : L"Cannot determine the app: duplicate titles";
        else if (op.reason == "no-session")
            fb.message = titled(shortLabel(op.buttonTitle), L"没有音频会话", L"no audio session", chinese);
        else
            fb.message = chinese ? L"该位置无法操作" : L"Nothing to control here";
        return fb;
    case VolumeOp::Kind::None:
        break;
    }
    return fb;
}

} // namespace tv
