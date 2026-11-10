// taskbar_resolver.cpp — UI Automation taskbar snapshot builder.
//
// Refresh strategy: a dedicated thread re-enumerates the taskbars once at
// startup, on a slow fallback timer (1 s), and shortly after window/taskbar
// changes reported by SetWinEventHook. The events are what keep a closed app
// from leaving stale button rectangles around for up to a second; the
// fallback timer only catches anything the events miss.
//
// Mapping strategy: the visible top-level windows are collected once per
// refresh (title, executable path, explicit AUMID), then each taskbar button
// is mapped from its AutomationId ("Appid: MSEdge") and accessibility name.
// See resolveButtonTarget() in mouse_logic.cpp.
#include "taskbar_resolver.h"

#include "winshun.h"

#include <appmodel.h>
#include <dwmapi.h>
// UIAutomationCore.h's interfaces need the OLE accessibility declarations in
// place first, or the SDK header fails to parse.
#include <oleacc.h>
#include <uiautomationclient.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace tv {

namespace {

constexpr UINT kTimerMs = 1000; // fallback cadence
constexpr UINT kDebounceMs = 180; // settle after the last event
constexpr ULONGLONG kMaxDelayMs = 600; // a constant event stream must not starve
constexpr ULONGLONG kHealMs = 5000; // retry a lost WinEvent hook / a failed UIA init

bool isTaskbarRoot(HWND hwnd)
{
    if (!hwnd)
        return false;
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (!root)
        return false;
    wchar_t cls[64] = {};
    if (::GetClassNameW(root, cls, static_cast<int>(sizeof cls / sizeof cls[0])) == 0)
        return false;
    const std::wstring name(cls);
    return name == L"Shell_TrayWnd" || name == L"Shell_SecondaryTrayWnd";
}

bool windowVisibleUncloaked(HWND hwnd)
{
    if (!::IsWindowVisible(hwnd))
        return false;
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)
        return false;
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd)
        return false;
    // Skip UWP ghosts: cloaked ApplicationFrame windows are invisible on the
    // desktop and must not win a match.
    int cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0)
        return false;
    return true;
}

std::wstring exePathOf(DWORD pid)
{
    std::wstring path;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return path;
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(sizeof buffer / sizeof buffer[0]);
    if (::QueryFullProcessImageNameW(h, 0, buffer, &size) && size > 0)
        path.assign(buffer, size);
    ::CloseHandle(h);
    return path;
}

// The process's explicit AppUserModelID ("MSEdge", "ByteDance.TraeCN",
// "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"). Empty for unpackaged
// processes that never set one; those are matched by executable path instead.
std::wstring appIdOf(DWORD pid)
{
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t buffer[512] = {};
    UINT32 length = static_cast<UINT32>(sizeof buffer / sizeof buffer[0]);
    std::wstring appId;
    if (::GetApplicationUserModelId(h, &length, buffer) == ERROR_SUCCESS)
        appId = buffer;
    ::CloseHandle(h);
    return appId;
}

// All visible top-level windows with a title, plus the owning process data.
// Process properties are fetched once per pid, not once per window.
std::vector<WindowInfo> collectWindows()
{
    struct ProcessData {
        std::wstring exePath;
        std::wstring appId;
    };
    std::vector<WindowInfo> out;
    std::map<DWORD, ProcessData> cache;
    struct Ctx {
        std::vector<WindowInfo>* out;
        std::map<DWORD, ProcessData>* cache;
    } ctx { &out, &cache };

    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx& c = *reinterpret_cast<Ctx*>(lp);
        if (!windowVisibleUncloaked(hwnd))
            return TRUE;
        const int len = ::GetWindowTextLengthW(hwnd);
        if (len <= 0)
            return TRUE;
        std::wstring title(static_cast<std::size_t>(len) + 1, L'\0');
        const int got = ::GetWindowTextW(hwnd, title.data(), len + 1);
        if (got <= 0)
            return TRUE;
        title.resize(static_cast<std::size_t>(got));

        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0)
            return TRUE;

        auto it = c.cache->find(pid);
        if (it == c.cache->end()) {
            ProcessData data;
            data.exePath = exePathOf(pid);
            data.appId = appIdOf(pid);
            it = c.cache->emplace(pid, std::move(data)).first;
        }

        WindowInfo w;
        w.title = std::move(title);
        w.pid = pid;
        w.exePath = it->second.exePath;
        w.appId = it->second.appId;
        c.out->push_back(std::move(w));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return out;
}

