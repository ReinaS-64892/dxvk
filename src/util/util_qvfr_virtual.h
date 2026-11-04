#pragma once
#include "util_qvfr.h"

namespace dxvk::qvfr {
  constexpr uint32_t VirtualSlots = 128;
  constexpr uint32_t VirtualUniformSlot = 127; // D3D11 uses CBV slots 0..95.
  struct VirtualMap {
    // low transform, left/right focus transforms, left/right UV bounds,
    // inverse atlas size (also the enable flag). Six constant-buffer vectors.
    std::array<std::array<float, 4>, 6> rows = {};
  };
  static_assert(sizeof(VirtualMap) == 96);
  struct VirtualLayout {
    Extent extent;
    std::array<Rect, 2> low, focus;
    VirtualMap mapping;
  };
  inline VirtualLayout virtualLayout(Extent original, const std::array<EyePlan, 2>& eyes) {
    uint32_t lw = eyes[0].peripheral.width, lh = eyes[0].peripheral.height;
    uint32_t fw = eyes[0].focus.width, fh = eyes[0].focus.height;
    VirtualLayout r = {};
    r.extent = {2 * (lw + fw), std::max(lh, fh), 1};
    float iw = 1.0f / r.extent.width, ih = 1.0f / r.extent.height;
    r.mapping.rows[0] = {2 * lw * iw, lh * ih, 0, 0};
    r.mapping.rows[5] = {iw, ih, 0, 0};
    for (uint32_t i = 0; i < 2; i++) {
      r.low[i] = {i * lw, 0, lw, lh, 0};
      r.focus[i] = {2 * lw + i * fw, 0, fw, fh, 0};
      auto c = eyes[i].composite;
      r.mapping.rows[1+i] = {original.width * iw, original.height * ih,
        (float(r.focus[i].x) - c.x) * iw, -float(c.y) * ih};
      r.mapping.rows[3+i] = {float(c.x)/original.width, float(c.y)/original.height,
        float(c.x+c.width)/original.width, float(c.y+c.height)/original.height};
    }
    return r;
  }
  // CPU reference for the shader coordinate transform, including tile-edge
  // clamping so bilinear taps never enter a neighbouring tile or atlas padding.
  inline std::array<float, 2> virtualUv(const VirtualMap& m, float u, float v) {
    if (!m.rows[5][0]) return {u, v};
    u = std::clamp(u, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f);
    auto eval = [&] (uint32_t i, const std::array<float, 4>& bounds) {
      std::array<float, 2> out;
      for (uint32_t j = 0; j < 2; j++) {
        float lo = bounds[j] * m.rows[i][j] + m.rows[i][j+2] + .5f*m.rows[5][j];
        float hi = bounds[j+2] * m.rows[i][j] + m.rows[i][j+2] - .5f*m.rows[5][j];
        out[j] = std::clamp((j ? v : u)*m.rows[i][j] + m.rows[i][j+2], lo, std::max(lo, hi));
      }
      return out;
    };
    for (uint32_t eye = 0; eye < 2; eye++) {
      auto b = m.rows[3+eye];
      if (u >= b[0] && v >= b[1] && u < b[2] && v < b[3]) return eval(1+eye, b);
    }
    return eval(0, {0, 0, 1, 1});
  }
}
