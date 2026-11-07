// main.cpp — TaskbarVolume: a Win顺 resident plugin.
//
// Mouse: Ctrl+wheel over a taskbar app button adjusts that app's audio
// session; over the tray volume icon it adjusts the master volume. Ctrl+
// left-click toggles mute. Recognized-but-unusable targets swallow the
// event and explain why in the overlay; everything else passes through.
//
// Keyboard: the `vol` keyword lists master volume and per-app sessions with
// read-only queries; Enter toggles mute, Ctrl+Enter up, Shift+Enter down.
#include "winshun.h"

#include "audio_controller.h"
#include "audio_worker.h"
#include "mouse_hook.h"
#include "settings.h"
#include "taskbar_resolver.h"

#include <cwctype>
#include <memory>
#include <mutex>
#include <set>

using winshun::Action;
namespace effect = winshun::effect;
using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;
using winshun::ActionRequest;

namespace {

std::wstring toLower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(::towlower(c));
    return s;
}

std::wstring percentText(int percent, bool muted, bool chinese)
{
    std::wstring t = std::to_wstring(percent) + L"%";
    if (muted)
        t += chinese ? L" · 已静音" : L" · muted";
    return t;
}

} // namespace

class VolumePlugin : public winshun::Plugin {
public:
    VolumePlugin()
    {
        enablePerMonitorDpi();
        s_instance = this;
        m_worker.start();
    }

    ~VolumePlugin() override
    {
        m_hook.stop();
        m_resolver.stop();
        m_worker.stop();
        m_protocolBackend.reset(); // COM object of the protocol thread
        if (s_instance == this)
            s_instance = nullptr;
    }

protected:
    std::string name() const override { return "taskbar-volume"; }

    // initialize carries the merged settings (defaults + the user's values);
    // settingsChanged only follows a later edit. Applying them here is what
    // makes a saved percentPerStep / enableMouse take effect after a restart.
    void initialized() override
    {
        applySettings(tv::parseSettings(settings()));
    }

    void appearanceChanged() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.chinese = chinese();
        m_worker.applySettings(m_settings);
    }

    Items query(const Query& q) override
    {
        const bool zh = chinese();
        tv::IAudioBackend* backend = protocolBackend();
        if (!backend) {
            Item error("audio-unavailable",
                zh ? "音频服务不可用" : "Audio service unavailable",
                zh ? "无法访问 Windows Core Audio，请确认有声卡设备后重试"
                   : "Windows Core Audio is unavailable; check the audio device and retry");
            error.invalid();
            return { error };
        }

        Items items;
        // Master volume first.
        const std::optional<int> master = backend->masterPercent();
        const std::optional<bool> masterMuted = backend->masterMuted();
        if (master) {
            Item it("master", zh ? "系统音量" : "System volume",
                winshun::utf8(percentText(*master, masterMuted.value_or(false), zh)));
            it.withData(Json::Object { { "target", "master" } });
            items.push_back(std::move(it));
        }

        // Sessions, optionally filtered. One row per process: the plugin
        // writes every session of a pid, so repeated rows for the same pid
        // would only show values that are about to be overwritten.
        const std::string filter = winshun::trim(q.text);
        const std::wstring wfilter = toLower(winshun::wide(filter));
        std::set<DWORD> seenPids;
        for (const tv::SessionInfo& s : backend->listSessions()) {
            if (!wfilter.empty() && toLower(s.name).find(wfilter) == std::wstring::npos)
                continue;
            if (!seenPids.insert(s.pid).second)
                continue;
            std::string id = s.id.empty()
                ? "session:" + std::to_string(s.pid) + ":" + winshun::utf8(s.name)
                : "session:" + winshun::utf8(s.id);
            const std::string subtitle = "PID " + std::to_string(s.pid)
                + " · " + winshun::utf8(percentText(s.percent, s.muted, zh));
            Item it(std::move(id), winshun::utf8(s.name), subtitle);
            it.withData(Json::Object { { "target", "session" }, { "pid", s.pid } });
            items.push_back(std::move(it));
        }

        if (items.empty()) {
            Item empty("no-match",
                zh ? "没有匹配的音频会话" : "No matching audio session",
                zh ? "输入应用名筛选，或留空列出全部"
                   : "Type an app name to filter, or leave it empty to list all");
            empty.invalid();
            items.push_back(std::move(empty));
        }
        return items;
    }

    Effects action(const ActionRequest& a) override
    {
        const bool zh = chinese();
        const Json& data = a.item["data"];
        const std::string target = data["target"].toString();
        const DWORD pid = static_cast<DWORD>(data["pid"].toInt(0));

        tv::IAudioBackend* backend = protocolBackend();
        if (!backend)
            return { effect::notify(zh ? "音频服务不可用" : "Audio service unavailable"), effect::hide() };

        tv::Settings settings;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            settings = m_settings;
        }

        std::string text;
        if (a.action == "toggleMute") {
            if (target == "master") {
                const std::optional<bool> muted = backend->masterMuted();
                if (!muted)
                    text = zh ? "无法读取静音状态" : "Cannot read the mute state";
                else {
                    bool now = false;
                    text = backend->setMasterMute(!*muted, now)
                        ? (zh ? (now ? "系统音量 · 已静音" : "系统音量 · 取消静音")
                              : (now ? "System volume · muted" : "System volume · unmuted"))
                        : (zh ? "切换系统静音失败" : "Failed to toggle the system mute");
                }
            } else if (target == "session" && pid != 0) {
                const std::optional<bool> muted = backend->sessionMuted(pid);
                if (!muted)
                    text = zh ? "该应用没有音频会话" : "This app has no audio session";
                else {
                    bool now = false;
                    text = backend->setSessionMute(pid, !*muted, now)
                        ? (zh ? (now ? "已静音" : "取消静音") : (now ? "muted" : "unmuted"))
                        : (zh ? "切换静音失败" : "Failed to toggle mute");
                }
            }
        } else if (a.action == "volumeUp" || a.action == "volumeDown") {
            const int steps = a.action == "volumeUp" ? 1 : -1;
            if (target == "master") {
                const std::optional<int> cur = backend->masterPercent();
                if (!cur)
                    text = zh ? "无法读取系统音量" : "Cannot read the system volume";
                else {
                    const int next = tv::steppedPercent(*cur, steps, settings.percentPerStep);
                    text = backend->setMasterPercent(next)
                        ? (zh ? "系统音量 · " : "System volume · ") + std::to_string(next) + "%"
                        : (zh ? "调整系统音量失败" : "Failed to change the system volume");
                }
            } else if (target == "session" && pid != 0) {
                const std::optional<int> cur = backend->sessionPercent(pid);
                if (!cur)
                    text = zh ? "该应用没有音频会话" : "This app has no audio session";
                else {
                    const int next = tv::steppedPercent(*cur, steps, settings.percentPerStep);
                    text = backend->setSessionPercent(pid, next)
                        ? (zh ? "音量 · " : "Volume · ") + std::to_string(next) + "%"
                        : (zh ? "调整失败" : "Failed to change the volume");
                }
            }
        }

        if (text.empty())
            return { effect::notify(zh ? "不支持的操作" : "Unsupported action"), effect::hide() };
        return { effect::notify(text), effect::hide() };
    }

    void settingsChanged(const Json& s) override
    {
        applySettings(tv::parseSettings(s));
    }