// The UIA AutomationId of a taskbar button is "Appid: <AppUserModelID>".
std::wstring automationAppId(IUIAutomationElement* el)
{
    BSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(el->get_CurrentAutomationId(&id)) && id) {
        out = id;
        ::SysFreeString(id);
    }
    constexpr wchar_t kPrefix[] = L"Appid: ";
    if (out.rfind(kPrefix, 0) == 0)
        out.erase(0, sizeof kPrefix / sizeof kPrefix[0] - 1);
    // Trim spaces and slashes Windows sometimes appends.
    while (!out.empty() && (out.front() == L' ' || out.front() == L'\t'))
        out.erase(0, 1);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t'))
        out.pop_back();
    return out;
}

// The tray volume icon's accessible name starts with the localized word for
// "volume" and ends with the live value ("Volume BELLATOR N27Q (…): 65%").
// Windows' UI language is independent of Win顺's locale, so cover the common
// ones; English/Chinese are certain, the rest are best-effort translations.
// This is only the fallback: the button is normally recognized by its class
// name and automation id, which do not depend on the UI language.
bool startsWithVolumeWord(const std::wstring& name)
{
    static const wchar_t* const kWords[] = {
        L"音量", L"Volume", L"Volumen", L"Lautstärke", L"Громкость", L"볼륨",
        L"Głośność", L"Hlasitost", L"Volume:", L"Ses", L"Volym", L"Äänenvoimakkuus",
        L"Hangerő", L"Ses düzeyi", L"Poziom", L"Гучнасць", L"Гучність"
    };
    for (const wchar_t* word : kWords) {
        const std::wstring prefix(word);
        if (!prefix.empty() && name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

// The tray volume button is the one XAML peer Explorer marks as
// "SystemTray.OmniButtonRight". Verified on Windows 11 22H2+ zh-CN and en-US:
// network is SystemTray.AccentButton, the clock SystemTray.OmniButton, the
// overflow arrow and notification icons SystemTray.NormalButton, and the show
// desktop corner SystemTray.ShowDesktopButton.
bool isVolumeButtonClass(const std::wstring& className)
{
    return className == L"SystemTray.OmniButtonRight";
}

bool isTrayButtonClass(const std::wstring& className)
{
    constexpr wchar_t kPrefix[] = L"SystemTray.";
    return className.rfind(kPrefix, 0) == 0;
}

std::string hresultText(HRESULT hr)
{
    char code[16] = {};
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    std::string text(code);
    wchar_t* buffer = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (n && buffer) {
        std::wstring message(buffer, n);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'
                   || message.back() == L' ' || message.back() == L'.'))
            message.pop_back();
        if (!message.empty())
            text += ": " + winshun::utf8(message);
    }
    if (buffer)
        ::LocalFree(buffer);
    return text;
}

// How far the tray volume button's hit area may grow on each side. Measured on
// Windows 11: the peer reports 24 px while the gaps to the network button and
// the clock are 4 px and 8 px, so a strict hit test drops clicks that a remote
// desktop's cursor scaling or a touchpad puts a couple of pixels off. The
// widening never crosses a neighbour (see expandHitRect).
int volumeHitSlop(RECT button)
{
    const int width = static_cast<int>(button.right - button.left);
    const int height = static_cast<int>(button.bottom - button.top);
    return std::max(4, std::min(width, height) / 4);
}

class UiaSession {
public:
    UiaSession() = default;
    ~UiaSession()
    {
        if (m_uia)
            m_uia->Release();
    }
    UiaSession(const UiaSession&) = delete;
    UiaSession& operator=(const UiaSession&) = delete;

    bool init()
    {
        if (m_uia)
            return true;
        const HRESULT hr = ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_ALL,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&m_uia));
        if (FAILED(hr) || !m_uia) {
            m_uia = nullptr;
            // Without UI Automation the snapshot stays empty and every gesture
            // silently passes through, so this failure must be visible.
            if (!m_failureLogged) {
                m_failureLogged = true;
                winshun::debug("TaskbarVolume: UI Automation unavailable (hr=" + hresultText(hr)
                    + "); taskbar gestures are disabled");
            }
            return false;
        }
        m_failureLogged = false;
        return true;
    }

    // Collects taskbar app buttons (Win11 XAML and classic) plus the volume
    // button rect for one taskbar window.
    void collectFromTaskbar(HWND taskbar, TaskbarSnapshot& out, const std::vector<WindowInfo>& windows)
    {
        if (!m_uia || !taskbar)
            return;
        IUIAutomationElement* root = nullptr;
        if (FAILED(m_uia->ElementFromHandle(taskbar, &root)) || !root)
            return;

        const std::size_t before = out.appButtons.size();
        collectAppButtons(root, out, windows);

        // The hit rect must stay inside the taskbar, so use its window bounds;
        // a hidden or off-screen taskbar reports an empty rect and the element
        // bounds are the only fallback.
        RECT taskbarBounds {};
        if (!::GetWindowRect(taskbar, &taskbarBounds)
            || taskbarBounds.right <= taskbarBounds.left || taskbarBounds.bottom <= taskbarBounds.top) {
            taskbarBounds = RECT {};
            if (FAILED(root->get_CurrentBoundingRectangle(&taskbarBounds)))
                taskbarBounds = RECT {};
        }
        collectVolumeButton(root, out, taskbarBounds);

        // Classic taskbar: the buttons live under an MSTaskListWClass child
        // and expose no "Appid:" automation id. Only used when the XAML peers
        // found nothing for this taskbar, so modern buttons are never doubled.
        if (out.appButtons.size() == before) {
            HWND taskList = nullptr;
            while ((taskList = ::FindWindowExW(taskbar, taskList, L"MSTaskListWClass", nullptr)) != nullptr) {
                IUIAutomationElement* listEl = nullptr;
                if (SUCCEEDED(m_uia->ElementFromHandle(taskList, &listEl)) && listEl) {
                    collectLegacyButtons(listEl, out, windows);
                    listEl->Release();
                }
            }
        }
        root->Release();
    }

