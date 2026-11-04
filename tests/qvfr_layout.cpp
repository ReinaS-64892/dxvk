#include "../src/util/util_qvfr.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace dxvk::qvfr;

// Do not use assert: these checks must also run in release builds.
static void checkAt(bool condition, int line) {
  if (!condition) {
    std::cerr << "qvfr layout check failed at line " << line << '\n';
    std::abort();
  }
}
#define check(condition) checkAt(condition, __LINE__)

static void near(double a, double b) { check(std::abs(a - b) < 1e-9); }

static void verify(Extent extent, StereoLayout layout, double scale,
    double focus, std::array<Gaze, 2> gaze, double focusScale = 1.0) {
  auto result = makePlan(extent, layout, scale, focus, gaze, focusScale);
  check(bool(result));
  auto p = *result;
  check(p.peripheral.width <= extent.width && p.peripheral.height <= extent.height);
  check(p.focus.width <= extent.width && p.focus.height <= extent.height);
  check(p.peripheral.layers == extent.layers && p.focus.layers == extent.layers);
  for (auto e : p.eyes) {
    auto atlas = scratchAtlas(e);
    check(atlas.peripheral.x == 0 && atlas.peripheral.y == 0);
    check(atlas.focus.x == atlas.peripheral.width && atlas.focus.y == 0);
    check(atlas.focus.x + atlas.focus.width == atlas.extent.width);
    check(atlas.peripheral.height <= atlas.extent.height);
    check(atlas.focus.height <= atlas.extent.height);
    check(atlas.extent.layers == 1);
    for (auto thickness : {1u, 8u, 100000u}) {
      for (auto border : outline(e.composite, thickness)) {
        check(border.x >= e.composite.x && border.y >= e.composite.y);
        check(border.x + border.width <= e.composite.x + e.composite.width);
        check(border.y + border.height <= e.composite.y + e.composite.height);
        check(border.layer == e.composite.layer);
        check(border.width > 0 && border.height > 0);
      }
    }
    check(e.composite.x >= e.original.x && e.composite.y >= e.original.y);
    check(e.composite.x + e.composite.width <= e.original.x + e.original.width);
    check(e.composite.y + e.composite.height <= e.original.y + e.original.height);
    check(e.focus.width == uint32_t(std::ceil(e.composite.width * focusScale)));
    check(e.focus.height == uint32_t(std::ceil(e.composite.height * focusScale)));
    check(e.focus.width > 0 && e.focus.height > 0);
    check(e.peripheral.width > 0 && e.peripheral.height > 0);
    check(e.focus.x + e.focus.width <= p.focus.width);
    check(e.peripheral.x + e.peripheral.width <= p.peripheral.width);
    check(e.original.layer == e.composite.layer && e.focus.layer == e.original.layer);
    double left = 2.0 * (e.composite.x - e.original.x) / e.original.width - 1;
    double right = left + 2.0 * e.composite.width / e.original.width;
    double top = 1 - 2.0 * e.composite.y / e.original.height;
    double bottom = top - 2.0 * e.composite.height / e.original.height;
    // Crop edges map to the new clip boundary for any homogeneous w.
    for (double w : {0.125, 1.0, 23.0}) {
      near((e.clip.scaleX * left * w + e.clip.offsetX * w) / w, -1);
      near((e.clip.scaleX * right * w + e.clip.offsetX * w) / w, 1);
      near((e.clip.scaleY * top * w + e.clip.offsetY * w) / w, 1);
      near((e.clip.scaleY * bottom * w + e.clip.offsetY * w) / w, -1);
    }
    // A full per-eye scissor must cover each scratch image exactly.
    for (bool focusPass : {false, true}) {
      auto r = replayRaster(e, e.original, focusPass);
      auto size = focusPass ? e.focus : e.peripheral;
      auto packed = replayAtlasRaster(e, e.original, focusPass);
      auto tile = focusPass ? atlas.focus : atlas.peripheral;
      auto ordinary = replayAtlasViewport(e, e.original, focusPass,
        e.original.x, e.original.y + e.original.height, e.original.width, -double(e.original.height));
      near(ordinary.x, packed.x); near(ordinary.y, packed.y);
      near(ordinary.width, packed.width); near(ordinary.height, packed.height);
      // A full-width viewport keeps the original screen UV for either eye,
      // including a focus crop away from the center. Rasterization is clipped
      // to this tile, while the viewport itself spans the entire stereo image.
      auto wide = replayAtlasViewport(e, e.original, focusPass,
        0, extent.height, extent.width, -double(extent.height));
      auto region = focusPass ? e.composite : e.original;
      for (double t : {.01, .25, .5, .99}) {
        double px = region.x + t * region.width;
        double py = region.y + t * region.height;
        double u = px / extent.width;
        double v = (double(extent.height) - py) / extent.height;
        near(wide.x + u * wide.width, tile.x + t * tile.width);
        near(wide.y + v * wide.height, tile.y + t * tile.height);
      }
      check(wide.scissor.x == packed.scissor.x && wide.scissor.y == packed.scissor.y);
      check(wide.scissor.width == packed.scissor.width && wide.scissor.height == packed.scissor.height);
      near(packed.x - tile.x, r.x);
      near(packed.y - tile.y, r.y);
      near(packed.width, r.width);
      near(packed.height, r.height);
      check(packed.scissor.x == tile.x && packed.scissor.y == tile.y);
      check(packed.scissor.width == tile.width && packed.scissor.height == tile.height);
      check(r.scissor.x == 0 && r.scissor.y == 0);
      check(r.scissor.width == size.width && r.scissor.height == size.height);
      // Original normalized device coordinates map to the expected crop pixels.
      for (double u : {0.0, .25, .5, .75, 1.0}) {
        double x = r.x + u * r.width;
        double y = r.y + (1-u) * r.height;
        if (focusPass) {
          near(x, (double(e.original.x) + u * e.original.width - e.composite.x) * e.focus.width / e.composite.width);
          near(y, (u * e.original.height - e.composite.y) * e.focus.height / e.composite.height);
        } else {
          near(x, u * e.peripheral.width);
          near(y, u * e.peripheral.height);
        }
      }
    }
  }
  if (layout == StereoLayout::DoubleWide) {
    check(p.eyes[0].focus.x + p.eyes[0].focus.width == p.eyes[1].focus.x);
    check(p.eyes[0].peripheral.x + p.eyes[0].peripheral.width == p.eyes[1].peripheral.x);
  } else {
    check(p.eyes[0].focus.layer == 0 && p.eyes[1].focus.layer == 1);
  }
}

