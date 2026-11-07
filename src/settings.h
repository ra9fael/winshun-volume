// settings.h — plugin settings with strict validation. All bad values fall
// back to defaults instead of erroring: a wrong settings file must never
// disable the plugin or produce out-of-range behavior.
#pragma once

#include "winshun.h"

#include "overlay.h"

namespace tv {

struct Settings {
    int percentPerStep = 2; // 1..10
    bool showOverlay = true;
    int overlayMs = 600; // 200..2000
    bool enableMouse = true;
    bool chinese = true; // host language for overlay / list text; not a user setting
    OverlayAnchor overlayAnchor = OverlayAnchor::Center;
    int overlayOffsetX = 0; // -4000..4000 px from the anchor
    int overlayOffsetY = 0;
};

inline Settings parseSettings(const winshun::Json& s)
{
    Settings out;
    if (!s.isObject())
        return out;
    const winshun::Json& step = s["percentPerStep"];
    if (step.isNumber()) {
        const double v = step.toNumber();
        if (v >= 1 && v <= 10)
            out.percentPerStep = static_cast<int>(v);
    }
    const winshun::Json& overlay = s["showOverlay"];
    if (overlay.isBool())
        out.showOverlay = overlay.toBool();
    const winshun::Json& ms = s["overlayMs"];
    if (ms.isNumber()) {
        const double v = ms.toNumber();
        if (v >= 200 && v <= 2000)
            out.overlayMs = static_cast<int>(v);
    }
    const winshun::Json& mouse = s["enableMouse"];
    if (mouse.isBool())
        out.enableMouse = mouse.toBool();

    const winshun::Json& anchor = s["overlayAnchor"];
    if (anchor.isString()) {
        const std::string v = anchor.toString();
        if (v == "top-left")
            out.overlayAnchor = OverlayAnchor::TopLeft;
        else if (v == "top-center")
            out.overlayAnchor = OverlayAnchor::TopCenter;
        else if (v == "top-right")
            out.overlayAnchor = OverlayAnchor::TopRight;
        else if (v == "middle-left")
            out.overlayAnchor = OverlayAnchor::MiddleLeft;
        else if (v == "middle-right")
            out.overlayAnchor = OverlayAnchor::MiddleRight;
        else if (v == "bottom-left")
            out.overlayAnchor = OverlayAnchor::BottomLeft;
        else if (v == "bottom-center")
            out.overlayAnchor = OverlayAnchor::BottomCenter;
        else if (v == "bottom-right")
            out.overlayAnchor = OverlayAnchor::BottomRight;
        else if (v == "mouse")
            out.overlayAnchor = OverlayAnchor::Mouse;
        else
            out.overlayAnchor = OverlayAnchor::Center;
    }

    auto offset = [](const winshun::Json& value) {
        if (!value.isNumber())
            return 0;
        double d = value.toNumber();
        if (d < -4000)
            d = -4000;
        if (d > 4000)
            d = 4000;
        return static_cast<int>(d);
    };
    out.overlayOffsetX = offset(s["overlayOffsetX"]);
    out.overlayOffsetY = offset(s["overlayOffsetY"]);
    return out;
}

} // namespace tv
