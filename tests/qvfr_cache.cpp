#include "../src/util/util_qvfr_cache.h"
#include "../src/util/util_qvfr_unsafe.h"
#include "../src/util/util_qvfr_scene.h"

#include <cstdlib>
#include <iostream>

using namespace dxvk::qvfr;

static void checkAt(bool condition, int line) {
  if (!condition) {
    std::cerr << "qvfr cache check failed at line " << line << '\n';
    std::abort();
  }
}
#define check(condition) checkAt(condition, __LINE__)

int main() {
  check(directReplayInstances(false, false, true, true)); // Direct eye-local instancing.
  check(!directReplayInstances(false, false, true, false)); // Full-width stereo excluded.
  check(!directReplayInstances(false, false, false, true)); // Explicit opt-out.
  check(directReplayInstances(true, false, false, false)); // Legacy single-instance gate.
  check(directReplayInstances(false, true, false, false)); // Explicit legacy UNSAFE.
  ReplayTimingBudget timingBudget;
  for (uint32_t sample = 0; sample < 8; sample++) check(timingBudget.take());
  for (uint32_t sample = 0; sample < 1000; sample++) check(!timingBudget.take());
  check(timingBudget.samples == 8 && timingBudget.skipped == 1000);
  timingBudget = {}; // A new measured frame gets a fresh, bounded budget.
  check(timingBudget.take() && timingBudget.samples == 1 && !timingBudget.skipped);
  ReplayTimingBudget fullBudget;
  for (uint32_t i = 0; i < ReplayTimingBudget::FullLimit; i++) check(fullBudget.take(true));
  check(!fullBudget.take(true) && fullBudget.skipped == 1);
  check(fullBudget.samples == ReplayTimingBudget::FullLimit);
  check(fullInteropPublication(false, true, false)); // Generic external use.
  check(!fullInteropPublication(false, true, true)); // Known image, lazy DS.
  check(!fullInteropPublication(true, false, true)); // Known image, virtual.
  check(fullInteropPublication(false, false, true)); // Legacy eager mode.
  check(fullInteropPublication(true, true, false)); // Unknown dependencies.
  // A write mask with KEEP in all outcomes is a read-only stencil test.
  // Fail/depth-fail writes must still publish stencil, even if pass is KEEP.
  check(!stencilMayWrite(255, 0, 0, 0));
  check(!stencilMayWrite(0, 1, 2, 3));
  check(stencilMayWrite(255, 1, 0, 0));
  check(stencilMayWrite(255, 0, 2, 0));
  check(stencilMayWrite(255, 0, 0, 3));
  PendingPlanes readOnlyStencil;
  readOnlyStencil.draw(stencilMayWrite(255, 0, 0, 0));
  check(!readOnlyStencil.stencil);
  readOnlyStencil.draw(stencilMayWrite(255, 0, 2, 0));
  readOnlyStencil.draw(stencilMayWrite(255, 0, 0, 0));
  check(readOnlyStencil.stencil); // A later read does not erase earlier writes.
  SceneCycle resumed;
  check(!resumed.resume(10));
  resumed.rendered(10);
  check(!resumed.resume(10));
  check(resumed.finish(10));
  check(!resumed.resume(11)); // An explicit resume never crosses a frame.
  check(resumed.resume(10) && resumed.allows(10));
  check(!resumed.resume(10));
  check(resumed.finish(10) && !resumed.allows(10));
  check(resumed.allows(11) && !resumed.resume(11));
  SceneCycle scene;
  check(!scene.finish(10)); // Shadow/desktop work before the first scene.
  check(scene.allows(10));
  scene.rendered(10); // Left and right may both accumulate deferred draws.
  scene.rendered(10);
  check(scene.finish(10)); // First scene read/resolve/fallback closes it.
  check(!scene.allows(10)); // Later world/UI draws must stay native.
  check(!scene.finish(10)); // A second eye handoff does not reopen it.
  check(!scene.allows(10));
  check(scene.allows(11)); // Only the next endFrame resets the phase.
  check(!scene.finish(11)); // No scene was replayed in this frame yet.
  scene.rendered(11);
  check(scene.finish(11));
  check(!scene.allows(11));
  // Color resolve closes replay, but does not itself consume depth/stencil.
  // A later native depth read publishes depth; stencil remains available for
  // its own reader. A full clear next frame discards the obsolete remainder.
  PendingPlanes retained;
  SceneCycle lazyScene;
  retained.draw(true);
  lazyScene.rendered(20);
  check(lazyScene.finish(20));
  retained.complete(true, false);
  check(!retained.color && retained.depth && retained.stencil);
  check(!lazyScene.allows(20));
  retained.complete(false, true); // Native depth SRV/test needs current depth.
  check(!retained.depth && retained.stencil);
  check(!lazyScene.allows(20)); // Publishing a plane must not reopen replay.
  check(lazyScene.allows(21));
  retained.complete(false, true, true); // Full DS clear discards pending data.
  check(!retained.any());
  retained.draw(true);
  retained.complete(true, false); // No depth/stencil consumers this frame.
  check(retained.depth && retained.stencil);
  retained.complete(false, true, true); // Next full clear discards both planes.
  check(!retained.any());
  retained.draw(true);
  retained.complete(true, true, true); // OFF/external all-plane publication.
  check(!retained.any());
  for (const char* reason : {"external-transition", "external-acquire", "external-release",
      "external-rendering", "interop-flush", "control-disable"})
    check(unsafePublishBoundary(reason));
  for (const char* reason : {"shader-image", "image-access", "fallback-color", "fallback-depth",
      "resolve-read", "copy-read", "readback", "cache-reuse", "shared-image", "virtual-evict", "frame-end"})
    check(!unsafePublishBoundary(reason));
  UnsafeTerminalCycle terminal;
  check(!terminal.startDraw(1)); // First use is unknown until the handoff.
  terminal.submitted(1);
  check(!terminal.startDraw(2)); // Intermediate images stay split.
  check(terminal.startDraw(1));
  check(terminal.startDraw(2)); // Finish subsequent passes normally too.
  terminal.submitted(2); // End this cycle, and learn the alternate eye image.
  check(!terminal.startDraw(3));
  check(terminal.startDraw(2));
  terminal.submitted(2);
  check(!terminal.startDraw(0));
  check(terminal.startDraw(1));
  for (uint64_t id = 3; id < 200; id++) terminal.submitted(id);
  check(terminal.images.size() == 128 && !terminal.normal);
  PendingPlanes pending;
  check(!pending.any());
  pending.draw();
  // Resolving color preserves depth and the ability to continue the atlas.
  pending.complete(true, false);
  check(!pending.color && pending.depth && pending.any());
  pending.complete(true, false); // Repeated reads require no new color work.
  check(!pending.color && pending.depth);
  pending.draw(); // A later draw dirties color again without reseeding depth.
  check(pending.color && pending.depth);
  pending.complete(false, true);
  check(pending.color && !pending.depth && pending.any());
  pending.complete(true, true); // Full dependency ends the continuation.
  check(!pending.any());
  pending.draw();
  pending.complete(true, false);
  pending.invalidateColor(); // Ordinary draw touched color, not depth.
  check(!pending.color && pending.depth && pending.colorStale);
  pending.refreshColor(); // Reseed only color; retain the pending depth tiles.
  check(!pending.color && pending.depth && !pending.colorStale);
  pending.draw();
  check(pending.color && pending.depth && !pending.colorStale);
  pending.complete(true, false);
  pending.invalidateColor();
  pending.complete(false, true); // Actual depth dependency ends continuation.
  check(!pending.any());
  pending.refreshColor(); // A full seed clears invalidation on the next group.
  check(!pending.colorStale);
  // Stencil survives color/depth reads and ordinary non-stencil draws.
  pending.draw(true);
  check(pending.color && pending.depth && pending.stencil);
  pending.complete(true, true);
  check(!pending.color && !pending.depth && pending.stencil && pending.any());
  pending.draw();
  check(pending.stencil);
  pending.complete(false, false, true);
  check(pending.color && pending.depth && !pending.stencil);
  pending.complete(true, true, true);
  check(!pending.any());
  // A non-stencil fallback may change original depth while deferred stencil
  // survives. Refresh color/depth independently before continuing that atlas.
  pending.draw(true);
  pending.complete(true, true);
  pending.invalidateColor();
  pending.invalidateDepth();
  check(pending.stencil && pending.any() && pending.colorStale && pending.depthStale);
  pending.refreshColor();
  check(!pending.colorStale && pending.depthStale && pending.stencil);
  pending.refreshDepth();
  check(!pending.depthStale && pending.stencil);
  pending.draw();
  check(pending.color && pending.depth && pending.stencil && !pending.depthStale);
  pending.complete(true, true, true);
  check(!pending.any());
  // A stencil-only clear preserves color/depth; a depth-only clear preserves
  // stencil. Only clearing all planes ends a retained continuation.
  pending.draw(true);
  pending.complete(false, false, true);
  check(pending.color && pending.depth && !pending.stencil);
  pending.draw(true);
  pending.complete(false, true);
  check(pending.color && !pending.depth && pending.stencil);
  pending.complete(true, false);
  check(!pending.color && !pending.depth && pending.stencil);
  pending.invalidateColor();
  pending.refreshColor();
  check(pending.stencil && !pending.colorStale);
  pending.complete(true, true, true);
  check(!pending.any());
  std::array<CacheSlot, 4> slots = {};
  // Alternating eyes retain separate scratch images for the same attachments.
  auto left = chooseDeferredSlot(slots, 100, 200, 0);
  slots[left] = {100, 200, 0, 1, true};
  auto right = chooseDeferredSlot(slots, 100, 200, 1);
  check(left != right);
  slots[right] = {100, 200, 1, 2, true};
  for (unsigned i = 0; i < 100; i++) {
    check(chooseDeferredSlot(slots, 100, 200, 0) == left);
    check(chooseDeferredSlot(slots, 100, 200, 1) == right);
  }
  // An unrelated target uses a free slot, leaving both eyes pending.
  auto other = chooseDeferredSlot(slots, 300, 400, 0);
  check(other != left && other != right);
  slots[other] = {300, 400, 0, 3, true};
  // Reading a shared color OR depth requires both eyes, not the other target.
  for (auto image : {uintptr_t(100), uintptr_t(200)}) {
    check(slots[left].references(image));
    check(slots[right].references(image));
    check(!slots[other].references(image));
  }
  for (const auto& slot : slots) {
    check(!slot.references(0));
    check(!slot.references(999));
  }
  auto last = chooseDeferredSlot(slots, 500, 600, 0);
  slots[last] = {500, 600, 0, 4, true};
  check(chooseDeferredSlot(slots, 700, 800, 0) == left); // Full: oldest.
  slots[left].lastUse = 5;
  check(chooseDeferredSlot(slots, 700, 800, 0) == right);
  // A recently composed slot is cheaper than evicting any dirty group.
  slots[last].dirty = false;
  check(!slots[last].references(500));
  check(chooseDeferredSlot(slots, 700, 800, 0) == last);
  // Exact matches survive even with a clean alternative available.
  check(chooseDeferredSlot(slots, 100, 200, 1) == right);
  check(chooseDeferredSlot(slots, 500, 600, 0) == last);
  std::cout << "qvfr deferred cache checks passed\n";
}
