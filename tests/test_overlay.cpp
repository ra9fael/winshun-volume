// test_overlay.cpp — the pure placement math of the overlay: anchors, offsets,
// clamping into the work area, and the clearance that keeps a bottom-anchored
// panel above the strip the taskbar draws its hover tooltips into. No window,
// no COM.
#include "overlay.h"
#include "test_harness.h"

#include <windows.h>

using namespace tv;

namespace {

// A 2560x1440 monitor whose taskbar (48 px) sits at the bottom edge.
constexpr RECT kWork { 0, 0, 2560, 1392 };
constexpr RECT kMonitor { 0, 0, 2560, 1440 };
constexpr int kW = 220;
constexpr int kH = 76;

RECT place(OverlayAnchor anchor, int ox = 0, int oy = 0, POINT cursor = { 1280, 700 },
    RECT work = kWork, RECT monitor = kMonitor)
{
    return overlayRect(anchor, ox, oy, kW, kH, cursor, work, monitor);
}

bool eq(RECT a, RECT b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

} // namespace

TEST("overlay: anchors place the panel inside the work area")
{
    CHECK(eq(place(OverlayAnchor::Center),
        RECT { 1170, 658, 1390, 734 }));
    CHECK(eq(place(OverlayAnchor::BottomRight),
        RECT { 2334, 1310, 2554, 1386 }));
    CHECK(eq(place(OverlayAnchor::BottomLeft),
        RECT { 6, 1310, 226, 1386 }));
    CHECK(eq(place(OverlayAnchor::TopRight),
        RECT { 2334, 6, 2554, 82 }));
}

TEST("overlay: bottom placements stay out of the taskbar's hover band")
{
    // The tray tooltip and button previews are drawn just above the taskbar;
    // a panel landing in that strip stops one small gap above the work area's
    // bottom edge and is kept topmost by the overlay, so it covers the part
    // that matters instead of dodging the whole strip.
    const RECT r = place(OverlayAnchor::BottomRight);
    CHECK_EQ(r.bottom, 1386); // 6 px above the taskbar top
    CHECK_EQ(r.right, 2554);  // 6 px from the right edge

    // The follow-mouse anchor gets the same treatment: a cursor sitting on
    // the taskbar itself would otherwise leave the panel floating above it.
    const RECT m = place(OverlayAnchor::Mouse, 0, 0, { 2454, 1416 });
    CHECK_EQ(m.bottom, 1386);

    // Bottom-center too; top placements are not touched.
    CHECK_EQ(place(OverlayAnchor::BottomCenter).bottom, 1386);
    CHECK_EQ(place(OverlayAnchor::TopRight).bottom, 82);
}

TEST("overlay: a monitor without a bottom taskbar keeps the plain margin")
{
    const RECT noTaskbar { 0, 0, 2560, 1440 };
    const RECT r = place(OverlayAnchor::BottomRight, 0, 0, { 1280, 700 }, noTaskbar, noTaskbar);
    CHECK_EQ(r.bottom, 1440 - 6);
}

TEST("overlay: offsets move the panel but cannot drop it into the band")
{
    const RECT pushed = place(OverlayAnchor::BottomRight, 0, 60);
    CHECK_EQ(pushed.bottom, 1386); // pulled back up above the taskbar

    const RECT nudged = place(OverlayAnchor::BottomRight, -40, 0);
    CHECK_EQ(nudged.right, 2514);
    CHECK_EQ(nudged.bottom, 1386);
}

TEST("overlay: panel clamps at the screen edges")
{
    // A cursor near the left edge with the follow-mouse anchor: the panel is
    // pulled back into the monitor.
    const RECT left = place(OverlayAnchor::Mouse, 0, 0, { 5, 700 });
    CHECK_EQ(left.left, 0);

    // An anchor pushed past the bottom of a band-less monitor clamps too.
    const RECT noTaskbar { 0, 0, 2560, 1440 };
    const RECT low = place(OverlayAnchor::BottomRight, 0, 200, { 1280, 700 }, noTaskbar, noTaskbar);
    CHECK_EQ(low.bottom, 1440);
}

TEST("overlay: a small monitor still fits the panel above the band")
{
    // A band that would push the panel past the top edge wins only until the
    // panel no longer fits: clamp to the work area's bottom.
    const RECT work { 0, 0, 800, 120 };
    const RECT monitor { 0, 0, 800, 160 };
    const RECT r = overlayRect(OverlayAnchor::BottomRight, 0, 0, kW, kH, { 400, 100 },
        work, monitor);
    // The band is 40 px: the panel keeps its gap above the taskbar.
    CHECK_EQ(r.top, 38);
    CHECK_EQ(r.bottom, 114);
}
