#pragma once

#include "util_qvfr.h"

namespace dxvk::qvfr {
  // The post path replays BOTH eyes and only tracks color writes. Eye-local
  // geometry and draws using depth/stencil belong to the scene replay path,
  // even in UNSAFE mode; treating them as post skips scene cache renewal.
  inline bool postReplayEligible(bool fullViewport, bool depthTest,
      bool stencilTest, bool depthBounds) {
    return fullViewport && !depthTest && !stencilTest && !depthBounds;
  }

  // Empirical rule for the HDR pass measured in
  // logs/orientation-identity-first-20261104: both eyes were identity-y in
  // all 18 captures. This is not a general Unity orientation detector.
  inline bool measuredIdentityPost(std::string_view vs, std::string_view ps, uint32_t format) {
    return format == 97
      && vs == "vs.135010b798b162853b35cfe2e49f79ab"
      && ps == "fs.3649abe482390ade1473c1588ebba0d1";
  }

  inline std::optional<std::array<EyePlan, 2>> postSnapshotEyes(Extent size,
      double scale, double fraction, const std::array<EyePlan, 2>& source,
      std::array<Gaze, 2> gaze, bool preserveSource) {
    // The input snapshot is authoritative: rebuilding from newer gaze can
    // lose the location of its actual high-resolution pixels, even with no
    // Y flip. Copy the complete plans, including clip/crop and eye packing.
    if (preserveSource) return source;
    auto output = makeOutputPlan(size, scale, fraction, gaze);
    if (!output) return std::nullopt;
    return output->eyes;
  }
}
