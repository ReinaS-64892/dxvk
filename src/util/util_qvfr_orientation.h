#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace dxvk::qvfr {
  struct OrientationPixel { float u, v, valid, alpha; };

  // Four 2x2 patches: left/right eye centers, upper/lower quarter.
  // This is evidence at the sampled points, not proof of a global affine UV.
  inline const char* classifyOrientation(const std::array<OrientationPixel, 16>& pixels,
      uint32_t eye, uint32_t height) {
    if (eye >= 2 || !height) return "unknown";
    bool identity = true, flip = true;
    for (uint32_t row = 0; row < 2; row++) for (uint32_t y = 0; y < 2; y++)
      for (uint32_t x = 0; x < 2; x++) {
        const auto& p = pixels[(row * 2 + eye) * 4 + y * 2 + x];
        if (!std::isfinite(p.u) || !std::isfinite(p.v) || p.valid != 1.f || p.alpha != 1.f)
          return "unknown";
        double v = (height * (row ? 3u : 1u) / 4u + y + .5) / height;
        identity &= std::abs(p.v - v) < 0.002;
        flip &= std::abs(p.v - (1.0-v)) < 0.002;
      }
    return identity ? "identity-y" : flip ? "flip-y" : "other";
  }
}
