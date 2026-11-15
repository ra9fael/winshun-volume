// audio_controller.cpp — Core Audio implementation. One worker thread owns
// the backend: the mouse hook never touches COM, it only queues VolumeOps.
#include "audio_controller.h"
#include "mouse_logic.h"
#include "winshun.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <shellapi.h>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

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

// DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, …) spelled out: the header that
// defines it (functiondiscoverykeys_devpkey.h) is not part of every MinGW
// installation, and the value is part of the documented endpoint property set.
const PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

std::wstring friendlyNameOf(IMMDevice* device)
{
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store)
        return {};
    PROPVARIANT value;
    ::PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR
        && value.pwszVal)
        name = value.pwszVal;
    ::PropVariantClear(&value);
    store->Release();
    return name;
}

std::wstring deviceIdOf(IMMDevice* device)
{
    LPWSTR id = nullptr;
    std::wstring value;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        value = id;
        ::CoTaskMemFree(id);
    }
    return value;
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
        releaseBinding();
        safeRelease(m_enumerator);
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
        // listSessions() is ordered so the first match is the session the user
        // hears, not an idle session of the same app on another device.
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
        std::map<DWORD, std::wstring> exeCache;
        forEachSession([&](const EndpointInfo& endpoint, IAudioSessionControl2* control, bool active,
                           DWORD pid) {
            SessionInfo info;
            info.pid = pid;
            info.state = active ? SessionState::Active : SessionState::Inactive;
            info.onDefaultDevice = endpoint.isDefault;
            info.deviceName = endpoint.name;

            LPWSTR id = nullptr;
            if (SUCCEEDED(control->GetSessionIdentifier(&id)) && id) {
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
            if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
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
        });
        sortSessionsForRead(out);
        return out;
    }

    bool setSessionPercent(DWORD pid, int percent) override
    {
        const float target = clampPercent(percent) / 100.f;
        bool written = false;
        // Every session of the app on every active output device, not just the
        // default one: a per-app output choice or a stream pinned to another
        // device is still the app's volume.
        forEachAppSession(pid, [&](ISimpleAudioVolume* volume) {
            if (FAILED(volume->SetMasterVolume(target, nullptr)))
                return;
            float readBack = 0.f;
            if (SUCCEEDED(volume->GetMasterVolume(&readBack))
                && std::fabs(readBack - target) < 0.005f)
                written = true;
        });
        return written;
    }

    bool setSessionMute(DWORD pid, bool mute, bool& nowMuted) override
    {
        bool written = false;
        forEachAppSession(pid, [&](ISimpleAudioVolume* volume) {
            if (FAILED(volume->SetMute(mute ? TRUE : FALSE, nullptr)))
                return;
            BOOL readBack = FALSE;
            if (SUCCEEDED(volume->GetMute(&readBack))) {
                nowMuted = readBack != 0;
                if ((readBack != 0) == mute)
                    written = true;
            }
        });
        return written;
    }

