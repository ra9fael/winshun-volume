// overlay.cpp — non-activating tooltip-style volume indicator.
#include "overlay.h"

#include <algorithm>

namespace tv {

namespace {

constexpr wchar_t kClassName[] = L"TaskbarVolumeOverlay";
// One width for both modes: the tray's own tooltip card is right-aligned to
// the screen edge and about 300 px wide (a longer state line widens it), so a
// narrower panel lets its left end show next to the panel. 340 covers the
// measured cards with room to spare, and a single width keeps the panel from
// changing size between a wheel step and a mute toggle.
constexpr int kWidth = 340;
constexpr int kHeight = 76;     // value mode: label, big percentage, bar
constexpr int kTextHeight = 72; // message mode: one or two lines
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT_PTR kRaiseTimer = 2;

HFONT makeFont(int height, int weight)
{
    return ::CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

} // namespace

void Overlay::registerClassOnce()
{
    static const bool done = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Overlay::procStatic;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        return ::RegisterClassW(&wc) != 0; // ERROR_CLASS_ALREADY_EXISTS is fine
    }();
    (void)done;
}

bool Overlay::create()
{
    if (m_hwnd)
        return true;
    registerClassOnce();

    m_hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClassName, L"", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this); // arrives as lpCreateParams
    return m_hwnd != nullptr;
}

