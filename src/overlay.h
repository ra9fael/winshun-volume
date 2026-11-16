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

// Computes the panel rectangle for one presentation. `work` is the work area of
// the monitor the pointer is on.
//
// A bottom taskbar draws hover tooltips and the tray's own volume bubble just
// above itself. The panel covers that strip instead of dodging it: placements
// sit flush on the work area's edges, so a bottom-right panel reaches the
// screen's right edge and the taskbar's top edge, and present() keeps
// re-asserting topmost so the shell's bubble stays underneath. An offset moves
// the panel anywhere else when the default placement is not wanted.
inline RECT overlayRect(OverlayAnchor anchor, int offsetX, int offsetY, int width, int height,
    POINT cursor, RECT work)
{
    const int midX = work.left + (work.right - work.left) / 2;
    const int midY = work.top + (work.bottom - work.top) / 2;
    int x = midX - width / 2;
    int y = midY - height / 2;

    switch (anchor) {
    case OverlayAnchor::TopLeft:
        x = work.left;
        y = work.top;
        break;
    case OverlayAnchor::TopCenter:
        x = midX - width / 2;
        y = work.top;
        break;
    case OverlayAnchor::TopRight:
        x = work.right - width;
        y = work.top;
        break;
    case OverlayAnchor::MiddleLeft:
        x = work.left;
        y = midY - height / 2;
        break;
    case OverlayAnchor::MiddleRight:
        x = work.right - width;
        y = midY - height / 2;
        break;
    case OverlayAnchor::BottomLeft:
        x = work.left;
        y = work.bottom - height;
        break;
    case OverlayAnchor::BottomCenter:
        x = midX - width / 2;
        y = work.bottom - height;
        break;
    case OverlayAnchor::BottomRight:
        x = work.right - width;
        y = work.bottom - height;
        break;
    case OverlayAnchor::Mouse:
        x = cursor.x - width / 2;
        y = cursor.y - height - 12;
        if (y < work.top)
            y = cursor.y + 24;
        break;
    case OverlayAnchor::Center:
    default:
        break;
    }

    x += offsetX;
    y += offsetY;

    if (x + width > work.right)
        x = work.right - width;
    if (x < work.left)
        x = work.left;
    if (y + height > work.bottom)
        y = work.bottom - height;
    if (y < work.top)
        y = work.top;
    return RECT { x, y, x + width, y + height };
}

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

    // Shows a status for `ms` milliseconds, then hides itself: the label above
    // the state, or a single centred line when there is no label.
    void show(const std::wstring& label, const std::wstring& text,
        const OverlayPlacement& placement, int ms);

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
    ULONGLONG m_hideAt = 0; // tick the current presentation hides at
    bool m_valueMode = false;
    std::wstring m_text;
    std::wstring m_label;
    int m_percent = 0;
};

} // namespace tv
