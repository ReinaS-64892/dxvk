#include "../src/util/util_qvfr_grab.h"
#include <cstdlib>
#include <iostream>
static void check(bool c) { if (!c) std::abort(); }
int main() {
  using namespace dxvk::qvfr;
  GrabProvenance p;
  GrabToken root{10, 7, 120, 0};
  check(p.link(20, 200, 120, root)); // Resolve -> copy/region transfer.
  auto copy = p.find(20, 200, 120);
  check(copy && copy->root == 10 && copy->generation == 7 && copy->hops == 1);
  check(p.link(30, 300, 120, *copy)); // Copy -> blit/fullscreen shader.
  auto shader = p.find(30, 300, 120);
  check(shader && shader->root == 10 && shader->hops == 2);
  check(!p.find(30, 301, 120)); // Renamed image storage must not inherit provenance.
  check(!p.find(30, 300, 121)); // Scene learning only follows this frame's transfers.
  check(p.find(30, 300, 121, true).has_value()); // Later external handoff can blacklist root.
  p.invalidate(30);
  check(!p.find(30, 300, 120)); // Any unrelated overwrite drops lineage.
  check(!p.link(10, 100, 120, root)); // No root cycles/self transfer.
  check(!p.link(31, 310, 121, root)); // An old token cannot establish a new dependency.
  check(!p.link(32, 320, 120, GrabToken{10, 7, 120, GrabProvenance::MaxHops}));
  for (uint64_t i = 100; i < 400; i++) check(p.link(i, i, 120, root));
  check(p.nodes.size() == GrabProvenance::Limit);
  int exits = 0;
  auto earlyReturn = [&] { auto g = grabScopeExit([&] { exits++; }); return 1; };
  check(earlyReturn() == 1 && exits == 1); // HW/inline early returns still publish metadata.
  std::cout << "QVFR grab: copy/shader chain, generations, storage/frame isolation, external lookup, invalidation and bounds passed\n";
}
