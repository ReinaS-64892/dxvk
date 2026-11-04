#include "../src/util/util_qvfr_gaze_shm.h"
#include "../src/util/util_qvfr.h"

#include <cstdlib>
#include <iostream>
#include <limits>

static void check(bool ok) { if (!ok) std::abort(); }

int main() {
  using namespace dxvk::qvfr;
  for (auto gaze : {Gaze{.125, 0}, Gaze{.875, .25}, Gaze{.5, .5}, Gaze{.25, 1}}) {
    auto same = orientGaze(gaze, false);
    auto flipped = orientGaze(gaze, true);
    auto twice = orientGaze(flipped, true);
    check(same.u == gaze.u && same.v == gaze.v);
    check(flipped.u == gaze.u && flipped.v == 1 - gaze.v);
    check(twice.u == gaze.u && twice.v == gaze.v);
  }
  auto normal = makePlan({5184, 2592, 1}, StereoLayout::DoubleWide, .5, .25,
    {Gaze{.25, .25}, Gaze{.75, .25}});
  auto flipped = makePlan({5184, 2592, 1}, StereoLayout::DoubleWide, .5, .25,
    {orientGaze({.25, .25}, true), orientGaze({.75, .25}, true)});
  check(normal && flipped);
  for (unsigned eye = 0; eye < 2; eye++) {
    auto a = normal->eyes[eye].composite;
    auto b = flipped->eyes[eye].composite;
    check(a.x == b.x && a.width == b.width && a.height == b.height);
    check(a.y + b.y + a.height == 2592);
  }
  using namespace gaze_bridge;
  SharedGaze memory = {};
  Sample sample, read;
  uint32_t seq = 0;
  check(!snapshot(&memory, read, seq));
  sample.valid = true;
  sample.u[0] = .2f; sample.u[1] = .8f;
  publish(&memory, sample);
  check(snapshot(&memory, read, seq) && read.valid);
  check(read.u[0] == .2f && read.u[1] == .8f);
  auto start = Freshness::Clock::now();
  Freshness freshness(start);
  check(!freshness.observe(seq, start)); // Existing valid packets are not live.
  check(!freshness.observe(seq, start + std::chrono::milliseconds(1)));
  publish(&memory, sample);
  check(snapshot(&memory, read, seq));
  check(freshness.observe(seq, start + std::chrono::milliseconds(2)));
  check(!freshness.expired(start + std::chrono::milliseconds(101)));
  check(freshness.expired(start + std::chrono::milliseconds(102)));
  check(!freshness.observe(seq, start + std::chrono::milliseconds(102)));
  store(&memory.words[3], seq | 1u);
  check(!snapshot(&memory, read, seq)); // Writer interrupted mid-packet.
  publish(&memory, sample);
  sample.v[1] = std::numeric_limits<float>::quiet_NaN();
  publish(&memory, sample);
  check(snapshot(&memory, read, seq) && !read.valid);
  std::cout << "qvfr gaze Y orientation, stereo crop, ABI, heartbeat and invalid-packet checks passed\n";
}
