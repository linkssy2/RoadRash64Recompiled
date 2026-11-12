#include <cstdlib>
#include <iostream>

#include "render/rt64_rr64_frame_overwrite.h"

using namespace RT64::RR64FramePacing;

static void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "Frame overwrite proof failed: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    const OverwriteTarget color{1, 1920, 1440};
    const OverwriteTarget otherColor{2, 1920, 1440};
    const OverwriteTarget depth{3, 1920, 1440};
    const OverwriteTarget otherDepth{4, 1920, 1440};

    require(exactFullOverwriteRect(color, 0, 0, 1920, 1440),
        "an exact converted GPU rectangle covers the target");
    require(!exactFullOverwriteRect(color, 0, 0, 320, 240),
        "guest dimensions are not a full high-resolution GPU clear");
    require(!exactFullOverwriteRect(color, 0, 0, 1919, 1440) &&
        !exactFullOverwriteRect(color, 0, 0, 1920, 1439),
        "a one-pixel short clear leaves retained content");
    require(!exactFullOverwriteRect(color, 1, 0, 1920, 1440) &&
        !exactFullOverwriteRect(color, 0, 1, 1920, 1440),
        "an offset clear is not full coverage");
    require(!exactFullOverwriteRect(color, -1, 0, 1920, 1440) &&
        !exactFullOverwriteRect(color, 0, 0, 1921, 1440),
        "out-of-bounds rectangles are rejected conservatively");
    require(!exactFullOverwriteRect({}, 0, 0, 1920, 1440) &&
        !exactFullOverwriteRect({1, 0, 0}, 0, 0, 0, 0),
        "an invalid or empty target cannot certify a clear");

    FrameOverwriteProof valid(color);
    valid.clearDepth(depth, true);
    valid.clearColor(color, true);
    valid.drawColor(color, depth, true);
    valid.drawColor(color, {}, false); // A later HUD pair on the same color target.
    require(valid.complete(), "depth clear, color clear, world and HUD form one initialized image");

    FrameOverwriteProof colorOnly(color);
    colorOnly.clearColor(color, true);
    colorOnly.drawColor(color, {}, false);
    require(colorOnly.complete(), "color-only rendering does not require an unused depth buffer");

    FrameOverwriteProof noClear(color);
    noClear.drawColor(color, depth, false);
    noClear.clearColor(color, true);
    require(!noClear.complete(), "a late color clear cannot repair failed first-write admission");

    FrameOverwriteProof partial(color);
    partial.clearColor(color, false);
    partial.clearColor(color, true);
    require(!partial.complete(), "partial-first color clears cannot acquire a certificate later");

    FrameOverwriteProof holes(color);
    holes.clearColor(color, false);
    holes.clearColor(color, false);
    require(!holes.complete(), "merged bounding rectangles cannot prove gap-free coverage");

    FrameOverwriteProof lateDepth(color);
    lateDepth.clearColor(color, true);
    lateDepth.drawColor(color, depth, true);
    lateDepth.clearDepth(depth, true);
    require(!lateDepth.complete(), "depth must be fully initialized before its first dependency");

    FrameOverwriteProof partialDepth(color);
    partialDepth.clearColor(color, true);
    partialDepth.clearDepth(depth, false);
    partialDepth.drawColor(color, depth, true);
    require(!partialDepth.complete(), "a depth-only fast path is insufficient without full coverage");

    FrameOverwriteProof switchedDepth(color);
    switchedDepth.clearColor(color, true);
    switchedDepth.clearDepth(depth, true);
    switchedDepth.drawColor(color, otherDepth, true);
    require(!switchedDepth.complete(), "a different depth target cannot borrow a clear certificate");

    FrameOverwriteProof grownDepth(color);
    grownDepth.clearColor(color, true);
    grownDepth.clearDepth(depth, true);
    grownDepth.drawColor(color, {depth.identity, depth.width, depth.height + 1}, true);
    require(!grownDepth.complete(), "depth dimensions are part of the exact initialization proof");

    FrameOverwriteProof sameDepthAfterPartial(color);
    sameDepthAfterPartial.clearColor(color, true);
    sameDepthAfterPartial.clearDepth(depth, true);
    sameDepthAfterPartial.clearDepth(depth, false);
    sameDepthAfterPartial.drawColor(color, depth, true);
    require(sameDepthAfterPartial.complete(), "partial writes after a full depth clear retain initialization");

    FrameOverwriteProof differentColor(color);
    differentColor.clearColor(color, true);
    differentColor.clearColor(otherColor, true);
    require(!differentColor.complete(), "another color target is outside the single-target proof");

    FrameOverwriteProof depthAlias(color);
    depthAlias.clearColor(color, true);
    depthAlias.clearDepth(color, true);
    require(!depthAlias.complete(), "one GPU target cannot simultaneously certify color and depth");

    FrameOverwriteProof unsupported(color);
    unsupported.clearColor(color, true);
    unsupported.reject();
    unsupported.clearColor(color, true);
    require(!unsupported.complete(), "unsupported operations invalidate the entire sequence permanently");

    FrameOverwriteProof untouched(color);
    untouched.clearDepth(depth, true);
    require(!untouched.complete(), "depth-only work does not produce a complete selected color image");
    require(!FrameOverwriteProof({}).complete(), "an uninitialized proof is rejected");

    FrameOverwriteProof capacity(color);
    capacity.clearColor(color, true);
    for (uintptr_t id = 10; id < 27; id++) {
        capacity.clearDepth({id, 1920, 1440}, true);
    }
    require(!capacity.complete(), "exceeding the bounded depth certificate capacity fails closed");

    std::cout << "RR64 frame overwrite proof passed: converted GPU coverage, command order, "
        "exact depth identity and extent, later HUD writes, and conservative rejections. "
        "No GPU or ROM is used.\n";
}
