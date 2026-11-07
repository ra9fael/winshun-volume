// overlay.h — a tiny non-activating window that flashes the result
// ("Chrome · 35%") near the mouse pointer. Lives on the audio worker thread.
#pragma once

#include <windows.h>

#include <string>

namespace tv {

// Where the overlay goes. Nine fixed anchors on the monitor under the
// pointer, plus the old follow-the-mouse mode (kept as an option; the
// taskbar's hover preview can cover it).
enum class OverlayAnchor {
    Center,
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    Mouse,
};

struct OverlayPlacement {
    OverlayAnchor anchor = OverlayAnchor::Center;
    int offsetX = 0; // pixels from the anchor, applied after positioning
    int offsetY = 0;
};

class Overlay {
public:
    Overlay() = default;
    ~Overlay() { destroy(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Creates the window; must be called from the owning thread.
    bool create();
    void destroy();
    bool valid() const { return m_hwnd != nullptr; }

    // Shows a one-line message for `ms` milliseconds, then hides itself.
    void show(const std::wstring& text, const OverlayPlacement& placement, int ms);

    // Shows a large percentage with the app label and a progress bar, for
    // fine adjustment while scrolling.
    void showValue(const std::wstring& label, int percent, const OverlayPlacement& placement, int ms);

private:
    static LRESULT CALLBACK procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void present(const OverlayPlacement& placement, int ms);
    void paint();
    static void registerClassOnce();

    HWND m_hwnd = nullptr;
    int m_durationMs = 600;
    bool m_valueMode = false;
    std::wstring m_text;
    std::wstring m_label;
    int m_percent = 0;
};

} // namespace tv
