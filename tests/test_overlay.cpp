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
        RECT { 2316, 1268, 2536, 1344 })); // lifted above the hover band
    CHECK(eq(place(OverlayAnchor::BottomLeft),
        RECT { 24, 1268, 244, 1344 }));
    CHECK(eq(place(OverlayAnchor::TopRight),
        RECT { 2316, 24, 2536, 100 }));
}

TEST("overlay: bottom placements clear the taskbar's hover band")
{
    // The tray tooltip and button previews are drawn just above the taskbar;
    // their band is one taskbar height. The panel's bottom edge must sit above
    // it, so bottom-right lands 48 px higher than the plain margin.
    const RECT r = place(OverlayAnchor::BottomRight);
    CHECK_EQ(r.bottom, 1344); // kWork.bottom - 48
    CHECK_EQ(r.right, 2536);  // horizontal placement unchanged

    // The follow-mouse anchor clamps against the same band: a cursor sitting
    // on the taskbar itself would otherwise leave the panel flush with it.
    const RECT m = place(OverlayAnchor::Mouse, 0, 0, { 2454, 1416 });
    CHECK_EQ(m.bottom, 1344);

    // Bottom-center is lifted as well; top placements are not touched.
    CHECK_EQ(place(OverlayAnchor::BottomCenter).bottom, 1344);
    CHECK_EQ(place(OverlayAnchor::TopRight).bottom, 100);
}

TEST("overlay: a monitor without a bottom taskbar keeps the plain margin")
{
    const RECT noTaskbar { 0, 0, 2560, 1440 };
    const RECT r = place(OverlayAnchor::BottomRight, 0, 0, { 1280, 700 }, noTaskbar, noTaskbar);
    CHECK_EQ(r.bottom, 1440 - 24);
}

TEST("overlay: offsets move the panel but cannot put it back into the band")
{
    const RECT pushed = place(OverlayAnchor::BottomRight, 0, 60);
    CHECK_EQ(pushed.bottom, 1344); // the downward offset is clamped away

    const RECT nudged = place(OverlayAnchor::BottomRight, -40, 0);
    CHECK_EQ(nudged.right, 2496);
    CHECK_EQ(nudged.bottom, 1344);
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
    // panel no longer fits: clamp to the work area's top.
    const RECT work { 0, 0, 800, 120 };
    const RECT monitor { 0, 0, 800, 160 };
    const RECT r = overlayRect(OverlayAnchor::BottomRight, 0, 0, kW, kH, { 400, 100 },
        work, monitor);
    // The band is 40 px and the panel 76 px: 120 - 40 - 76 = 4, so it starts
    // four pixels below the top and covers the rest of the work area.
    CHECK_EQ(r.top, 4);
    CHECK_EQ(r.bottom, 80);
}
