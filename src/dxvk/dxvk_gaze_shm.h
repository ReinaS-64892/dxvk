// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gaze_bridge {
// ABI v1: 64 bytes, little endian, 32-bit lock-free atomic words.
// 0 magic, 1 version, 2 size, 3 sequence, 4 valid,
// 5..6 left/right U, 7..8 left/right V (top = 0), 9..15 reserved.
struct alignas(4) SharedGaze { uint32_t words[16]; };
static_assert(sizeof(SharedGaze) == 64);
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr));
constexpr uint32_t Magic = 0x325a4147; // "GAZ2"
constexpr uint32_t Version = 1;
struct Sample { bool valid = false; float u[2] = {0.5f, 0.5f}; float v[2] = {0.5f, 0.5f}; };
inline uint32_t bits(float f) { uint32_t n; std::memcpy(&n, &f, 4); return n; }
inline float value(uint32_t n) { float f; std::memcpy(&f, &n, 4); return f; }
inline uint32_t load(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void store(uint32_t* p, uint32_t n) { __atomic_store_n(p, n, __ATOMIC_SEQ_CST); }
// Single writer, enforced by an exclusive flock for the lifetime of its mapping.
inline void publish(SharedGaze* memory, const Sample& sample) {
  uint32_t seq = (load(&memory->words[3]) + 1u) | 1u;
  store(&memory->words[3], seq);
  store(&memory->words[0], Magic);
  store(&memory->words[1], Version);
  store(&memory->words[2], sizeof(SharedGaze));
  store(&memory->words[4], sample.valid ? 1 : 0);
  for (unsigned eye = 0; eye < 2; ++eye) {
    store(&memory->words[5 + eye], bits(sample.u[eye]));
    store(&memory->words[7 + eye], bits(sample.v[eye]));
  }
  for (unsigned i = 9; i < 16; ++i) store(&memory->words[i], 0);
  store(&memory->words[3], seq + 1u);
}
// Returns a coherent packet, including invalid packets (heartbeat).
inline bool snapshot(const SharedGaze* memory, Sample& sample, uint32_t& sequence) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    uint32_t first = load(&memory->words[3]);
    if (first & 1u) continue;
    uint32_t words[16];
    for (unsigned i = 0; i < 16; ++i) words[i] = load(&memory->words[i]);
    if (first != load(&memory->words[3])) continue;
    if (words[0] != Magic || words[1] != Version || words[2] != sizeof(SharedGaze)) return false;
    sequence = first;
    sample.valid = words[4] == 1;
    for (unsigned eye = 0; eye < 2; ++eye) {
      sample.u[eye] = value(words[5 + eye]);
      sample.v[eye] = value(words[7 + eye]);
      if (!std::isfinite(sample.u[eye]) || !std::isfinite(sample.v[eye]) ||
          sample.u[eye] < 0 || sample.u[eye] > 1 || sample.v[eye] < 0 || sample.v[eye] > 1)
        sample.valid = false;
    }
    return true;
  }
  return false;
}
// Require a sequence change after mapping: an old valid file is never accepted
// just because the reader has started. No cross-process clock comparison needed.
class Freshness {
public:
  using Clock = std::chrono::steady_clock;
  explicit Freshness(Clock::time_point now = Clock::now()) : changed(now) {}
  bool observe(uint32_t seq, Clock::time_point now) {
    if (!seen) { seen = true; sequence = seq; return false; }
    if (seq != sequence) { sequence = seq; changed = now; live = true; }
    return live && !expired(now);
  }
  bool expired(Clock::time_point now) const { return now - changed >= std::chrono::milliseconds(100); }
private:
  uint32_t sequence = 0;
  bool seen = false, live = false;
  Clock::time_point changed;
};
} // namespace gaze_bridge
