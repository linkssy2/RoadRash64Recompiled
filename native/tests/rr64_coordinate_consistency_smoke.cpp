#include "hle/rt64_rr64_coordinate_consistency.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using RT64::RR64CoordinateConsistency::TranslationState;
using RT64::RR64CoordinateConsistency::coherentTranslations;

namespace {
    void require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }
}

int main() {
    // A stationary point in absolute coordinates rebased by 256 units: both
    // authored endpoints agree, but a held world and half-blended view do not.
    TranslationState world{ { 512.0f, 0.0f, 0.0f }, { 256.0f, 0.0f, 0.0f }, true, false };
    TranslationState view{ { -500.0f, 0.0f, 0.0f }, { -244.0f, 0.0f, 0.0f }, true, true };
    const float previousPoint = world.previous[0] + view.previous[0];
    const float currentPoint = world.current[0] + view.current[0];
    const float mixedPoint = world.current[0] + (view.previous[0] + view.current[0]) * 0.5f;
    require(previousPoint == currentPoint && mixedPoint != currentPoint,
        "fixture must reproduce the origin cancellation failure");
    require(!coherentTranslations(true, world, view), "mixed origin decisions must refuse interpolation");

    world.interpolate = true;
    require(coherentTranslations(true, world, view), "coordinated interpolation must remain eligible");
    const float coherentPoint = (world.previous[0] + world.current[0]) * 0.5f +
        (view.previous[0] + view.current[0]) * 0.5f;
    require(coherentPoint == currentPoint, "paired interpolation must preserve origin cancellation");

    world.interpolate = false;
    const auto rebasedWorld = world;
    const auto movingView = view;
    world.current = world.previous;
    require(coherentTranslations(true, world, view), "static terrain with moving camera must remain eligible");
    world = rebasedWorld;
    view.current = view.previous;
    require(coherentTranslations(true, world, view), "unchanged view must not trigger the coupled-motion guard");
    view = movingView;
    view.interpolate = false;
    require(coherentTranslations(true, world, view), "two held translations must remain eligible");

    // An explicitly held camera with an interpolated world loses the same
    // cancellation in the opposite direction. Exercise the finished point,
    // rather than only testing the helper's flag combination.
    world.interpolate = true;
    const float reverseMixedPoint = (world.previous[0] + world.current[0]) * 0.5f + view.current[0];
    require(reverseMixedPoint != currentPoint, "reverse fixture must reproduce the origin cancellation failure");
    require(!coherentTranslations(true, world, view), "held view with blended world must refuse interpolation");
    world.current = world.previous;
    require(coherentTranslations(true, world, view), "unchanged world must allow a held view");
    world = rebasedWorld;
    world.interpolate = true;
    view.current = view.previous;
    require(coherentTranslations(true, world, view), "unchanged held view must allow world interpolation");
    world = rebasedWorld;

    view = movingView;
    require(coherentTranslations(false, world, view), "disabled or non-race scope must preserve existing behavior");
    world.mapped = false;
    require(coherentTranslations(true, world, view), "unmatched world remains the membership gate's responsibility");
    world = rebasedWorld;
    view.mapped = false;
    require(coherentTranslations(true, world, view), "unmatched view remains the projection gate's responsibility");
    view = movingView;

    // Check all axes and signed zero, not only forward camera motion.
    for (unsigned axis = 0; axis < 3; axis++) {
        TranslationState axisWorld{ {}, {}, true, false };
        TranslationState axisView{ {}, {}, true, true };
        axisWorld.current[axis] = -64.0f;
        axisView.current[axis] = 64.0f;
        require(!coherentTranslations(true, axisWorld, axisView), "origin conflict must be detected on each axis");
        axisWorld.interpolate = true;
        axisView.interpolate = false;
        require(!coherentTranslations(true, axisWorld, axisView), "reverse origin conflict must be detected on each axis");
        axisWorld.current[axis] = -0.0f;
        require(coherentTranslations(true, axisWorld, axisView), "signed zero is not changed translation");
    }

    for (const float invalid : { std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
        for (unsigned field = 0; field < 4; field++) {
            auto invalidWorld = rebasedWorld;
            auto invalidView = movingView;
            invalidWorld.interpolate = true;
            auto &value = (field == 0) ? invalidWorld.previous[1] :
                (field == 1) ? invalidWorld.current[1] :
                (field == 2) ? invalidView.previous[1] : invalidView.current[1];
            value = invalid;
            require(!coherentTranslations(true, invalidWorld, invalidView), "mapped nonfinite translations must fail closed");
        }
    }

    std::cout << "RR64 coordinate consistency smoke passed\n";
}
