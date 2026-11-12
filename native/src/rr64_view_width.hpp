#pragma once
#include <atomic>
namespace rr64 {
// Renderer expansion relative to the N64 television's 4:3 frame.
inline std::atomic<double> view_width{4.0 / 3.0};
}