private:
    void collectAppButtons(IUIAutomationElement* scope, TaskbarSnapshot& out,
        const std::vector<WindowInfo>& windows)
    {
        VARIANT className;
        className.vt = VT_BSTR;
        className.bstrVal = ::SysAllocString(L"Taskbar.TaskListButtonAutomationPeer");
        IUIAutomationCondition* cond = nullptr;
        if (FAILED(m_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, className, &cond))) {
            ::VariantClear(&className);
            return;
        }
        ::VariantClear(&className);

        IUIAutomationElementArray* found = nullptr;
        const HRESULT hr = scope->FindAll(TreeScope_Descendants, cond, &found);
        cond->Release();
        if (FAILED(hr) || !found)
            return;

        int n = 0;
        found->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(found->GetElement(i, &el)) && el) {
                TaskbarButton b = buttonFromElement(el);
                const std::wstring appId = automationAppId(el);
                el->Release();
                if (!b.title.empty() || !appId.empty()) {
                    b.pid = resolveButtonTarget(appId, b.title, windows, b.ambiguous);
                    if (!b.title.empty())
                        out.appButtons.push_back(std::move(b));
                }
            }
        }
        found->Release();
    }

    // Classic toolbar buttons: name is the window title, no automation id.
    void collectLegacyButtons(IUIAutomationElement* scope, TaskbarSnapshot& out,
        const std::vector<WindowInfo>& windows)
    {
        VARIANT buttonType;
        buttonType.vt = VT_I4;
        buttonType.lVal = UIA_ButtonControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        if (FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, &cond)))
            return;

        IUIAutomationElementArray* found = nullptr;
        const HRESULT hr = scope->FindAll(TreeScope_Descendants, cond, &found);
        cond->Release();
        if (FAILED(hr) || !found)
            return;

        int n = 0;
        found->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* el = nullptr;
            if (SUCCEEDED(found->GetElement(i, &el)) && el) {
                TaskbarButton b = buttonFromElement(el);
                el->Release();
                if (!b.title.empty()) {
                    b.pid = resolveButtonTarget(L"", b.title, windows, b.ambiguous);
                    out.appButtons.push_back(std::move(b));
                }
            }
        }
        found->Release();
    }

    // The tray volume button: identified by its Explorer class name, which is
    // language independent, and only falling back to the localized name. The
    // hit rectangle is then widened to cover the dead gap to its neighbours, so
    // a click a few pixels off — normal on a remote desktop or a high-DPI
    // pointer — still lands.
    void collectVolumeButton(IUIAutomationElement* scope, TaskbarSnapshot& out, RECT taskbarBounds)
    {
        VARIANT buttonType;
        buttonType.vt = VT_I4;
        buttonType.lVal = UIA_ButtonControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        if (FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, &cond)))
            return;

        IUIAutomationElementArray* found = nullptr;
        const HRESULT hr = scope->FindAll(TreeScope_Descendants, cond, &found);
        cond->Release();
        if (FAILED(hr) || !found)
            return;

        // Structural match: the class name does not change with the Windows
        // display language, so it wins over the name heuristic. The automation
        // id is only a tiebreaker - a hidden placeholder of the same class would
        // otherwise be picked.
        int bestStructRank = -1; // 1 = OmniButtonRight + SystemTrayIcon, 0 = class only
        long long bestStructArea = -1;
        RECT bestStruct {};
        // Name fallback, used only when no structural candidate exists: the
        // smallest volume-named button is the tray icon, not a window whose
        // title merely contains "Volume".
        long long bestNamedArea = -1;
        RECT bestNamed {};
        std::vector<RECT> trayRects;

        int n = 0;
        found->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(found->GetElement(i, &el)) || !el)
                continue;

            RECT r {};
            const bool usable = SUCCEEDED(el->get_CurrentBoundingRectangle(&r))
                && r.right > r.left && r.bottom > r.top;
            if (usable) {
                std::wstring className;
                std::wstring automationId;
                if (BSTR value = nullptr; SUCCEEDED(el->get_CurrentClassName(&value)) && value) {
                    className.assign(value);
                    ::SysFreeString(value);
                }
                if (BSTR value = nullptr; SUCCEEDED(el->get_CurrentAutomationId(&value)) && value) {
                    automationId.assign(value);
                    ::SysFreeString(value);
                }

                if (isVolumeButtonClass(className)) {
                    const int rank = automationId == L"SystemTrayIcon" ? 1 : 0;
                    const long long area = 1LL * (r.right - r.left) * (r.bottom - r.top);
                    if (rank > bestStructRank || (rank == bestStructRank && (bestStructArea < 0 || area < bestStructArea))) {
                        bestStructRank = rank;
                        bestStructArea = area;
                        bestStruct = r;
                    }
                } else if (isTrayButtonClass(className)) {
                    trayRects.push_back(r); // a neighbour that limits the widening
                }
            }

            if (usable && bestNamedArea < 0) {
                if (BSTR name = nullptr; SUCCEEDED(el->get_CurrentName(&name)) && name) {
                    const std::wstring label(name);
                    ::SysFreeString(name);
                    if (startsWithVolumeWord(label)) {
                        const long long area = 1LL * (r.right - r.left) * (r.bottom - r.top);
                        if (bestNamedArea < 0 || area < bestNamedArea) {
                            bestNamedArea = area;
                            bestNamed = r;
                        }
                    }
                }
            }
            el->Release();
        }
        found->Release();

        if (bestStructArea < 0) {
            if (bestNamedArea < 0)
                return;
            bestStruct = bestNamed;
        }

        out.hasVolume = true;
        out.volumeRect = expandHitRect(bestStruct, taskbarBounds, trayRects, volumeHitSlop(bestStruct));
    }

    static TaskbarButton buttonFromElement(IUIAutomationElement* el)
    {
        TaskbarButton b;
        BSTR name = nullptr;
        if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
            b.title = name;
            ::SysFreeString(name);
        }
        el->get_CurrentBoundingRectangle(&b.rect);
        if (b.rect.right <= b.rect.left || b.rect.bottom <= b.rect.top)
            b.title.clear(); // off-screen / hidden placeholder: unusable
        return b;
    }

    IUIAutomation* m_uia = nullptr;
    bool m_failureLogged = false;
};

