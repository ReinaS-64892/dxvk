#include "../src/util/util_qvfr_stale_resolve.h"
#include <cstdlib>
#include <iostream>
static void check(bool condition) { if (!condition) std::abort(); }
int main() {
  using dxvk::qvfr::StaleResolvePolicy;
  using dxvk::qvfr::staleResolveReaderGate;
  // Same image IDs with different views no longer depend on RTV/DSV equality.
  check(!staleResolveReaderGate(false, true, true, true, 10, 20, 10, 20));
  // Color ping-pong keeps the scene depth: this is the missing learning path.
  check(!staleResolveReaderGate(false, true, true, true, 10, 20, 11, 20));
  check(staleResolveReaderGate(false, true, true, true, 10, 20, 11, 21));
  check(staleResolveReaderGate(true, true, true, true, 10, 20, 11, 20));
  check(staleResolveReaderGate(false, false, true, true, 10, 20, 11, 20));
  check(staleResolveReaderGate(false, true, false, true, 10, 20, 11, 20));
  check(staleResolveReaderGate(false, true, true, false, 10, 20, 11, 20));
  check(staleResolveReaderGate(false, true, true, true, 10, 0, 11, 0));
  StaleResolvePolicy p;
  check(!p.skip(120, 8, true)); // Unknown destination must really resolve.
  p.refreshed(120);
  check(!p.skip(121, 8, true)); // A resolve alone does not prove a scene reader.
  p.sceneReader = true;
  check(!p.skip(120, 8, true)); // Even after learning, do not skip refresh frame.
  check(!p.skip(119, 8, true)); // No unsigned wrap/backward-frame reuse.
  for (uint32_t interval = 1; interval <= 120; interval++) {
    for (uint64_t age = 0; age <= interval + 1; age++) {
      check(p.skip(120 + age, interval, true) == (age > 0 && age < interval));
      check(!p.skip(120 + age, interval, false)); // Closed/no pending scene.
    }
  }
  p.external = true;
  check(!p.skip(121, 8, true));
  p.refreshed(200);
  check(!p.skip(201, 8, true)); // Refresh must never remove the external blacklist.
  p = {};
  check(!p.skip(201, 8, true)); // Reallocated/changed source requires relearning.
  p.refreshed(200); p.sceneReader = true;
  check(p.skip(207, 8, true) && !p.skip(208, 8, true));
  p.refreshed(208);
  check(p.skip(209, 8, true));
  p.observeStorage(1, 2, true, 208, 208); // Real resolve renamed storage this frame.
  check(p.valid && p.sceneReader && p.refreshedFrame == 208);
  p.observeStorage(2, 3, false, 208, 209); // Unrelated rename must not be treated as initialized.
  check(!p.valid && !p.sceneReader && !p.skip(210, 8, true));
  p.refreshed(210); p.sceneReader = true;
  p.observeStorage(3, 4, true, 210, 211); // An old pending refresh cannot initialize new storage.
  check(!p.valid && !p.sceneReader);
  p.external = true;
  p.observeStorage(4, 5, true, 212, 212);
  check(p.valid && p.external && !p.skip(213, 8, true));
  std::cout << "QVFR stale resolve: warmup, refresh boundaries, all intervals, external/closed exclusion passed\n";
}
