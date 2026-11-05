#pragma once
#include <optional>
#include <string_view>
#include <string>
#include <set>
#include <tuple>
#include "util_qvfr.h"

namespace dxvk::qvfr {
  inline std::optional<bool> parseControl(std::string_view value) {
    auto space = [] (char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    if (value == "on" || value == "1") return true;
    if (value == "off" || value == "0") return false;
    return std::nullopt;
  }

  struct ControlSettings {
    bool profileDetails = false, sceneResumeAfterResolve = false, unsafeSceneContinue = false;
    bool unsafeSkipHdrReturn = false;
    bool resolveReuse = false, unsafeDeferHdrForward = false;
    uint32_t unsafeResolveRefreshFrames = 8;
    bool enabled = true, profile = false, orientationTrace = false, outline = true;
    bool stencil = true, safeInstancing = true, lazyDepthStencil = true, stereoAtlas = true;
    bool unsafeSkipImageSeed = false, unsafeSkipStencilSeed = false, unsafeSkipStencilCompose = false;
    bool unsafeSkipDepthCompose = false, colorSinglePass = true;
    uint32_t colorFadePixels = 0;
    Extent size = {5184, 2592, 1};
    uint32_t samples = 2, format = 97;
    double scale = .5, focus = .25, focusScale = 1.0;
    std::string profilePasses = "sampled";
    std::string gaze = "shared", gazeFile = "Z:/dev/shm/vrchat_dfr_gaze";
    auto values() const {
      return std::tie(enabled, profile, profileDetails, sceneResumeAfterResolve, unsafeSceneContinue, unsafeSkipHdrReturn, resolveReuse, unsafeDeferHdrForward, unsafeResolveRefreshFrames, orientationTrace, outline, stencil, safeInstancing,
        lazyDepthStencil, stereoAtlas, unsafeSkipImageSeed, unsafeSkipStencilSeed, unsafeSkipStencilCompose, unsafeSkipDepthCompose, colorSinglePass, colorFadePixels, size.width, size.height, samples, format, scale, focus, focusScale, profilePasses, gaze, gazeFile);
    }
    bool operator==(const ControlSettings& other) const { return values() == other.values(); }
    bool operator!=(const ControlSettings& other) const { return !(*this == other); }
  };

  inline std::string_view trimControl(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
  }

  // Parse a complete, bounded request transactionally. Missing keys inherit
  // startup defaults, not the previous request; malformed edits change nothing.
  inline std::optional<ControlSettings> parseControlSettings(std::string_view text,
      ControlSettings settings = {}) {
    if (text.size() > 4096 || text.find('\0') != text.npos) return std::nullopt;
    if (auto legacy = parseControl(text)) { settings.enabled = *legacy; return settings; }
    std::set<std::string_view> seen;
    while (!text.empty()) {
      auto end = text.find('\n');
      auto line = text.substr(0, end);
      text = end == text.npos ? std::string_view() : text.substr(end + 1);
      line = trimControl(line.substr(0, line.find('#')));
      if (line.empty()) continue;
      auto eq = line.find('=');
      if (eq == line.npos) return std::nullopt;
      auto key = trimControl(line.substr(0, eq)), value = trimControl(line.substr(eq+1));
      if (value.empty() || !seen.insert(key).second) return std::nullopt;
      bool* flag = key == "enabled" ? &settings.enabled : key == "profile" ? &settings.profile
        : key == "profile_details" ? &settings.profileDetails
        : key == "orientation_trace" ? &settings.orientationTrace : key == "outline" ? &settings.outline
        : key == "stencil" ? &settings.stencil : key == "safe_instancing" ? &settings.safeInstancing
        : key == "lazy_depth_stencil" ? &settings.lazyDepthStencil
        : key == "stereo_atlas" ? &settings.stereoAtlas
        : key == "scene_resume_after_resolve" ? &settings.sceneResumeAfterResolve
        : key == "unsafe_scene_continue" ? &settings.unsafeSceneContinue
        : key == "unsafe_skip_hdr_return" ? &settings.unsafeSkipHdrReturn
        : key == "resolve_reuse" ? &settings.resolveReuse
        : key == "unsafe_defer_hdr_forward" ? &settings.unsafeDeferHdrForward
        : key == "unsafe_skip_image_seed" ? &settings.unsafeSkipImageSeed
        : key == "unsafe_skip_stencil_seed" ? &settings.unsafeSkipStencilSeed
        : key == "unsafe_skip_stencil_compose" ? &settings.unsafeSkipStencilCompose
        : key == "unsafe_skip_depth_compose" ? &settings.unsafeSkipDepthCompose
        : key == "color_single_pass" ? &settings.colorSinglePass : nullptr;
      if (flag) {
        auto parsed = parseControl(value);
        if (!parsed && value == "true") parsed = true;
        if (!parsed && value == "false") parsed = false;
        if (!parsed) return std::nullopt;
        *flag = *parsed;
      } else if (key == "unsafe_resolve_refresh_frames") {
        auto n = parseUint(value, 120);
        if (!n || !*n) return std::nullopt;
        settings.unsafeResolveRefreshFrames = *n;
      } else if (key == "color_fade_pixels") {
        auto n = parseUint(value, 256);
        if (!n) return std::nullopt;
        settings.colorFadePixels = *n;
      } else if (key == "size") {
        auto size = parseSize(value);
        if (!size || size->width % 2) return std::nullopt;
        settings.size = *size;
      } else if (key == "samples" || key == "format") {
        auto n = parseUint(value, key == "samples" ? 16 : 1000000000);
        if (!n || !*n || (key == "samples" && (*n & (*n-1)))) return std::nullopt;
        (key == "samples" ? settings.samples : settings.format) = *n;
      } else if (key == "scale" || key == "focus" || key == "focus_scale") {
        double n;
        auto result = std::from_chars(value.data(), value.data()+value.size(), n);
        if (result.ec != std::errc() || result.ptr != value.data()+value.size()
         || !std::isfinite(n) || n <= 0 || n > 1) return std::nullopt;
        (key == "scale" ? settings.scale : key == "focus" ? settings.focus : settings.focusScale) = n;
      } else if (key == "profile_passes") {
        if (value != "sampled" && value != "full") return std::nullopt;
        settings.profilePasses = value;
      } else if (key == "gaze") {
        if (value != "shared" && value != "fixed") return std::nullopt;
        settings.gaze = value;
      } else if (key == "gaze_file") {
        if (value.size() > 1024) return std::nullopt;
        settings.gazeFile = value;
      } else return std::nullopt;
    }
    if (seen.empty()) return std::nullopt;
    return settings;
  }
}
