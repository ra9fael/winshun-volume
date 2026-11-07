// audio_worker.h — a message-loop thread that owns the Core Audio backend
// and the overlay window. The hook never blocks on audio work: it posts
// VolumeOps here and returns.
#pragma once

#include "audio_controller.h"
#include "settings.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace tv {

class AudioWorker {
public:
    AudioWorker() = default;
    ~AudioWorker() { stop(); }
    AudioWorker(const AudioWorker&) = delete;
    AudioWorker& operator=(const AudioWorker&) = delete;

    bool start();
    void stop();
    bool running() const { return m_running.load(); }

    // Thread-safe; the op is heap-copied and delivered via thread message.
    void post(VolumeOp op);
    // Thread-safe settings update (percentPerStep, showOverlay, overlayMs).
    void applySettings(const Settings& s);

private:
    void run();

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    std::mutex m_mutex;
    Settings m_settings;
    // Handshake so the first post() cannot race the worker's message queue.
    std::mutex m_startMutex;
    std::condition_variable m_startCv;
    bool m_ready = false;
};

} // namespace tv