private:
    void applySettings(const tv::Settings& in)
    {
        tv::Settings settings = in;
        settings.chinese = chinese();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_settings = settings;
        }
        m_worker.applySettings(settings);

        // Start/stop threads outside the lock: stop() joins the hook thread,
        // and that thread takes m_mutex in dispatchMouse. Holding it here
        // would deadlock.
        if (settings.enableMouse) {
            m_resolver.start();
            if (!m_hook.active()) {
                m_hook.start(
                    [this] { return m_resolver.snapshot(); },
                    [](const tv::MouseDecision& d, const tv::MouseEvent&, DWORD pid,
                        const std::wstring& title) {
                        if (s_instance)
                            s_instance->dispatchMouse(d, pid, title);
                    });
            }
        } else {
            m_hook.stop();
            m_resolver.stop();
        }
    }

    // One COM backend for the protocol thread (query/action); creating one
    // per call re-ran CoInitializeEx and device enumeration on every request.
    tv::IAudioBackend* protocolBackend()
    {
        if (!m_protocolBackend) {
            m_protocolBackend = tv::createBackend();
            if (m_protocolBackend && !m_protocolBackend->init())
                m_protocolBackend.reset(); // retried on the next request
        }
        return m_protocolBackend.get();
    }

    static void enablePerMonitorDpi()
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (const HMODULE u32 = ::GetModuleHandleW(L"user32.dll")) {
            if (const auto fn = reinterpret_cast<SetCtxFn>(
                    ::GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                if (fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
                    return;
            }
        }
        ::SetProcessDPIAware();
    }

    void dispatchMouse(const tv::MouseDecision& d, DWORD pid, const std::wstring& title)
    {
        tv::Settings settings;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            settings = m_settings;
        }

        tv::VolumeOp op;
        switch (d.action) {
        case tv::MouseActionKind::AppWheel:
            op.kind = tv::VolumeOp::Kind::AppWheel;
            op.pid = pid;
            op.steps = d.steps;
            op.buttonTitle = title;
            break;
        case tv::MouseActionKind::MasterWheel:
            op.kind = tv::VolumeOp::Kind::MasterWheel;
            op.steps = d.steps;
            op.buttonTitle = title;
            break;
        case tv::MouseActionKind::AppClickToggle:
            op.kind = tv::VolumeOp::Kind::AppClickToggle;
            op.pid = pid;
            op.buttonTitle = title;
            break;
        case tv::MouseActionKind::MasterClickToggle:
            op.kind = tv::VolumeOp::Kind::MasterClickToggle;
            op.buttonTitle = title;
            break;
        case tv::MouseActionKind::Blocked:
            op.kind = tv::VolumeOp::Kind::BlockedReason;
            op.reason = d.reason;
            op.buttonTitle = title;
            break;
        default:
            return;
        }
        op.percentPerStep = settings.percentPerStep;
        m_worker.post(std::move(op));
    }

    static VolumePlugin* s_instance;

    std::mutex m_mutex;
    tv::Settings m_settings;
    std::unique_ptr<tv::IAudioBackend> m_protocolBackend; // protocol thread only
    tv::AudioWorker m_worker;
    tv::TaskbarResolver m_resolver;
    tv::MouseHook m_hook;
};

VolumePlugin* VolumePlugin::s_instance = nullptr;

int main()
{
    VolumePlugin plugin;
    return plugin.serve();
}
