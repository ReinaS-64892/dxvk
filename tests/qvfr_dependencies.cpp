#include "../src/util/util_qvfr_dependencies.h"

#include <cstdlib>
#include <iostream>
#include <map>

static void check(bool c) { if (!c) std::abort(); }

int main() {
  using dxvk::qvfr::ImageReadVisits;
  ImageReadVisits<> visits;
  auto first = visits.visit(16, 2, false); // Depth-only input attachment.
  check(first.aspects == 2 && !first.observe);
  auto sampled = visits.visit(16, 2, true); // Same depth as sampled image.
  check(!sampled.aspects && sampled.observe);
  auto stencil = visits.visit(16, 4, true); // Same image's other plane.
  check(stencil.aspects == 4 && !stencil.observe);
  auto storage = visits.visit(16, 6, false); // Conservative full-plane access.
  check(!storage.aspects && !storage.observe);
  auto other = visits.visit(32, 2, true);
  check(other.aspects == 2 && other.observe);
  // A new scan after a draw/write must visit the same image again.
  visits = {};
  first = visits.visit(16, 2, true);
  check(first.aspects == 2 && first.observe);

  // Collisions and saturation may cause redundant work, never lost planes or
  // lost learning. Compare effective work with an uncached reference stream.
  ImageReadVisits<8> crowded;
  std::map<uintptr_t, uint32_t> expected, actual;
  std::map<uintptr_t, bool> expectedSamples, actualSamples;
  for (uint32_t i = 0; i < 4000; i++) {
    uintptr_t image = (1 + ((i * 7) % 37)) * 128; // All keys collide.
    uint32_t aspects = 1u << (i % 3);
    if (!(i % 5)) aspects = 7; // Full storage access after narrow reads.
    bool observe = i % 4 != 0;
    expected[image] |= aspects;
    expectedSamples[image] |= observe;
    auto work = crowded.visit(image, aspects, observe);
    actual[image] |= work.aspects;
    actualSamples[image] |= work.observe;
  }
  check(expected == actual && expectedSamples == actualSamples);
  std::cout << "QVFR dependencies: planes, sampled/storage overlap, scan reset, collisions and saturation passed\n";
}
