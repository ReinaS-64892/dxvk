#include "../src/util/util_qvfr_hdr_return.h"
#include <cstdlib>
#include <iostream>
static void check(bool c) { if (!c) std::abort(); }
int main() {
  using namespace dxvk::qvfr;
  HdrReturnHistory history;
  HdrReturnPath left{120, 10, 100, 20, 200, 30, 300, 0}, right = left;
  right.eye = 1;
  check(!history.consume(120, 10, 100, 20, 200, 0)); // Unknown pair.
  check(history.record(left));
  check(!history.consume(120, 10, 100, 20, 200, 1)); // Other eye not captured.
  history.invalidate(10, &right); // Valid first leg writes only the right half.
  check(history.record(right));
  check(!history.consume(121, 10, 100, 20, 200, 0)); // Never reuse a prior frame.
  check(!history.consume(120, 10, 101, 20, 200, 0)); // Temporary renamed.
  check(!history.consume(120, 10, 100, 20, 201, 0)); // Original renamed.
  check(!history.consume(120, 10, 100, 21, 200, 0)); // Different original.
  check(history.consume(120, 10, 100, 20, 200, 0));
  check(!history.consume(120, 10, 100, 20, 200, 0)); // At most one return per capture.
  check(history.consume(120, 10, 100, 20, 200, 1));
  for (uint64_t overwritten : {10u, 20u, 30u}) {
    history = {}; check(history.record(left)); check(history.record(right));
    history.invalidate(overwritten);
    check(!history.consume(120, 10, 100, 20, 200, 0));
    check(!history.consume(120, 10, 100, 20, 200, 1));
  }
  history = {}; check(history.record(left)); check(history.record(right));
  history.invalidate(10, &left); // Recapture one eye without losing the other.
  check(!history.consume(120, 10, 100, 20, 200, 0));
  check(history.consume(120, 10, 100, 20, 200, 1));
  check(history.record(left)); check(history.consume(120, 10, 100, 20, 200, 0));
  history = {};
  for (uint32_t i = 0; i < 4; i++) { auto p = left; p.temporary += i; check(history.record(p)); }
  auto extra = left; extra.temporary = 99; check(!history.record(extra)); // Bound, no eviction of live pairs.
  extra.frame++; check(history.record(extra)); // New frame can reclaim stale slots.
  std::cout << "QVFR HDR return: stereo capture, overwrite/rename/frame exclusion, single consumption and bounds passed\n";
}
