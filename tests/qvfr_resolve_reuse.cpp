#include "../src/util/util_qvfr_resolve_reuse.h"
#include <cstdlib>
#include <iostream>
static void check(bool c) { if (!c) std::abort(); }
int main() {
  using namespace dxvk::qvfr;
  ResolveReuseKey key{120, 10, 100, 20, 200, 97, 2, 5184, 2592};
  ResolveReuseHistory h;
  check(!h.contains(key)); h.record(key); check(h.contains(key));
  for (uint32_t field = 0; field < 7; field++) {
    auto changed = key;
    switch (field) {
      case 0: changed.frame++; break;
      case 1: changed.sourceStorage++; break;
      case 2: changed.destinationStorage++; break;
      case 3: changed.format++; break;
      case 4: changed.samples = 4; break;
      case 5: changed.width /= 2; break;
      case 6: changed.height /= 2; break;
    }
    check(!h.contains(changed));
  }
  h.invalidate(99); check(h.contains(key));
  h.invalidate(key.source); check(!h.contains(key));
  h.record(key); h.invalidate(key.destination); check(!h.contains(key));
  for (uint64_t i = 0; i < 80; i++) { auto k = key; k.source += i; h.record(k); check(h.contains(k)); }
  check(!h.contains(key)); // Bounded eviction requires a real resolve again.
  std::cout << "QVFR resolve reuse: frame/storage/format/region keys, write invalidation and bounded eviction passed\n";
}
