// test_overlay.cpp — the pure placement math of the overlay: anchors, offsets,
// and clamping into the work area so a bottom-right panel sits flush on the
// taskbar's top edge and the screen's right edge, covering the strip the
// taskbar draws its hover tooltips into. No window, no COM.
#include "overlay.h"
#include "test_harness.h"

#include <windows.h>

using namespace tv;

namespace {

// A 2560x1440 monitor whose taskbar (48 px) sits at the bottom edge.
constexpr RECT kWork { 0, 0, 2560, 1392 };
constexpr int kW = 220;
constexpr int kH = 76;

RECT place(OverlayAnchor anchor, int ox = 0, int oy = 0, POINT cursor = { 1280, 700 },
    RECT work = kWork)
{
    return overlayRect(anchor, ox, oy, kW, kH, cursor, work);
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
        RECT { 2340, 1316, 2560, 1392 }));
    CHECK(eq(place(OverlayAnchor::BottomLeft),
        RECT { 0, 1316, 220, 1392 }));
    CHECK(eq(place(OverlayAnchor::TopRight),
        RECT { 2340, 0, 2560, 76 }));
}

TEST("overlay: bottom placements sit flush on the work area's edges")
{
    // The tray tooltip and button previews are drawn just above the taskbar;
    // the panel sits flush on the work area's bottom edge and right edge and
    // is kept topmost, so it covers them completely.
    const RECT r = place(OverlayAnchor::BottomRight);
    CHECK_EQ(r.bottom, 1392); // flush with the taskbar's top edge
    CHECK_EQ(r.right, 2560);  // flush with the screen's right edge

    // The follow-mouse anchor clamps the same way: a cursor sitting on the
    // taskbar would otherwise leave the panel floating above it.
    const RECT m = place(OverlayAnchor::Mouse, 0, 0, { 2454, 1416 });
    CHECK_EQ(m.bottom, 1392);

    // Bottom-center too; top placements are not touched.
    CHECK_EQ(place(OverlayAnchor::BottomCenter).bottom, 1392);
    CHECK_EQ(place(OverlayAnchor::TopRight).bottom, 76);
}

TEST("overlay: a monitor without a bottom taskbar clamps the same way")
{
    const RECT noTaskbar { 0, 0, 2560, 1440 };
    const RECT r = place(OverlayAnchor::BottomRight, 0, 0, { 1280, 700 }, noTaskbar);
    CHECK_EQ(r.bottom, 1440);
    CHECK_EQ(r.right, 2560);
}

TEST("overlay: offsets move the panel but cannot drop it past the work area")
{
    const RECT pushed = place(OverlayAnchor::BottomRight, 0, 60);
    CHECK_EQ(pushed.bottom, 1392); // pulled back up onto the edge

    const RECT nudged = place(OverlayAnchor::BottomRight, -40, 0);
    CHECK_EQ(nudged.right, 2520);
    CHECK_EQ(nudged.bottom, 1392);
}

TEST("overlay: panel clamps at the screen edges")
{
    // A cursor near the left edge with the follow-mouse anchor: the panel is
    // pulled back into the monitor.
    const RECT left = place(OverlayAnchor::Mouse, 0, 0, { 5, 700 });
    CHECK_EQ(left.left, 0);

    // An anchor pushed past the bottom of a band-less monitor clamps too.
    const RECT noTaskbar { 0, 0, 2560, 1440 };
    const RECT low = place(OverlayAnchor::BottomRight, 0, 200, { 1280, 700 }, noTaskbar);
    CHECK_EQ(low.bottom, 1440);
}

TEST("overlay: a short work area still fits the panel")
{
    const RECT work { 0, 0, 800, 120 };
    const RECT r = overlayRect(OverlayAnchor::BottomRight, 0, 0, kW, kH, { 400, 100 }, work);
    CHECK_EQ(r.top, 120 - kH);
    CHECK_EQ(r.bottom, 120);
}
