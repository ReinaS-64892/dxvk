#pragma once
#include <string_view>
#include <set>
#include <cstdint>

namespace dxvk::qvfr {
  // Strict UNSAFE: internal consumers never force a full-size representation.
  // Explicit handoffs and disabling QVFR must still publish deferred pixels.
  inline bool unsafePublishBoundary(std::string_view reason) {
    return reason == "external-transition" || reason == "external-acquire"
      || reason == "external-release" || reason == "external-rendering"
      || reason == "interop-flush" || reason == "control-disable";
  }

  struct UnsafeTerminalCycle {
    std::set<uint64_t> images;
    bool normal = false;
    bool startDraw(uint64_t target) {
      if (target && images.count(target)) normal = true;
      return normal;
    }
    void submitted(uint64_t image) {
      if (image && images.size() < 128) images.insert(image);
      normal = false;
    }
  };
}
