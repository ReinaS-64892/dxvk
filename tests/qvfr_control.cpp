#include "../src/util/util_qvfr_control.h"
#include <cstdlib>
#include <iostream>
#include <iterator>

static void check(bool value) { if (!value) std::abort(); }

int main(int argc, char**) {
  if (argc > 1) {
    std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    return dxvk::qvfr::parseControlSettings(input) ? 0 : 1;
  }
  for (auto value : {"on", "1", " \t1\r\n", "on\n"}) {
    auto state = dxvk::qvfr::parseControl(value);
    check(state.has_value() && *state);
  }
  for (auto value : {"off", "0", " \t0\r\n", "off\n"}) {
    auto state = dxvk::qvfr::parseControl(value);
    check(state.has_value() && !*state);
  }
  for (auto value : {"", " \n", "ON", "OFF", "on off", "2", "false", "on!", "o", "of"})
    check(!dxvk::qvfr::parseControl(value).has_value());
  check(!dxvk::qvfr::parseControl(std::string_view("on\0", 3)).has_value());
  using namespace dxvk::qvfr;
  ControlSettings old;
  check(!old.unsafeSkipHdrReturn);
  check(!old.resolveReuse && !old.unsafeDeferHdrForward);
  auto extra = parseControlSettings("resolve_reuse=on\nunsafe_defer_hdr_forward=on\n", old);
  check(extra && extra->resolveReuse && extra->unsafeDeferHdrForward && *extra != old);
  check(!parseControlSettings("resolve_reuse=maybe\n", old));
  auto hdrReturn = parseControlSettings("unsafe_skip_hdr_return=on\n", old);
  check(hdrReturn && hdrReturn->unsafeSkipHdrReturn && *hdrReturn != old);
  check(!parseControlSettings("unsafe_skip_hdr_return=maybe\n", old));
  check(!parseControlSettings("unsafe_skip_hdr_return=off\n", *hdrReturn)->unsafeSkipHdrReturn);
  old.profile = true; old.samples = 4; old.gaze = "fixed";
  auto config = parseControlSettings("# override old launch options\nenabled=off\nprofile=off\n"
    "gaze=shared\nsize=5184x2592\nsamples=2\nscale=0.5\nfocus=.25\n"
    "orientation_trace=true\noutline=0\nstencil=on\nsafe_instancing=1\nlazy_depth_stencil=false\n", old);
  check(bool(config));
  check(!config->enabled && !config->profile && config->gaze == "shared" && config->samples == 2);
  check(config->orientationTrace && !config->outline && !config->lazyDepthStencil);
  check(old.enabled && old.profile && old.samples == 4); // Parse is transactional.
  check(parseControlSettings("off\n", old)->profile); // Legacy ON/OFF preserves defaults.
  check(parseControlSettings("profile=off\n", old)->samples == 4);
  check(*parseControlSettings("profile=on\n", old) == old);
  check(!old.unsafeSkipImageSeed && !old.unsafeSkipStencilSeed && !old.unsafeSkipStencilCompose);
  for (unsigned mask = 0; mask < 8; mask++) {
    auto request = std::string("unsafe_skip_image_seed=") + (mask & 1 ? "on" : "off")
      + "\nunsafe_skip_stencil_seed=" + (mask & 2 ? "on" : "off")
      + "\nunsafe_skip_stencil_compose=" + (mask & 4 ? "on" : "off");
    auto parsed = parseControlSettings(request, old);
    check(bool(parsed));
    check(parsed->unsafeSkipImageSeed == bool(mask & 1));
    check(parsed->unsafeSkipStencilSeed == bool(mask & 2));
    check(parsed->unsafeSkipStencilCompose == bool(mask & 4));
    check(parsed->profile && parsed->samples == 4);
    check((*parsed != old) == bool(mask));
  }
  check(!old.unsafeSceneContinue && old.unsafeResolveRefreshFrames == 8);
  auto stale = parseControlSettings("unsafe_scene_continue=on\nunsafe_resolve_refresh_frames=12", old);
  check(bool(stale) && stale->unsafeSceneContinue && stale->unsafeResolveRefreshFrames == 12 && *stale != old);
  for (auto invalid : {"0", "121", "-1", "1.5"})
    check(!parseControlSettings(std::string("unsafe_resolve_refresh_frames=") + invalid));
  check(!old.sceneResumeAfterResolve);
  auto resume = parseControlSettings("scene_resume_after_resolve=on", old);
  check(bool(resume) && resume->sceneResumeAfterResolve && *resume != old);
  check(!parseControlSettings("scene_resume_after_resolve=off", *resume)->sceneResumeAfterResolve);
  check(!parseControlSettings("scene_resume_after_resolve=maybe"));
  check(!old.profileDetails);
  auto details = parseControlSettings("profile_details=on", old);
  check(bool(details) && details->profileDetails && *details != old && details->profile);
  check(!parseControlSettings("profile_details=off", *details)->profileDetails);
  check(!parseControlSettings("profile_details=maybe"));
  check(old.stereoAtlas);
  auto separate = parseControlSettings("stereo_atlas=off", old);
  check(bool(separate) && !separate->stereoAtlas && *separate != old);
  check(parseControlSettings("stereo_atlas=on", *separate)->stereoAtlas);
  check(!parseControlSettings("stereo_atlas=invalid", old));
  auto composite = parseControlSettings("unsafe_skip_depth_compose=on\ncolor_single_pass=off\ncolor_fade_pixels=24");
  check(bool(composite) && composite->unsafeSkipDepthCompose && !composite->colorSinglePass && composite->colorFadePixels == 24);
  check(!old.unsafeSkipDepthCompose && old.colorSinglePass && old.colorFadePixels == 0);
  check(parseControlSettings("color_fade_pixels=0")->colorFadePixels == 0);
  check(parseControlSettings("color_fade_pixels=256")->colorFadePixels == 256);
  auto density = parseControlSettings("scale=.125\nfocus_scale=.75\nfocus=.0625\nprofile_passes=full");
  check(bool(density) && density->scale == .125 && density->focusScale == .75 && density->focus == .0625);
  check(density->profilePasses == "full");
  check(old.focusScale == 1.0 && old.profilePasses == "sampled");
  for (auto invalid : {"", "# only comments\n", "profile=o", "profile=on\nprofile=off",
      "focus_scale=0", "focus_scale=1.1", "focus_scale=nan", "profile_passes=all",
      "color_fade_pixels=-1", "color_fade_pixels=257", "color_fade_pixels=1.5",
      "unknown=1", "samples=3", "samples=32", "size=5183x2592", "size=0x2592",
      "format=0", "scale=nan", "focus=0", "scale=1.1", "gaze=other", "outline=source",
      "profile=on\nsize=broken", "gaze_file="})
    check(!parseControlSettings(invalid, old));
  check(!parseControlSettings(std::string_view("profile=on\0", 11)));
  check(!parseControlSettings(std::string(4097, ' ')));
  std::cout << "QVFR runtime control: on/off, whitespace, invalid and partial requests passed\n";
}
