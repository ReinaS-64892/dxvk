#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace dxvk::qvfr {
  struct GrabToken {
    uint64_t root = 0, generation = 0, frame = 0;
    uint32_t hops = 0;
    explicit operator bool() const { return root != 0; }
  };
  // Cookie and storage identities only: never retain application images.
  struct GrabProvenance {
    static constexpr size_t Limit = 128;
    static constexpr uint32_t MaxHops = 8;
    struct Node { uint64_t storage; GrabToken token; };
    std::map<uint64_t, Node> nodes;
    void invalidate(uint64_t cookie) { nodes.erase(cookie); }
    std::optional<GrabToken> find(uint64_t cookie, uint64_t storage, uint64_t frame, bool external = false) const {
      auto it = nodes.find(cookie);
      if (it == nodes.end() || it->second.storage != storage
       || (!external && it->second.token.frame != frame)) return std::nullopt;
      return it->second.token;
    }
    bool link(uint64_t destination, uint64_t storage, uint64_t frame, GrabToken source) {
      invalidate(destination);
      if (!source || destination == source.root || source.frame != frame || source.hops >= MaxHops) return false;
      if (nodes.size() >= Limit) {
        auto old = std::min_element(nodes.begin(), nodes.end(),
          [] (const auto& a, const auto& b) { return a.second.token.frame < b.second.token.frame; });
        nodes.erase(old);
      }
      source.hops++;
      nodes.emplace(destination, Node{storage, source});
      return true;
    }
  };
  template<typename F> struct GrabScopeExit {
    F action;
    explicit GrabScopeExit(F f) : action(std::move(f)) { }
    ~GrabScopeExit() { action(); }
    GrabScopeExit(const GrabScopeExit&) = delete;
    GrabScopeExit& operator=(const GrabScopeExit&) = delete;
  };
  template<typename F> auto grabScopeExit(F f) { return GrabScopeExit<F>(std::move(f)); }
}
