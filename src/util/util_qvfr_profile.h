#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dxvk::qvfr {
  // CPU timings only; no Vulkan queries and no allocations per scope.
  template<size_t Count, typename Clock = std::chrono::steady_clock>
  struct CpuDetailCollector {
    struct Total { double inclusiveMs = 0, exclusiveMs = 0; uint64_t calls = 0; };
    struct Scope;
    uint64_t frame = ~uint64_t(0);
    std::array<Total, Count> totals = {};
    Scope* parent = nullptr;
    struct Scope {
      CpuDetailCollector* collector = nullptr;
      Scope* parent = nullptr;
      size_t stage = 0;
      double childMs = 0;
      typename Clock::time_point start;
      Scope(CpuDetailCollector* c, bool enabled, uint64_t f, size_t s) {
        if (!c || !enabled || f % 120 || s >= Count) return;
        if (c->frame != f) {
          if (c->parent) return; // A scope cannot straddle capture frames.
          c->frame = f;
          c->totals = {};
        }
        collector = c; parent = c->parent; stage = s;
        c->parent = this;
        start = Clock::now();
      }
      ~Scope() { finish(); }
      void finish() {
        if (!collector) return;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto& total = collector->totals[stage];
        total.inclusiveMs += ms;
        total.exclusiveMs += std::max(0.0, ms - childMs);
        total.calls++;
        if (parent) parent->childMs += ms;
        collector->parent = parent;
        collector = nullptr;
      }
      Scope(const Scope&) = delete;
      Scope& operator=(const Scope&) = delete;
    };
  };
}
