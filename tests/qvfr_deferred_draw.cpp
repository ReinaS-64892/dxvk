#include "../src/util/util_qvfr_deferred_draw.h"
#include <cstdlib>
#include <iostream>
#include <vector>
static void check(bool c) { if (!c) std::abort(); }
int main() {
  dxvk::qvfr::DeferredDrawSlot slot;
  std::vector<int> gpu;
  auto command = [&] (int id) { auto d = slot.take(); if (d.token) gpu.push_back(int(d.count)); gpu.push_back(id); };
  auto left = slot.store(true, 6, 10, -2);
  check(left && !slot.store(false, 3, 0, 0)); // Cannot silently replace pending work.
  check(!slot.cancel(left + 1));
  command(100); check(gpu == std::vector<int>({6, 100})); // Draw precedes a state-changing command.
  check(!slot.cancel(left)); // Already emitted: cancellation must fail.
  auto right = slot.store(false, 3, 7, 0);
  check(right != left && slot.cancel(right));
  command(200); check(gpu == std::vector<int>({6, 100, 200})); // Cancelled draw never emitted.
  auto token = slot.store(true, 6, 11, -7);
  auto draw = slot.take();
  check(draw.token == token && draw.indexed && draw.first == 11 && draw.vertexOffset == -7);
  check(!slot.take().token); // Finalization cannot emit the draw twice.
  std::cout << "QVFR deferred draw: command ordering, exact arguments, cancellation and single emission passed\n";
}