void Overlay::destroy()
{
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Overlay::show(const std::wstring& label, const std::wstring& text,
    const OverlayPlacement& placement, int ms)
{
    if (!m_hwnd || (text.empty() && label.empty()))
        return;
    m_valueMode = false;
    m_label = label;
    m_text = text;
    present(placement, ms);
}

void Overlay::showValue(const std::wstring& label, int percent, const OverlayPlacement& placement, int ms)
{
    if (!m_hwnd)
        return;
    m_valueMode = true;
    m_label = label;
    m_percent = std::clamp(percent, 0, 100);
    present(placement, ms);
}

// Places the panel on the monitor the pointer is on (geometry in overlayRect).
void Overlay::present(const OverlayPlacement& placement, int ms)
{
    m_durationMs = ms;

    POINT cursor;
    ::GetCursorPos(&cursor);

    RECT work { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi { sizeof mi };
    if (::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi))
        work = mi.rcWork;

    const int w = kWidth;
    const int h = m_valueMode ? kHeight : kTextHeight;
    const RECT rc = overlayRect(placement.anchor, placement.offsetX, placement.offsetY,
        w, h, cursor, work);

    ::SetWindowPos(m_hwnd, HWND_TOPMOST, rc.left, rc.top, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::SetTimer(m_hwnd, kHideTimer, static_cast<UINT>(ms), nullptr);
    m_hideAt = ::GetTickCount64() + static_cast<ULONGLONG>(ms);
    ::SetTimer(m_hwnd, kRaiseTimer, 30, nullptr);
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

void Overlay::paint()
{
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(m_hwnd, &ps);
    RECT rc;
    ::GetClientRect(m_hwnd, &rc);

    // Dark rounded panel, light text — readable over both light and dark
    // taskbars, independent of the host theme.
    HBRUSH bg = ::CreateSolidBrush(RGB(32, 32, 36));
    HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, 12, 12);
    ::FillRgn(dc, rgn, bg);
    ::DeleteObject(rgn);
    ::DeleteObject(bg);

    ::SetBkMode(dc, TRANSPARENT);

    if (!m_valueMode) {
        ::SetBkMode(dc, TRANSPARENT);
        if (m_label.empty()) {
            // One-line message ("无法读取系统音量"): centred, sized to fit.
            ::SetTextColor(dc, RGB(240, 240, 240));
            HFONT font = makeFont(15, FW_SEMIBOLD);
            HFONT old = static_cast<HFONT>(::SelectObject(dc, font));
            RECT textRect { rc.left + 12, rc.top + 8, rc.right - 12, rc.bottom - 8 };
            // Long reasons ("Microsoft Edge · 没有音频会话") need two lines;
            // measure first so a single line can still be centred vertically.
            RECT measure = textRect;
            ::DrawTextW(dc, m_text.c_str(), -1, &measure,
                DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            const int textHeight = measure.bottom - measure.top;
            const int room = textRect.bottom - textRect.top;
            if (textHeight > 0 && textHeight < room)
                textRect.top += (room - textHeight) / 2;
            ::DrawTextW(dc, m_text.c_str(), -1, &textRect,
                DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            ::SelectObject(dc, old);
            ::DeleteObject(font);
            ::EndPaint(m_hwnd, &ps);
            return;
        }
        // Two-line message: what was adjusted on top, the state change below.
        ::SetTextColor(dc, RGB(198, 198, 208));
        HFONT small = makeFont(13, FW_NORMAL);
        HFONT old = static_cast<HFONT>(::SelectObject(dc, small));
        RECT labelRect { rc.left + 12, rc.top + 8, rc.right - 12, rc.top + 28 };
        ::DrawTextW(dc, m_label.c_str(), -1, &labelRect,
            DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SelectObject(dc, old);
        ::DeleteObject(small);

        ::SetTextColor(dc, RGB(240, 240, 240));
        HFONT font = makeFont(15, FW_SEMIBOLD);
        old = static_cast<HFONT>(::SelectObject(dc, font));
        RECT textRect { rc.left + 12, rc.top + 28, rc.right - 12, rc.bottom - 8 };
        RECT measure = textRect;
        ::DrawTextW(dc, m_text.c_str(), -1, &measure,
            DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        const int textHeight = measure.bottom - measure.top;
        const int room = textRect.bottom - textRect.top;
        if (textHeight > 0 && textHeight < room)
            textRect.top += (room - textHeight) / 2;
        ::DrawTextW(dc, m_text.c_str(), -1, &textRect,
            DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::DeleteObject(font);
        ::EndPaint(m_hwnd, &ps);
        return;
    }

    // Small app label at the top.
    ::SetTextColor(dc, RGB(198, 198, 208));
    HFONT small = makeFont(13, FW_NORMAL);
    HFONT old = static_cast<HFONT>(::SelectObject(dc, small));
    RECT labelRect { rc.left + 12, rc.top + 8, rc.right - 12, rc.top + 26 };
    ::DrawTextW(dc, m_label.c_str(), -1, &labelRect,
        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(dc, old);
    ::DeleteObject(small);

    // Big percentage.
    ::SetTextColor(dc, RGB(245, 245, 248));
    HFONT big = makeFont(38, FW_BOLD);
    old = static_cast<HFONT>(::SelectObject(dc, big));
    RECT valueRect { rc.left, rc.top + 22, rc.right, rc.top + 64 };
    const std::wstring value = std::to_wstring(m_percent) + L"%";
    ::DrawTextW(dc, value.c_str(), -1, &valueRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::DeleteObject(big);

    // Progress bar along the bottom.
    const int trackLeft = rc.left + 14;
    const int trackRight = rc.right - 14;
    const int trackTop = rc.bottom - 10;
    const int trackBottom = rc.bottom - 7;
    HBRUSH track = ::CreateSolidBrush(RGB(58, 58, 64));
    RECT trackRect { trackLeft, trackTop, trackRight, trackBottom };
    ::FillRect(dc, &trackRect, track);
    ::DeleteObject(track);

    if (m_percent > 0) {
        const int fillRight = trackLeft
            + static_cast<int>((trackRight - trackLeft) * (m_percent / 100.0));
        HBRUSH fill = ::CreateSolidBrush(RGB(122, 170, 255));
        RECT fillRect { trackLeft, trackTop, std::max(trackLeft + 1, fillRight), trackBottom };
        ::FillRect(dc, &fillRect, fill);
        ::DeleteObject(fill);
    }

    ::EndPaint(m_hwnd, &ps);
}

LRESULT CALLBACK Overlay::procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Overlay*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->proc(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_TIMER:
        if (wp == kHideTimer) {
            // Resetting the timer with SetTimer does not promise that a
            // WM_TIMER already queued for the old deadline is withdrawn; one
            // arriving late would hide a panel that a fresh wheel step just
            // updated. Hide only once the recorded deadline really passed,
            // otherwise re-arm for the remaining time.
            const ULONGLONG now = ::GetTickCount64();
            if (now >= m_hideAt) {
                ::KillTimer(hwnd, kHideTimer);
                ::KillTimer(hwnd, kRaiseTimer);
                ::ShowWindow(hwnd, SW_HIDE);
            } else {
                ::SetTimer(hwnd, kHideTimer,
                    static_cast<UINT>(m_hideAt - now), nullptr);
            }
        } else if (wp == kRaiseTimer) {
            // Shell tooltips (the tray volume bubble in particular) are
            // topmost too and jump above this panel when they appear. While
            // the panel is visible, keep claiming the top of the topmost
            // band so it covers them.
            if (::IsWindowVisible(hwnd)) {
                ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace tv
