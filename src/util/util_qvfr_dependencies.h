#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxvk::qvfr {
  // Per dependency scan only. Never cache across writes, transfers or draws.
  // Saturation falls back to visiting, so it cannot hide a dependency.
  template<size_t N = 32> struct ImageReadVisits {
    static_assert(N && !(N & (N - 1)));
    struct Entry { uintptr_t image = 0; uint32_t aspects = 0; bool sampled = false; };
    struct Visit { uint32_t aspects; bool observe; };
    std::array<Entry, N> entries = {};
    Visit visit(uintptr_t image, uint32_t aspects, bool observe) {
      size_t slot = (image >> 4) & (N - 1);
      for (size_t i = 0; i < N; i++, slot = (slot + 1) & (N - 1)) {
        auto& e = entries[slot];
        if (e.image && e.image != image) continue;
        Visit result{aspects & ~e.aspects, observe && !e.sampled};
        e.image = image;
        e.aspects |= aspects;
        e.sampled |= observe;
        return result;
      }
      return {aspects, observe};
    }
  };
}
