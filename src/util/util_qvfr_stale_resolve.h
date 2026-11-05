#pragma once
#include <cstdint>

namespace dxvk::qvfr {
  // Compare image identities rather than exact RTV/DSV objects. Unity may
  // switch HDR color targets while keeping the scene depth image.
  inline const char* staleResolveReaderGate(bool compute, bool depthTest, bool eyeViewport,
      bool sceneShape, uint64_t sourceColor, uint64_t sourceDepth, uint64_t consumerColor, uint64_t consumerDepth) {
    if (compute) return "reader_compute";
    if (!depthTest) return "reader_depth_disabled";
    if (!eyeViewport) return "reader_viewport";
    if (!sceneShape) return "reader_target_shape";
    if (sourceColor != consumerColor && (!sourceDepth || sourceDepth != consumerDepth))
      return "reader_unrelated_pair";
    return nullptr;
  }

  // This intentionally reuses previously resolved scene input. Never skip an
  // unknown/uninitialized target, the refresh frame, or a known external output.
  struct StaleResolvePolicy {
    bool valid = false, sceneReader = false, external = false;
    uint64_t refreshedFrame = 0;
    void refreshed(uint64_t frame) { valid = true; refreshedFrame = frame; }
    void observeStorage(uint64_t previous, uint64_t current, bool refreshPending,
        uint64_t refreshFrame, uint64_t frame) {
      if (refreshPending && refreshFrame == frame) refreshed(refreshFrame);
      else if (previous != current) { valid = false; sceneReader = false; }
    }
    bool skip(uint64_t frame, uint32_t interval, bool activeScene) const {
      return activeScene && valid && sceneReader && !external && interval > 1
        && frame > refreshedFrame && frame - refreshedFrame < interval;
    }
  };
}
