#pragma once
#include <array>
#include <cstdint>

namespace dxvk::qvfr {
  struct ResolveReuseKey {
    uint64_t frame = 0, source = 0, sourceStorage = 0, destination = 0, destinationStorage = 0;
    uint32_t format = 0, samples = 0, width = 0, height = 0;
    bool operator==(const ResolveReuseKey& b) const {
      return frame == b.frame && source == b.source && sourceStorage == b.sourceStorage
        && destination == b.destination && destinationStorage == b.destinationStorage
        && format == b.format && samples == b.samples && width == b.width && height == b.height;
    }
  };
  struct ResolveReuseHistory {
    std::array<ResolveReuseKey, 8> entries = {};
    uint32_t next = 0;
    void invalidate(uint64_t image) {
      for (auto& e : entries) if (e.source == image || e.destination == image) e = {};
    }
    bool contains(const ResolveReuseKey& key) const {
      if (!key.source || !key.destination) return false;
      for (const auto& e : entries) if (e == key) return true;
      return false;
    }
    void record(const ResolveReuseKey& key) {
      if (contains(key)) return;
      entries[next++ % entries.size()] = key;
    }
  };
}
