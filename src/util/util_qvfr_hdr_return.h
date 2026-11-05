#pragma once
#include <array>
#include <cstdint>

namespace dxvk::qvfr {
  struct HdrReturnPath {
    uint64_t frame = 0, temporary = 0, temporaryStorage = 0;
    uint64_t original = 0, originalStorage = 0, resolved = 0, resolvedStorage = 0;
    uint32_t eye = 0;
    bool sameImages(const HdrReturnPath& b) const {
      return frame == b.frame && temporary == b.temporary && temporaryStorage == b.temporaryStorage
        && original == b.original && originalStorage == b.originalStorage
        && resolved == b.resolved && resolvedStorage == b.resolvedStorage;
    }
  };
  // Numeric identities only. Each eye can skip one return only after its
  // first leg was executed or queued in the explicitly unsafe defer mode.
  struct HdrReturnHistory {
    struct Entry { HdrReturnPath path; uint32_t captured = 0, consumed = 0; };
    std::array<Entry, 4> entries = {};
    void invalidate(uint64_t image, const HdrReturnPath* writing = nullptr) {
      for (auto& e : entries) {
        if (!e.captured) continue;
        if (image == e.path.original || image == e.path.resolved) e = {};
        else if (image == e.path.temporary) {
          if (writing && writing->eye < 2 && e.path.sameImages(*writing)) {
            auto bit = 1u << writing->eye;
            e.captured &= ~bit; e.consumed &= ~bit;
          } else e = {};
        }
      }
    }
    bool record(const HdrReturnPath& path) {
      if (!path.temporary || !path.original || !path.resolved || path.eye >= 2) return false;
      Entry* free = nullptr;
      for (auto& e : entries) {
        if (e.captured && e.path.sameImages(path)) {
          e.captured |= 1u << path.eye; e.consumed &= ~(1u << path.eye); return true;
        }
        if (!e.captured || e.path.frame != path.frame) free = &e;
      }
      if (!free) return false;
      *free = {path, 1u << path.eye, 0};
      return true;
    }
    bool consume(uint64_t frame, uint64_t temporary, uint64_t temporaryStorage,
        uint64_t original, uint64_t originalStorage, uint32_t eye) {
      if (eye >= 2) return false;
      for (auto& e : entries) {
        const auto& p = e.path;
        uint32_t bit = 1u << eye;
        if (p.frame == frame && p.temporary == temporary && p.temporaryStorage == temporaryStorage
         && p.original == original && p.originalStorage == originalStorage
         && (e.captured & bit) && !(e.consumed & bit)) {
          e.consumed |= bit; return true;
        }
      }
      return false;
    }
  };
}