TaskbarSnapshot buildSnapshot(UiaSession* uia)
{
    TaskbarSnapshot out;
    const std::vector<WindowInfo> windows = collectWindows();
    if (uia && uia->init()) {
        HWND taskbar = nullptr;
        while ((taskbar = ::FindWindowExW(nullptr, taskbar, L"Shell_TrayWnd", nullptr)) != nullptr)
            uia->collectFromTaskbar(taskbar, out, windows);
        while ((taskbar = ::FindWindowExW(nullptr, taskbar, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
            uia->collectFromTaskbar(taskbar, out, windows);
    }
    return out;
}

} // namespace

TaskbarResolver* TaskbarResolver::s_live = nullptr;

// ---- lifecycle --------------------------------------------------------------------

bool TaskbarResolver::start()
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void TaskbarResolver::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

TaskbarSnapshot TaskbarResolver::snapshot() const
{
    TaskbarSnapshot out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out = m_snapshot;
    }
    // Read after the copy: an event that lands in between marks the copied
    // rectangles stale, and the hook then passes instead of acting on them.
    out.appButtonsStale = m_pending.load(std::memory_order_relaxed);
    return out;
}

void TaskbarResolver::scheduleRefresh(ULONGLONG delayMs)
{
    const ULONGLONG now = ::GetTickCount64();
    if (!m_dirty)
        m_firstEventTick = now;
    m_dirty = true;
    m_lastEventTick = now;
    m_refreshDelay = delayMs;
    m_pending.store(true, std::memory_order_relaxed);
}

void CALLBACK TaskbarResolver::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG, DWORD, DWORD)
{
    TaskbarResolver* self = s_live;
    if (!self || !hwnd)
        return;

    const bool taskbar = isTaskbarRoot(hwnd);
    switch (event) {
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_REORDER:
        // Only the taskbar's own geometry matters here; app windows moving
        // around on screen do not change the button rectangles.
        if (!taskbar)
            return;
        break;
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
        // An app window going away removes its taskbar button.
        if (::GetAncestor(hwnd, GA_ROOT) != hwnd)
            return;
        break;
    case EVENT_OBJECT_CREATE:
        // A new top-level window (a launched app). The SHOW event can arrive
        // before its title is readable, so the creation is the reliable one.
        // Explorer needs a moment to add the button, hence the longer delay.
        // Hidden/untitled helper windows are not taskbar apps and would only
        // flood the debounce.
        if (::GetAncestor(hwnd, GA_ROOT) != hwnd || !::IsWindowVisible(hwnd)
            || ::GetWindowTextLengthW(hwnd) == 0)
            return;
        self->scheduleRefresh(400);
        return;
    case EVENT_OBJECT_SHOW:
        // Menus and tooltips are top-level too; only titled windows are apps.
        if (::GetAncestor(hwnd, GA_ROOT) != hwnd || ::GetWindowTextLengthW(hwnd) == 0)
            return;
        self->scheduleRefresh(400);
        return;
    case EVENT_OBJECT_NAMECHANGE:
        // The taskbar's XAML InputSite child fires NAMECHANGE every second
        // because the clock's accessible name contains the time. Those arrive
        // with a custom XAML peer idObject (measured 0x1307C3), never
        // OBJID_WINDOW or OBJID_CLIENT, and they cannot change any rectangle
        // the resolver builds (it re-enumerates with FindAll, it does not
        // trust the event). Dropping them avoids a rebuild per second for no
        // reason. App titles still fire with OBJID_WINDOW on their own
        // top-level windows, which the branch below keeps.
        if (!taskbar && ::GetAncestor(hwnd, GA_ROOT) != hwnd)
            return;
        if (taskbar && idObject != OBJID_CLIENT && idObject != OBJID_WINDOW)
            return;
        break;
    default:
        return;
    }
    self->scheduleRefresh(kDebounceMs);
}

