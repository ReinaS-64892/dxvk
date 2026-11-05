#include "../src/util/util_qvfr_profile.h"
#include <cstdlib>
#include <iostream>

struct Clock {
  using time_point = std::chrono::time_point<Clock, std::chrono::milliseconds>;
  static inline int ticks = 0, reads = 0;
  static time_point now() { reads++; return time_point(std::chrono::milliseconds(ticks)); }
};
static void check(bool condition) { if (!condition) std::abort(); }
int main() {
  using Collector = dxvk::qvfr::CpuDetailCollector<3, Clock>;
  Collector c;
  { Collector::Scope disabled(&c, false, 120, 0); }
  { Collector::Scope unsampled(&c, true, 121, 0); }
  check(Clock::reads == 0);
  { Collector::Scope outer(&c, true, 120, 0);
    Clock::ticks = 2;
    { Collector::Scope child(&c, true, 120, 1);
      Clock::ticks = 3;
      { Collector::Scope grandchild(&c, true, 120, 2); Clock::ticks = 5; }
      Clock::ticks = 7;
    }
    Clock::ticks = 10;
  }
  check(!c.parent && c.frame == 120);
  check(c.totals[0].inclusiveMs == 10 && c.totals[0].exclusiveMs == 5);
  check(c.totals[1].inclusiveMs == 5 && c.totals[1].exclusiveMs == 3);
  check(c.totals[2].inclusiveMs == 2 && c.totals[2].exclusiveMs == 2);
  { Collector::Scope sibling(&c, true, 120, 1); Clock::ticks = 12; }
  check(c.totals[1].calls == 2 && c.totals[1].exclusiveMs == 5);
  { Collector::Scope nextFrame(&c, true, 240, 2); Clock::ticks = 13; }
  check(c.totals[0].calls == 0 && c.totals[1].calls == 0);
  check(c.totals[2].calls == 1 && c.totals[2].exclusiveMs == 1);
  { Collector::Scope early(&c, true, 240, 0);
    Clock::ticks = 15;
    early.finish();
    Clock::ticks = 20;
  }
  check(c.totals[0].calls == 1 && c.totals[0].exclusiveMs == 2 && !c.parent);
  std::cout << "QVFR CPU detail: nested exclusive timing, disabled/sampling, accumulation/reset passed\n";
}