int main() {
  check(isEyeViewport({5184, 2592, 1}, 0, 2592, 2592, -2592));
  check(isEyeViewport({5184, 2592, 1}, 2592, 2592, 2592, -2592));
  check(!isEyeViewport({5184, 2592, 1}, 0, 2592, 5184, -2592));
  check(!isEyeViewport({5184, 2592, 1}, 1, 2592, 2592, -2592));
  check(!isEyeViewport({5184, 2592, 1}, 0, 2592, 2592, 2592));
  // Moving gaze, including clamping at eye boundaries, changes only the
  // source crop. The atlas allocation key must keep the same dimensions.
  auto centered = makeScenePlan({5184, 2592, 1}, .5, .25, {});
  check(bool(centered));
  for (double u : {0.0, .1, .5, .9, 1.0})
    for (double v : {0.0, .1, .5, .9, 1.0}) {
      auto moved = makeScenePlan({5184, 2592, 1}, .5, .25, {{{u, v}, {1-u, 1-v}}});
      check(bool(moved));
      for (uint32_t eye = 0; eye < 2; eye++) {
        auto a = scratchAtlas(centered->eyes[eye]);
        auto b = scratchAtlas(moved->eyes[eye]);
        check(a.extent.width == b.extent.width && a.extent.height == b.extent.height);
        check(a.peripheral.width == b.peripheral.width && a.peripheral.height == b.peripheral.height);
        check(a.focus.width == b.focus.width && a.focus.height == b.focus.height);
        if (u == 0 && v == 0)
          check(centered->eyes[eye].composite.x != moved->eyes[eye].composite.x
            && centered->eyes[eye].composite.y != moved->eyes[eye].composite.y);
      }
    }
  OutlineCycle marker;
  check(marker.firstDraw(0));
  check(!marker.firstDraw(0)); // More scene passes and target changes.
  check(marker.firstDraw(1));
  check(!marker.firstDraw(1));
  marker.noteSubmission();
  marker.noteSubmission(); // Both eyes submitted without intervening draws.
  check(marker.firstDraw(1)); // Next cycle may draw the right eye first.
  check(!marker.firstDraw(1));
  check(marker.firstDraw(0));
  check(!marker.firstDraw(0));
  marker.noteSubmission();
  check(marker.firstDraw(0)); // No desktop Present is needed to rearm.
  check(marker.firstDraw(1));

  check(!parseUint({}, 10));
  check(parseUint("0", 10) == 0);
  check(parseUint("10", 10) == 10);
  for (auto text : {"-1", "+1", " 1", "1 ", "1.0", "11", "4294967296"})
    check(!parseUint(text, 10));
  check(bool(parseSize("4096x2048")));
  for (auto text : {"", "0x2", "2x0", "2X2", "2x2x2", "16385x2", "2x-2"})
    check(!parseSize(text));

  std::array<Gaze, 2> center {};
  auto p = makePlan({4000, 2000, 1}, StereoLayout::DoubleWide, 0.5, 0.25, center);
  check(bool(p));
  check(p->peripheral.width == 2000 && p->peripheral.height == 1000);
  check(p->focus.width == 1000 && p->focus.height == 500);
  check(p->eyes[0].composite.x == 750 && p->eyes[1].composite.x == 2750);
  near(p->eyes[0].clip.scaleX, 4);
  near(p->eyes[0].clip.offsetX, 0);
  near(p->eyes[0].clip.offsetY, 0);
  auto eye = p->eyes[1];
  auto partial = replayRaster(eye, {2800, 800, 100, 200, 0}, true);
  check(partial.scissor.x == 50 && partial.scissor.y == 50);
  check(partial.scissor.width == 100 && partial.scissor.height == 200);
  auto packedPartial = replayAtlasRaster(eye, {2800, 800, 100, 200, 0}, true);
  check(packedPartial.scissor.x == 1050 && packedPartial.scissor.y == 50);
  check(packedPartial.scissor.width == 100 && packedPartial.scissor.height == 200);
  near(packedPartial.x, 250);
  near(packedPartial.y, 1250);
  for (Rect outside : {Rect{0, 0, 1, 1, 0}, {3999, 1999, 1, 1, 0},
                       {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0}}) {
    auto empty = replayRaster(eye, outside, true);
    check(empty.scissor.width == 0 || empty.scissor.height == 0);
    check(empty.scissor.x <= eye.focus.width && empty.scissor.y <= eye.focus.height);
    auto packed = replayAtlasRaster(eye, outside, true);
    auto tile = scratchAtlas(eye).focus;
    check(packed.scissor.width == 0 || packed.scissor.height == 0);
    check(packed.scissor.x >= tile.x && packed.scissor.x <= tile.x + tile.width);
    check(packed.scissor.y >= tile.y && packed.scissor.y <= tile.y + tile.height);
  }

  for (auto layout : {StereoLayout::DoubleWide, StereoLayout::Array})
    for (uint32_t w : {1u, 3u, 1001u, 8192u})
      for (uint32_t h : {1u, 3u, 1501u, 16384u})
        for (double s : {0.001, 0.3, 0.5, 1.0})
          for (double f : {0.001, 0.25, 0.7, 1.0})
            for (double u : {0.0, 0.01, 0.5, 0.99, 1.0})
              for (double v : {0.0, 0.01, 0.5, 0.99, 1.0})
                verify({layout == StereoLayout::DoubleWide ? w * 2 : w,
                  h, layout == StereoLayout::DoubleWide ? 1u : 2u},
                  layout, s, f, {{{u, v}, {1-u, 1-v}}});

  // Screen coverage and gaze mapping must not change with focus density.
  for (double density : {0.001, 0.125, 0.5, 0.75, 1.0})
    for (double coverage : {0.001, 0.0625, 0.125, 0.25, 1.0})
      for (double low : {0.001, 0.0625, 0.125, 0.25})
        for (auto gaze : {center, std::array<Gaze, 2>{{{0, 0}, {1, 1}}}}) {
          verify({5184, 2592, 1}, StereoLayout::DoubleWide, low, coverage, gaze, density);
          auto base = makePlan({5184, 2592, 1}, StereoLayout::DoubleWide, low, coverage, gaze);
          auto scaled = makePlan({5184, 2592, 1}, StereoLayout::DoubleWide, low, coverage, gaze, density);
          for (uint32_t e = 0; e < 2; e++) {
            check(base->eyes[e].composite.x == scaled->eyes[e].composite.x);
            check(base->eyes[e].composite.y == scaled->eyes[e].composite.y);
            check(base->eyes[e].composite.width == scaled->eyes[e].composite.width);
            check(base->eyes[e].composite.height == scaled->eyes[e].composite.height);
          }
        }
  for (Extent bad : {Extent{0, 1, 1}, {2, 0, 1}, {3, 2, 1}, {2, 2, 2}, {16386, 2, 1}})
    check(!makePlan(bad, StereoLayout::DoubleWide, .5, .25, center));
  check(!makePlan({2, 2, 1}, StereoLayout::Array, .5, .25, center));
  double nan = std::numeric_limits<double>::quiet_NaN();
  double inf = std::numeric_limits<double>::infinity();
  for (double bad : {-1.0, 0.0, 1.01, nan, inf}) {
    check(!makePlan({4, 2, 1}, StereoLayout::DoubleWide, bad, .25, center));
    check(!makePlan({4, 2, 1}, StereoLayout::DoubleWide, .5, bad, center));
    check(!makePlan({4, 2, 1}, StereoLayout::DoubleWide, .5, .25, center, bad));
  }
  for (double bad : {-0.01, 1.01, nan, inf}) {
    check(!makePlan({4, 2, 1}, StereoLayout::DoubleWide, .5, .25, {{{bad, .5}, {.5, .5}}}));
    check(!makePlan({4, 2, 1}, StereoLayout::DoubleWide, .5, .25, {{{.5, .5}, {.5, bad}}}));
  }
  std::cout << "qvfr layout: 12800 plans and invalid inputs passed\n";
}
