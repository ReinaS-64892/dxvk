#pragma once
#include <cstdint>

namespace dxvk::qvfr {
  // Preserve the original instance count/firstInstance for both replays.
  // Full-width stereo instancing still needs a different coordinate policy.
  inline bool directReplayInstances(bool singleInstance, bool unsafeInstancing,
      bool safeInstancing, bool eyeViewport) {
    return singleInstance || unsafeInstancing || (safeInstancing && eyeViewport);
  }

  struct ReplayTimingBudget {
    static constexpr uint32_t Limit = 8, FullLimit = 8192;
    uint32_t samples = 0, skipped = 0;
    bool take(bool full = false) {
      if (samples == (full ? FullLimit : Limit)) { skipped++; return false; }
      samples++;
      return true;
    }
  };

  // An explicit image transition already publishes that external consumer's
  // complete dependency. Generic interop still requires every pending plane.
  inline bool fullInteropPublication(bool virtualImages, bool lazyDepthStencil, bool imageTransition) {
    return !imageTransition || (!virtualImages && !lazyDepthStencil);
  }

  // Desktop Present/endFrame is the conservative reset boundary. Interop
  // handoffs (including the second eye) must never reopen a closed scene.
  struct SceneCycle {
    uint64_t frame = ~uint64_t(0);
    bool started = false, closed = false;
    void enter(uint64_t nextFrame) {
      if (frame == nextFrame) return;
      frame = nextFrame;
      started = closed = false;
    }
    void rendered(uint64_t nextFrame) { enter(nextFrame); started = true; }
    bool finish(uint64_t nextFrame) {
      enter(nextFrame);
      if (!started || closed) return false;
      closed = true;
      return true;
    }
    // Explicit opt-in only: the caller must validate an eye-local scene draw
    // returning to the same original attachments after a resolve boundary.
    bool resume(uint64_t nextFrame) {
      if (frame != nextFrame || !started || !closed) return false;
      closed = false;
      return true;
    }
    bool allows(uint64_t nextFrame) { enter(nextFrame); return !closed; }
  };
}
