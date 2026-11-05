#pragma once
#include <cstdint>
#include <utility>

namespace dxvk::qvfr {
  struct DeferredDraw {
    uint64_t token = 0;
    bool indexed = false;
    uint32_t count = 0, first = 0;
    int32_t vertexOffset = 0;
  };
  struct DeferredDrawSlot {
    DeferredDraw pending;
    uint64_t serial = 0;
    // Caller must flush before replacing an occupied slot.
    uint64_t store(bool indexed, uint32_t count, uint32_t first, int32_t offset) {
      if (pending.token) return 0;
      pending = {++serial, indexed, count, first, offset};
      return pending.token;
    }
    bool cancel(uint64_t token) {
      if (!token || pending.token != token) return false;
      pending = {}; return true;
    }
    DeferredDraw take() { return std::exchange(pending, {}); }
  };
}