// ---- thread body -------------------------------------------------------------------

void TaskbarResolver::run()
{
    m_threadId = ::GetCurrentThreadId();
    MSG msg;
    // Force the thread message queue into existence so stop() can post to it.
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const HRESULT cohr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(cohr)) {
        winshun::debug("TaskbarVolume: CoInitializeEx failed (hr=" + hresultText(cohr)
            + "); taskbar gestures are disabled");
        // RPC_E_CHANGED_MODE still leaves a usable COM apartment behind, so
        // keep going: UIA may work even though our own CoUninitialize must not
        // run.
    }

    UiaSession uia;
    uia.init(); // logs its own failure

    auto refresh = [this, &uia] {
        TaskbarSnapshot out = buildSnapshot(&uia);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = std::move(out);
        // The snapshot now reflects every change seen before this pass; any
        // event arriving later will set m_pending again.
        m_pending.store(false, std::memory_order_relaxed);
    };

    // Out-of-context events are delivered to this thread, so the callback and
    // the snapshot live on the same thread.
    s_live = this;
    m_winEvent = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, nullptr,
        &TaskbarResolver::winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!m_winEvent)
        winshun::debug("TaskbarVolume: SetWinEventHook failed; snapshot falls back to the 1 s timer");

    refresh(); // initial snapshot, whatever the COM state

    ULONGLONG lastRefresh = ::GetTickCount64();
    ULONGLONG lastHeal = lastRefresh;
    while (m_running.load()) {
        // Wait for messages (out-of-context WinEvent callbacks are delivered
        // while the thread pumps) or 100 ms, whichever comes first.
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG message;
        bool quit = false;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                break;
            }
            if (message.hwnd) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
        if (quit)
            break;

        const ULONGLONG now = ::GetTickCount64();
        if (now - lastRefresh >= kTimerMs) {
            refresh(); // fallback for anything the events missed
            lastRefresh = now;
        }
        if (m_dirty && ((now - m_lastEventTick >= m_refreshDelay) || (now - m_firstEventTick >= kMaxDelayMs))) {
            m_dirty = false;
            refresh();
            lastRefresh = now;
        }
        // Self-healing: a lost WinEvent hook (Explorer restart, remote-session
        // reattach) or a UIA that only became available later would otherwise
        // leave the plugin gesture-less until the host restarts it.
        if (now - lastHeal >= kHealMs) {
            lastHeal = now;
            if (!m_winEvent) {
                m_winEvent = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, nullptr,
                    &TaskbarResolver::winEventProc, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                if (m_winEvent)
                    winshun::debug("TaskbarVolume: SetWinEventHook re-armed");
            }
            uia.init(); // logs once if it is still unavailable
        }
    }

    if (m_winEvent) {
        ::UnhookWinEvent(m_winEvent);
        m_winEvent = nullptr;
    }
    s_live = nullptr;
    if (SUCCEEDED(cohr))
        ::CoUninitialize();
    m_running.store(false);
}

TaskbarSnapshot TaskbarResolver::refreshNow()
{
    UiaSession uia;
    uia.init();
    TaskbarSnapshot out = buildSnapshot(&uia);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = out;
    }
    return out;
}

} // namespace tv
