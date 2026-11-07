// VolumeTestTarget — an E2E test helper for the TaskbarVolume plugin.
//
// Creates a visible top-level window with a unique title (so it appears on
// the taskbar) and a real WASAPI render session playing silence, all inside
// this one process. The plugin's resolver can then be verified against a
// controlled target instead of a human's desktop apps.
//
// Usage: VolumeTestTarget.exe "<window title>"
// stdin commands (one per line):
//   get   -> prints one JSON line: {"pid":N,"volume":P,"muted":B}
//   quit  -> exits
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct State {
    ISimpleAudioVolume* simple = nullptr;
    HWND hwnd = nullptr;
    std::wstring title;
};

State g_state;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool createWindow(const std::wstring& title)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VolumeTestTargetClass";
    if (!::RegisterClassW(&wc))
        return false;
    g_state.hwnd = ::CreateWindowW(wc.lpszClassName, title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 320, 200,
        nullptr, nullptr, wc.hInstance, nullptr);
    g_state.title = title;
    return g_state.hwnd != nullptr;
}

// A render client streaming silence: enough to create a real, active audio
// session for this process without producing audible output.
bool createSilenceSession()
{
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))))
        return false;
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        enumerator->Release();
        return false;
    }
    IAudioClient* client = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&client)))) {
        device->Release();
        enumerator->Release();
        return false;
    }
    WAVEFORMATEX* fmt = nullptr;
    bool ok = false;
    if (SUCCEEDED(client->GetMixFormat(&fmt)) && fmt) {
       REFERENCE_TIME bufferDuration = 10'000'000; // 1 s
        if (SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, fmt, nullptr))) {
            IAudioSessionManager2* manager = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&manager))) && manager) {
                IAudioSessionControl* control = nullptr;
                if (SUCCEEDED(manager->GetAudioSessionControl(nullptr, 0, &control)) && control) {
                    ok = SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                        reinterpret_cast<void**>(&g_state.simple)));
                    control->Release();
                }
                manager->Release();
            }
            if (ok)
                client->Start(); // silence: the format has all-zero buffers
        }
        ::CoTaskMemFree(fmt);
    }
    client->Release();
    device->Release();
    enumerator->Release();
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: VolumeTestTarget <window title>\n");
        return 2;
    }
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::fprintf(stderr, "CoInitializeEx failed\n");
        return 1;
    }
    if (!createWindow(argv[1]) || !createSilenceSession()) {
        std::fprintf(stderr, "setup failed (window=%d session=%d)\n",
            g_state.hwnd != nullptr, g_state.simple != nullptr);
        return 1;
    }

    char line[256];
    while (std::fgets(line, sizeof line, stdin)) {
        if (std::strncmp(line, "get", 3) == 0) {
            float v = 0.f;
            BOOL muted = FALSE;
            if (g_state.simple) {
                g_state.simple->GetMasterVolume(&v);
                g_state.simple->GetMute(&muted);
            }
            std::printf("{\"pid\":%lu,\"volume\":%d,\"muted\":%s}\n",
                static_cast<unsigned long>(::GetCurrentProcessId()),
                static_cast<int>(v * 100.f + 0.5f), muted ? "true" : "false");
            std::fflush(stdout);
        } else if (std::strncmp(line, "quit", 4) == 0) {
            break;
        }
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return 0;
}
