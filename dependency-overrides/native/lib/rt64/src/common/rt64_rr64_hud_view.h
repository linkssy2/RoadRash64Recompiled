#pragma once
#include <cstdint>

namespace RT64 {
    // Bounds are in authored framebuffer pixels, never controller/peer indices.
    // Online's one local camera consequently stays a single full-screen view.
    struct RR64HUDView {
        float left=0, top=0, right=0, bottom=0;
        constexpr float width() const { return right-left; }
        constexpr float height() const { return bottom-top; }
        constexpr bool contains(float l,float t,float r,float b) const {
            return l>=left-1 && t>=top-1 && r<=right+1 && b<=bottom+1;
        }
        constexpr bool same(const RR64HUDView& b) const {
            return left==b.left && top==b.top && right==b.right && bottom==b.bottom;
        }
    };
    constexpr uint16_t rr64HUDViewOrigin(const RR64HUDView& view,bool right,float framebufferWidth,uint16_t rightOrigin) {
        return uint16_t((right ? view.right : view.left)/framebufferWidth*rightOrigin+0.5f);
    }
    constexpr float rr64HUDViewOffset(uint16_t origin,uint16_t rightOrigin,float fullOffset) {
        return (2.0f*float(origin)/float(rightOrigin)-1.0f)*fullOffset;
    }
    // Local 3/4-player races draw each camera into one column of a two-column
    // grid. A column spans from a screen edge to the centre line, so it widens
    // with the window exactly like a full-width camera and the per-view HUD
    // origins above line up with its edges. Pixel bounds, small tolerance.
    constexpr bool rr64SplitColumnView(int32_t left,int32_t right,int32_t screenLeft,int32_t screenRight) {
        const int32_t width=screenRight-screenLeft;
        const int32_t middle=screenLeft+width/2;
        const int32_t slack=width/64+1;
        auto close=[slack](int32_t a,int32_t b){ return a-b<=slack && b-a<=slack; };
        return width>0 && ((close(left,screenLeft) && close(right,middle)) ||
            (close(left,middle) && close(right,screenRight)));
    }
}
