#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace dxvk::qvfr {
  // Vulkan KEEP is zero. A non-zero write mask alone does not imply a
  // write: all three outcomes can still preserve the original stencil.
  inline bool stencilMayWrite(uint32_t writeMask, uint32_t fail,
      uint32_t pass, uint32_t depthFail) {
    return writeMask && (fail || pass || depthFail);
  }

  struct PendingPlanes {
    bool color = false, depth = false, stencil = false;
    bool colorStale = false;
    bool depthStale = false;
    bool any() const { return color || depth || stencil; }
    void draw(bool writesStencil = false) {
      color = depth = true;
      stencil |= writesStencil;
      colorStale = false;
      depthStale = false;
    }
    // Call only after materializing color, before an ordinary color write.
    void invalidateColor() { colorStale = true; }
    void refreshColor() { colorStale = false; }
    void invalidateDepth() { depthStale = true; }
    void refreshDepth() { depthStale = false; }
    void complete(bool c, bool d, bool s = false) {
      if (c) color = false;
      if (d) depth = false;
      if (s) stencil = false;
    }
  };

  struct CacheSlot {
    uintptr_t color = 0, depth = 0;
    uint32_t eye = 0;
    uint64_t lastUse = 0;
    bool dirty = false;

    bool references(uintptr_t image) const {
      return dirty && image && (color == image || depth == image);
    }
  };

  // Retain both eyes and unrelated render targets. Only a full dirty cache
  // requires eviction; an exact existing target always keeps its slot.
  template<size_t N>
  size_t chooseDeferredSlot(const std::array<CacheSlot, N>& slots,
      uintptr_t color, uintptr_t depth, uint32_t eye) {
    static_assert(N > 0);
    for (size_t i = 0; i < N; i++)
      if (slots[i].color == color && slots[i].depth == depth && slots[i].eye == eye)
        return i;
    size_t chosen = 0;
    for (size_t i = 1; i < N; i++) {
      if ((!slots[i].dirty && slots[chosen].dirty)
       || (slots[i].dirty == slots[chosen].dirty && slots[i].lastUse < slots[chosen].lastUse))
        chosen = i;
    }
    return chosen;
  }
}