private:
    struct EndpointInfo {
        std::wstring name;
        bool isDefault = false;
    };

    // The output device the user hears. eConsole is the role the tray flyout
    // and Settings show; the other roles are fallbacks for a role that has no
    // default, so a media device is never missed.
    IMMDevice* openDefaultDevice()
    {
        if (!ensureEnumerator())
            return nullptr;
        static const ERole kRoles[] = { eConsole, eMultimedia, eCommunications };
        for (ERole role : kRoles) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(eRender, role, &device)) && device)
                return device;
        }
        return nullptr;
    }

    std::wstring defaultDeviceId()
    {
        IMMDevice* device = openDefaultDevice();
        if (!device)
            return {};
        std::wstring id = deviceIdOf(device);
        device->Release();
        return id;
    }

    bool ensureEnumerator()
    {
        if (m_enumerator)
            return true;
        const HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&m_enumerator));
        if (FAILED(hr)) {
            m_enumerator = nullptr;
            return false;
        }
        return true;
    }

    // Rebinds after device loss. Returns false when there is no default render
    // device any more; callers must not retry against the released pointers.
    bool rebind()
    {
        releaseBinding();
        return bindDefault();
    }

    // A device that merely stops being the default keeps its endpoint alive and
    // keeps accepting SetMasterVolumeLevelScalar, so a binding that is never
    // re-validated silently adjusts a device nobody is listening to - and the
    // read-back succeeds, so the overlay still shows the new value. The default
    // device is therefore compared on every call; nothing is cached.
    bool ensureBound()
    {
        if (m_device && m_endpoint) {
            const std::wstring current = defaultDeviceId();
            if (!current.empty() && current == m_deviceId)
                return true;
        }
        return rebind();
    }

    bool bindDefault()
    {
        IMMDevice* device = openDefaultDevice();
        if (!device)
            return false;
        const std::wstring id = deviceIdOf(device);
        IAudioEndpointVolume* endpoint = nullptr;
        const HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&endpoint));
        if (FAILED(hr) || !endpoint) {
            device->Release();
            return false;
        }
        m_device = device;
        m_endpoint = endpoint;
        m_deviceId = id;
        // One line per device rather than per call: this is what tells a
        // "the plugin adjusts the wrong output" report apart from anything else
        // in plugins.log, without logging on every wheel notch.
        if (m_deviceId != m_loggedDeviceId) {
            m_loggedDeviceId = m_deviceId;
            winshun::debug("TaskbarVolume: following output device "
                + winshun::utf8(friendlyNameOf(device)));
        }
        return true;
    }

    void releaseBinding()
    {
        safeRelease(m_endpoint);
        safeRelease(m_device);
        m_deviceId.clear();
    }

    // Calls `fn(device, endpoint)` for every active render endpoint.
    template <typename F>
    void forEachEndpoint(F&& fn)
    {
        if (!ensureEnumerator())
            return;
        const std::wstring defaultId = defaultDeviceId();
        IMMDeviceCollection* collection = nullptr;
        if (FAILED(m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))
            || !collection)
            return;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device)
                continue;
            EndpointInfo info;
            info.name = friendlyNameOf(device);
            const std::wstring id = deviceIdOf(device);
            info.isDefault = !id.empty() && id == defaultId;
            fn(device, info);
            device->Release();
        }
        collection->Release();
    }

    // Walks every live session of every active render endpoint. Expired
    // sessions, system sounds (no owning process) and our own session are never
    // controllable targets and are skipped here.
    template <typename F>
    void forEachSession(F&& fn)
    {
        forEachEndpoint([&](IMMDevice* device, const EndpointInfo& endpoint) {
            IAudioSessionManager2* manager = nullptr;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&manager)))
                || !manager)
                return;
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
                int count = 0;
                sessions->GetCount(&count);
                for (int i = 0; i < count; ++i) {
                    IAudioSessionControl* control = nullptr;
                    if (FAILED(sessions->GetSession(i, &control)) || !control)
                        continue;
                    IAudioSessionControl2* control2 = nullptr;
                    if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                            reinterpret_cast<void**>(&control2)))
                        && control2) {
                        AudioSessionState state = AudioSessionStateInactive;
                        DWORD pid = 0;
                        control2->GetState(&state);
                        control2->GetProcessId(&pid);
                        if (state != AudioSessionStateExpired && pid != 0
                            && pid != ::GetCurrentProcessId())
                            fn(endpoint, control2, state == AudioSessionStateActive, pid);
                        control2->Release();
                    }
                    control->Release();
                }
                sessions->Release();
            }
            manager->Release();
        });
    }

    // Every session the app owns on every active output device.
    template <typename F>
    void forEachAppSession(DWORD pid, F&& fn)
    {
        const std::wstring targetExe = exePathOf(pid);
        std::map<DWORD, std::wstring> exeCache;
        forEachSession([&](const EndpointInfo&, IAudioSessionControl2* control, bool, DWORD sessionPid) {
            auto exeIt = exeCache.find(sessionPid);
            if (exeIt == exeCache.end())
                exeIt = exeCache.emplace(sessionPid, exePathOf(sessionPid)).first;
            if (!sameApplication(sessionPid, exeIt->second, pid, targetExe))
                return;
            ISimpleAudioVolume* simple = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                    reinterpret_cast<void**>(&simple)))
                && simple) {
                fn(simple);
                simple->Release();
            }
        });
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

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioEndpointVolume* m_endpoint = nullptr;
    std::wstring m_deviceId;
    std::wstring m_loggedDeviceId; // last device id written to the log
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
