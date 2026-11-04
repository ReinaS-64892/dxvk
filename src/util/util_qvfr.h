#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace dxvk::qvfr {

  // Coordinates are top-left pixel coordinates, with exclusive right/bottom.
  struct Rect { uint32_t x, y, width, height, layer; };
  struct Extent { uint32_t width, height, layers; };
  inline bool isEyeViewport(Extent size, double x, double y, double width, double height) {
    return width == double(size.width / 2) && height == -double(size.height)
      && y == double(size.height) && (x == 0 || x == double(size.width / 2));
  }
  inline std::array<Rect, 4> outline(Rect r, uint32_t thickness) {
    uint32_t w = std::min(thickness, r.width);
    uint32_t h = std::min(thickness, r.height);
    return {{{r.x, r.y, r.width, h, r.layer},
             {r.x, r.y+r.height-h, r.width, h, r.layer},
             {r.x, r.y, w, r.height, r.layer},
             {r.x+r.width-w, r.y, w, r.height, r.layer}}};
  }
  // External reads for both eyes can repeat before scene rendering resumes.
  // Rearm lazily on the next scene draw, independently of desktop Present.
  struct OutlineCycle {
    std::array<bool, 2> drawn = {};
    bool submitted = false;

    void noteSubmission() { submitted = true; }

    bool firstDraw(uint32_t eye) {
      if (submitted) {
        drawn = {};
        submitted = false;
      }
      return !std::exchange(drawn[eye], true);
    }
  };

  enum class StereoLayout { DoubleWide, Array };
  struct Gaze { double u = 0.5, v = 0.5; };

  // Bridge UVs are top-left based; some scene render targets use the opposite Y.
  inline Gaze orientGaze(Gaze gaze, bool flipY) {
    return {gaze.u, flipY ? 1.0 - gaze.v : gaze.v};
  }

  // Applied to the per-eye clip position BEFORE any double-wide stereo packing:
  // x' = scaleX*x + offsetX*w; y' = scaleY*y + offsetY*w; z and w unchanged.
  // This is a geometric transform, not a guess at Unity constant-buffer offsets.
  struct ClipTransform { double scaleX, scaleY, offsetX, offsetY; };
  struct EyePlan {
    Rect original;
    Rect peripheral;
    Rect focus;
    Rect composite; // Focus destination in the original texture.
    ClipTransform clip;
  };
  struct Plan {
    Extent peripheral;
    Extent focus;
    std::array<EyePlan, 2> eyes;
  };

  struct ReplayRaster {
    double x, y, width, height;
    Rect scissor;
  };

  // Per-eye scratch targets start at (0,0). Preserve original pixel density
  // for focus by translating the full viewport; clip via the cropped scissor.
  inline ReplayRaster replayRaster(const EyePlan& eye, Rect scissor, bool focus) {
    Rect region = focus ? eye.composite : eye.original;
    auto intersect = [] (uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
      uint64_t lo = std::max(uint64_t(a), uint64_t(b));
      uint64_t hi = std::min(uint64_t(a) + n, uint64_t(b) + m);
      // An empty intersection must stay within the destination image.
      lo = std::clamp(lo, uint64_t(b), uint64_t(b) + m);
      hi = std::max(lo, hi);
      return std::array<uint64_t, 2>{lo-b, hi-b};
    };
    auto x = intersect(scissor.x, scissor.width, region.x, region.width);
    auto y = intersect(scissor.y, scissor.height, region.y, region.height);
    ReplayRaster r = {};
    uint64_t width = focus ? eye.focus.width : eye.peripheral.width;
    uint64_t height = focus ? eye.focus.height : eye.peripheral.height;
    r.x = double(int64_t(eye.original.x) - region.x) * width / region.width;
    r.y = double(int64_t(eye.original.height) - region.y) * height / region.height;
    r.width = double(eye.original.width) * width / region.width;
    r.height = -double(eye.original.height) * height / region.height;
    uint32_t loX = uint32_t(x[0] * width / region.width);
    uint32_t loY = uint32_t(y[0] * height / region.height);
    uint32_t hiX = uint32_t((x[1] * width + region.width - 1) / region.width);
    uint32_t hiY = uint32_t((y[1] * height + region.height - 1) / region.height);
    r.scissor = {loX, loY, hiX-loX, hiY-loY, 0};
    return r;
  }

  struct ScratchAtlas {
    Extent extent;
    Rect peripheral, focus;
  };

  // Two disjoint tiles in one attachment keep both draws in the same pass.
  // These are per-eye scratch coordinates, independent of stereo packing.
  inline ScratchAtlas scratchAtlas(const EyePlan& eye) {
    return {{eye.peripheral.width + eye.focus.width,
             std::max(eye.peripheral.height, eye.focus.height), 1},
            {0, 0, eye.peripheral.width, eye.peripheral.height, 0},
            {eye.peripheral.width, 0, eye.focus.width, eye.focus.height, 0}};
  }

  inline ReplayRaster replayAtlasRaster(const EyePlan& eye, Rect scissor, bool focus) {
    auto r = replayRaster(eye, scissor, focus);
    auto atlas = scratchAtlas(eye);
    auto tile = focus ? atlas.focus : atlas.peripheral;
    r.x += tile.x;
    r.y += tile.y;
    r.scissor.x += tile.x;
    r.scissor.y += tile.y;
    return r;
  }

  // Rebase an arbitrary original viewport into one atlas tile. In particular,
  // a full double-wide postprocess viewport must retain its full width: using
  // a per-eye viewport would repeat its left half in both eyes.
  inline ReplayRaster replayAtlasViewport(const EyePlan& eye, Rect scissor, bool focus,
      double x, double y, double width, double height) {
    auto r = replayAtlasRaster(eye, scissor, focus);
    auto atlas = scratchAtlas(eye);
    auto tile = focus ? atlas.focus : atlas.peripheral;
    auto region = focus ? eye.composite : eye.original;
    double sx = double(tile.width) / region.width;
    double sy = double(tile.height) / region.height;
    r.x = tile.x + (x - region.x) * sx;
    r.y = tile.y + (y - region.y) * sy;
    r.width = width * sx;
    r.height = height * sy;
    return r;
  }

  inline std::optional<uint32_t> parseUint(std::string_view text, uint32_t max) {
    if (text.empty())
      return std::nullopt;
    uint32_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc()
     || result.ptr != text.data() + text.size() || value > max)
      return std::nullopt;
    return value;
  }

  inline std::optional<Extent> parseSize(std::string_view text) {
    auto sep = text.find('x');
    if (sep == std::string_view::npos)
      return std::nullopt;
    auto w = parseUint(text.substr(0, sep), 16384);
    auto h = parseUint(text.substr(sep + 1), 16384);
    if (!w || !h || !*w || !*h)
      return std::nullopt;
    return Extent { *w, *h, 1 };
  }

  // An explicit layout is mandatory: dimensions alone cannot identify stereo.
  // focusFraction is screen coverage; focusScale controls its pixel density.
  inline std::optional<Plan> makePlan(Extent original, StereoLayout layout,
      double peripheralScale, double focusFraction, std::array<Gaze, 2> gaze, double focusScale = 1.0) {
    if (!original.width || !original.height || original.width > 16384 || original.height > 16384
     || !std::isfinite(peripheralScale) || peripheralScale <= 0 || peripheralScale > 1
     || !std::isfinite(focusFraction) || focusFraction <= 0 || focusFraction > 1
     || !std::isfinite(focusScale) || focusScale <= 0 || focusScale > 1)
      return std::nullopt;

    bool wide = layout == StereoLayout::DoubleWide;
    if ((wide && (original.layers != 1 || original.width % 2))
     || (!wide && original.layers != 2))
      return std::nullopt;

    uint32_t eyeWidth = wide ? original.width / 2 : original.width;
    // Round per eye so that scaling never changes the eye boundary.
    uint32_t lowWidth = uint32_t(std::ceil(eyeWidth * peripheralScale));
    uint32_t lowHeight = uint32_t(std::ceil(original.height * peripheralScale));
    uint32_t focusWidth = uint32_t(std::ceil(eyeWidth * focusFraction));
    uint32_t focusHeight = uint32_t(std::ceil(original.height * focusFraction));
    uint32_t focusPixelsWidth = uint32_t(std::ceil(focusWidth * focusScale));
    uint32_t focusPixelsHeight = uint32_t(std::ceil(focusHeight * focusScale));
    Plan plan = {};
    plan.peripheral = { wide ? 2 * lowWidth : lowWidth, lowHeight, original.layers };
    plan.focus = { wide ? 2 * focusPixelsWidth : focusPixelsWidth, focusPixelsHeight, original.layers };

    for (uint32_t eye = 0; eye < 2; eye++) {
      auto g = gaze[eye];
      if (!std::isfinite(g.u) || !std::isfinite(g.v) || g.u < 0 || g.u > 1 || g.v < 0 || g.v > 1)
        return std::nullopt;
      uint32_t layer = wide ? 0 : eye;
      uint32_t x = uint32_t(std::clamp(std::round(g.u * eyeWidth - focusWidth * 0.5),
        0.0, double(eyeWidth - focusWidth)));
      uint32_t y = uint32_t(std::clamp(std::round(g.v * original.height - focusHeight * 0.5),
        0.0, double(original.height - focusHeight)));
      auto& p = plan.eyes[eye];
      p.original = { wide ? eye * eyeWidth : 0, 0, eyeWidth, original.height, layer };
      p.peripheral = { wide ? eye * lowWidth : 0, 0, lowWidth, lowHeight, layer };
      p.focus = { wide ? eye * focusPixelsWidth : 0, 0, focusPixelsWidth, focusPixelsHeight, layer };
      p.composite = { p.original.x + x, y, focusWidth, focusHeight, layer };
      p.clip = { double(eyeWidth) / focusWidth, double(original.height) / focusHeight,
        double(int64_t(eyeWidth) - 2 * int64_t(x) - focusWidth) / focusWidth,
        double(2 * int64_t(y) + focusHeight - int64_t(original.height)) / focusHeight };
    }
    return plan;
  }

  // Default hypothesis: the rendered image's Y orientation is opposite to
  // the bridge's top-left gaze UV. Apply once at the scene boundary; retain
  // raw bridge gaze separately so later passes never flip an already-flipped
  // gaze. This is a policy, not a detected property of Unity or Submit.
  inline std::optional<Plan> makeScenePlan(Extent size, double scale, double focus,
      std::array<Gaze, 2> bridgeGaze, double focusScale = 1.0) {
    for (auto& gaze : bridgeGaze)
      gaze = orientGaze(gaze, true);
    return makePlan(size, StereoLayout::DoubleWide, scale, focus, bridgeGaze, focusScale);
  }

  // Unmeasured output passes use the same default absolute orientation as
  // the scene, derived from raw gaze rather than reflecting the source again.
  inline std::optional<Plan> makeOutputPlan(Extent size, double scale, double focus,
      std::array<Gaze, 2> bridgeGaze) {
    return makeScenePlan(size, scale, focus, bridgeGaze);
  }

}
