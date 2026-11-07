// audio_worker.cpp — the only thread that touches Core Audio after startup.
#include "audio_worker.h"
#include "overlay.h"
#include "winshun.h"

#include <objbase.h>

#include <chrono>
#include <memory>

namespace tv {

namespace {
constexpr UINT kOpMessage = WM_APP + 1;
} // namespace

bool AudioWorker::start()
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_ready = false;
    }
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    // Wait until the worker owns a message queue, so the very first post()
    // cannot be dropped by PostThreadMessageW.
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_startCv.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_ready; });
    return true;
}

void AudioWorker::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

void AudioWorker::post(VolumeOp op)
{
    if (!m_running.load())
        return;
    auto* copy = new VolumeOp(std::move(op));
    if (!::PostThreadMessageW(m_threadId, kOpMessage, 0,
            reinterpret_cast<LPARAM>(copy))) {
        delete copy; // the thread died before its queue existed
    }
}

void AudioWorker::applySettings(const Settings& s)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = s;
}

void AudioWorker::run()
{
    m_threadId = ::GetCurrentThreadId();
    MSG msg;
    // Force the thread message queue into existence before signaling ready.
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_ready = true;
    }
    m_startCv.notify_all();

    const HRESULT cohr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::unique_ptr<IAudioBackend> backend = createBackend();
    if (!backend || !backend->init())
        winshun::debug("TaskbarVolume: Core Audio unavailable on the worker thread");

    Overlay overlay;
    if (!overlay.create())
        winshun::debug("TaskbarVolume: overlay window creation failed");

    while (m_running.load()) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0)
            break;
        if (msg.message != kOpMessage || !msg.lParam) {
            // The overlay window lives on this thread: WM_PAINT and its hide
            // timer must reach the window procedure, or nothing is drawn.
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            continue;
        }
        std::unique_ptr<VolumeOp> op(reinterpret_cast<VolumeOp*>(msg.lParam));
        if (!backend) {
            winshun::debug("TaskbarVolume: dropping volume op, audio unavailable");
            continue; // the unique_ptr frees the op
        }

        Settings settings;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            settings = m_settings;
        }

        const VolumeFeedback fb = executeVolumeOp(*backend, *op, settings.chinese);
        if (settings.showOverlay && overlay.valid()) {
            const OverlayPlacement placement {
                settings.overlayAnchor, settings.overlayOffsetX, settings.overlayOffsetY
            };
            if (fb.hasValue)
                overlay.showValue(fb.label, fb.percent, placement, settings.overlayMs);
            else if (!fb.message.empty())
                overlay.show(fb.message, placement, settings.overlayMs);
        }
        if (!fb.ok && !fb.message.empty())
            winshun::debug("TaskbarVolume: op failed: " + winshun::utf8(fb.message));
    }

    // WM_QUIT leaves queued ops behind; free them instead of leaking the
    // heap copies posted by the hook thread.
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == kOpMessage && msg.lParam)
            delete reinterpret_cast<VolumeOp*>(msg.lParam);
    }

    overlay.destroy();
    if (SUCCEEDED(cohr))
        ::CoUninitialize();
    m_running.store(false);
}

} // namespace tv
