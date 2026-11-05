#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_qvfr.h"
#include "../util/util_env.h"
#include "../util/util_qvfr_control.h"
#include "../util/util_qvfr_json.h"
#include <iomanip>
#include <locale>
#include <cstring>

#include "buildenv.h"
#include <charconv>
#include <fstream>
#include <sstream>

namespace dxvk {
  namespace {
    bool stencilMayWrite(const DxvkDepthStencilState& ds) {
      auto face = [] (const DxvkStencilOp& op) {
        return qvfr::stencilMayWrite(op.writeMask(), uint32_t(op.failOp()),
          uint32_t(op.passOp()), uint32_t(op.depthFailOp()));
      };
      return ds.stencilTest() && (face(ds.stencilOpFront()) || face(ds.stencilOpBack()));
    }

    std::optional<qvfr::ControlSettings> readControlSettings(const std::string& path,
        const qvfr::ControlSettings& defaults) {
      std::ifstream file(path, std::ios::binary);
      if (!file) return std::nullopt;
      std::array<char, 4097> bytes = {};
      file.read(bytes.data(), bytes.size());
      auto count = file.gcount();
      if (file.bad() || count >= std::streamsize(bytes.size())) return std::nullopt;
      return qvfr::parseControlSettings(std::string_view(bytes.data(), size_t(count)), defaults);
    }

    void restoreRaster(DxvkContext& ctx, const DxvkViewportState& state) {
      std::array<DxvkViewport, DxvkLimits::MaxNumViewports> vp;
      for (uint32_t i = 0; i < state.viewportCount; i++)
        vp[i] = { state.viewports[i], state.scissorRects[i] };
      ctx.setViewports(state.viewportCount, vp.data());
    }

    Rc<DxvkImageView> sampled(const Rc<DxvkImageView>& view) {
      auto key = view->info();
      key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      key.layout = VK_IMAGE_LAYOUT_GENERAL;
      key.aspects &= VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
      return view->image()->createView(key);
    }

    void copyCrop(DxvkContext& ctx, const Rc<DxvkImageView>& dst, qvfr::Rect d,
        const Rc<DxvkImageView>& src, qvfr::Rect s);

    void stretch(DxvkContext& ctx, const Rc<DxvkImageView>& dst, qvfr::Rect d,
        const Rc<DxvkImageView>& src, qvfr::Rect s) {
      if (d.width == s.width && d.height == s.height) {
        copyCrop(ctx, dst, d, src, s);
        return;
      }
      VkOffset3D so[2] = {{int32_t(s.x), int32_t(s.y), 0},
        {int32_t(s.x+s.width), int32_t(s.y+s.height), 1}};
      VkOffset3D to[2] = {{int32_t(d.x), int32_t(d.y), 0},
        {int32_t(d.x+d.width), int32_t(d.y+d.height), 1}};
      ctx.blitImageView(dst, to, sampled(src), so, VK_FILTER_NEAREST);
    }

    void copyCrop(DxvkContext& ctx, const Rc<DxvkImageView>& dst, qvfr::Rect d,
        const Rc<DxvkImageView>& src, qvfr::Rect s) {
      if (d.width != s.width || d.height != s.height) {
        stretch(ctx, dst, d, src, s);
        return;
      }
      auto sk = src->info();
      auto dk = dst->info();
      // These copies carry color/depth only. Stencil has its own import and
      // composition paths, including independent unsafe skip controls.
      sk.aspects &= VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
      dk.aspects &= VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
      ctx.copyImage(dst->image(), {dk.aspects, dk.mipIndex, dk.layerIndex, 1},
        {int32_t(d.x), int32_t(d.y), 0}, src->image(),
        {sk.aspects, sk.mipIndex, sk.layerIndex, 1}, {int32_t(s.x), int32_t(s.y), 0},
        {s.width, s.height, 1});
    }

  }

  DxvkQvfrState::~DxvkQvfrState() { closeGaze(); }

  void DxvkQvfrState::closeGaze() {
#ifdef _WIN32
    if (gaze) UnmapViewOfFile(gaze);
    if (gazeMap) CloseHandle(HANDLE(gazeMap));
    if (gazeFile) CloseHandle(HANDLE(gazeFile));
#endif
    gaze = nullptr; gazeMap = gazeFile = nullptr;
  }

  bool DxvkQvfrState::readGaze(std::array<qvfr::Gaze, 2>& result) {
    if (!sharedGaze) return true; // Explicit fixed center mode.
    auto now = std::chrono::steady_clock::now();
#ifdef _WIN32
    if (!gaze && now >= nextOpen) {
      nextOpen = now + std::chrono::seconds(2);
      HANDLE file = CreateFileA(gazePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(file, &size) && size.QuadPart >= LONGLONG(sizeof(gaze_bridge::SharedGaze))) {
          gazeMap = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
          if (gazeMap)
            gaze = static_cast<const gaze_bridge::SharedGaze*>(
              MapViewOfFile(HANDLE(gazeMap), FILE_MAP_READ, 0, 0, sizeof(*gaze)));
        }
        gazeFile = file;
        if (!gaze) closeGaze();
        else freshness = gaze_bridge::Freshness{};
      }
    }
#endif
    if (!gaze) return false;
    gaze_bridge::Sample sample;
    uint32_t seq;
    if (gaze_bridge::snapshot(gaze, sample, seq) && freshness.observe(seq, now) && sample.valid) {
      for (uint32_t i = 0; i < 2; i++)
        result[i] = qvfr::Gaze{sample.u[i], sample.v[i]};
      return true;
    }
    if (freshness.expired(now)) closeGaze();
    return false;
  }

  void DxvkContext::initQvfr() {
    if (env::getEnvVar("DXVK_QVFR") != "1") return;
    auto q = std::make_unique<DxvkQvfrState>();
    std::string legacy;
    for (const auto& [name, key] : {
        std::pair{"DXVK_QVFR_SIZE", "size"}, {"DXVK_QVFR_FORMAT", "format"},
        {"DXVK_QVFR_SAMPLES", "samples"}, {"DXVK_QVFR_GAZE", "gaze"},
        {"DXVK_QVFR_SCALE", "scale"}, {"DXVK_QVFR_FOCUS", "focus"},
        {"DXVK_QVFR_PROFILE", "profile"}, {"DXVK_QVFR_ORIENTATION_TRACE", "orientation_trace"},
        {"DXVK_QVFR_STENCIL", "stencil"}, {"DXVK_QVFR_SAFE_INSTANCING", "safe_instancing"},
        {"DXVK_QVFR_LAZY_DEPTH_STENCIL", "lazy_depth_stencil"}, {"DXVK_GAZE_FILE", "gaze_file"}}) {
      auto value = env::getEnvVar(name);
      if (!value.empty()) legacy += std::string(key) + "=" + value + "\n";
    }
    auto outline = env::getEnvVar("DXVK_QVFR_OUTLINE");
    if (!outline.empty()) legacy += std::string("outline=") + (outline == "source" ? "on" : outline) + "\n";
    if (!legacy.empty()) {
      auto defaults = qvfr::parseControlSettings(legacy);
      if (defaults) q->controlDefaults = *defaults;
      else Logger::warn("QVFR: invalid legacy options; using runtime control defaults");
    }
    q->controlPath = env::getEnvVar("DXVK_QVFR_CONTROL_FILE");
    if (q->controlPath.empty()) q->controlPath = QVFR_CONTROL_FILE;
    if (q->controlPath.empty()) q->controlPath = "qvfr-control.txt";
    auto requested = readControlSettings(q->controlPath, q->controlDefaults);
    if (!requested) {
      q->controlError = true;
      Logger::warn(str::format("QVFR control initial file missing or invalid; using defaults: ", q->controlPath));
    }
    q->sceneOnly = env::getEnvVar("DXVK_QVFR_SCENE_ONLY") != "0";
    q->lazyDepthStencil = q->sceneOnly && env::getEnvVar("DXVK_QVFR_LAZY_DEPTH_STENCIL") != "0";
    q->virtualEnabled = !q->sceneOnly && env::getEnvVar("DXVK_QVFR_VIRTUAL") != "0";
    q->stencilEnabled = (q->sceneOnly || q->virtualEnabled) && env::getEnvVar("DXVK_QVFR_STENCIL") != "0";
    q->profile = env::getEnvVar("DXVK_QVFR_PROFILE") == "1"
      && m_device->queues().graphics.properties.core.queueFamilyProperties.timestampValidBits != 0;
    auto orientation = env::getEnvVar("DXVK_QVFR_ORIENTATION_TRACE");
    q->orientationTrace = orientation == "1";
    q->debugOutline = env::getEnvVar("DXVK_QVFR_DEBUG") == "1";
    q->sourceOutline = q->debugOutline;
    q->unsafeReplay = q->virtualEnabled && !q->debugOutline && env::getEnvVar("DXVK_QVFR_UNSAFE") == "1";
    q->unsafePost = q->virtualEnabled && !q->debugOutline
      && (q->unsafeReplay || env::getEnvVar("DXVK_QVFR_UNSAFE_POST") == "1");
    q->unsafeInstancing = !q->sceneOnly
      && (q->unsafeReplay || env::getEnvVar("DXVK_QVFR_UNSAFE_INSTANCING") == "1");
    q->safeInstancing = q->sceneOnly && env::getEnvVar("DXVK_QVFR_SAFE_INSTANCING") != "0";
    if (q->unsafeReplay && q->orientationTrace) {
      q->orientationTrace = false;
      Logger::info("QVFR orientation probes suppressed: strict UNSAFE retains split inputs until external handoff");
    }
    m_qvfr = std::move(q);
    applyQvfrControl(requested.value_or(m_qvfr->controlDefaults), true);
    Logger::info(str::format("QVFR rendering enabled size=", m_qvfr->size.width, "x", m_qvfr->size.height,
      " format=", m_qvfr->format, " samples=", m_qvfr->samples,
      " scale=", m_qvfr->scale, " focus=", m_qvfr->fraction,
      " gaze=", m_qvfr->sharedGaze ? "shared" : "fixed",
      " scene_gaze_y=default-flipped",
      " debug=", m_qvfr->debugOutline,
      " outline_source=", m_qvfr->sourceOutline,
      " scratch=atlas planes=separate profile=", m_qvfr->profile, " virtual=", m_qvfr->virtualEnabled,
      " stencil_replay=", m_qvfr->stencilEnabled, " unsafe_post=", m_qvfr->unsafePost,
      " unsafe_instancing=", m_qvfr->unsafeInstancing, " safe_instancing=", m_qvfr->safeInstancing,
      " unsafe_replay=", m_qvfr->unsafeReplay));
    if (m_qvfr->unsafeReplay)
      Logger::info("QVFR strict UNSAFE enabled: retain split, drop unsupported VR draws/compute, publish at learned terminal target or external handoff; post_classification=fullwidth-color-only");
    if (m_qvfr->sceneOnly)
      Logger::info("QVFR scene-only enabled: safe scene replay, compose at first dependency/fallback, normal until next endFrame unless resolve-resume is enabled; UNSAFE and virtual post disabled");
    if (m_qvfr->unsafeSceneContinue)
      Logger::info("QVFR UNSAFE scene continuation: reuse learned intermediate resolve contents, retain atlas without publication, periodically refresh; known external outputs excluded");
    if (m_qvfr->sceneResumeAfterResolve)
      Logger::info("QVFR resolve-resume experiment enabled: same original pair, depth-tested eye viewport, full scene safety checks, max 4 resumes/frame, no resume after external handoff/native scene-target fallback");
    if (m_qvfr->lazyDepthStencil)
      Logger::info("QVFR lazy depth/stencil enabled: publish color at scene-end; retain depth/stencil until access or discard on full clear");
    Logger::info("QVFR stereo atlas: shared color/depth attachments for both eyes, eye-local rows, runtime stereo_atlas=on/off");
    Logger::info("QVFR CPU replay: direct pair commits graphics state once, focus updates only raster state; atlas pass continuity/break counters enabled");
    Logger::info("QVFR overhead diagnostics: atlas allocation/rebuild/relocation counters, stencil KEEP writes excluded, closed scene fast path");
    if (m_qvfr->safeInstancing)
      Logger::info("QVFR safe direct instancing enabled: original instance IDs, half-width viewport, scene safety checks; post and indirect excluded");
    if (m_qvfr->profile)
      Logger::info("QVFR profile: every 120 frames; profile_passes=sampled measures up to 8 passes per category, full measures all QV/native passes up to 8192 each; totals require clipped=0 and skipped=0");
    if (!m_qvfr->controlPath.empty())
      Logger::info(str::format("QVFR control file=", m_qvfr->controlPath, " poll_ms=250"));
    if (m_qvfr->orientationTrace)
      Logger::info("QVFR orientation trace enabled: GPU UV probes, max 2 intermediate + 2 terminal/frame and 1024 total per context, asynchronous readback; diagnostic overhead");
  }

  void DxvkContext::applyQvfrControl(const qvfr::ControlSettings& settings, bool initial) {
    auto& q = *m_qvfr;
    if (!initial && settings == q.controlRequested) return;
    if (!initial) {
      // Commit old mappings/planes before changing geometry or diagnostics.
      // In-flight timestamp/readback objects remain alive until their fences.
      restoreQvfrVirtualReads();
      flushQvfr("control-disable");
      while (!q.virtualImages.empty())
        materializeQvfrVirtual(q.virtualImages.begin()->first, "control-disable");
      endCurrentPass(true);
    }
    bool resized = q.size.width != settings.size.width || q.size.height != settings.size.height;
    if (q.sharedGaze != (settings.gaze == "shared") || q.gazePath != settings.gazeFile)
      q.closeGaze();
    if (resized) q.orientationTarget = nullptr;
    if (resized || (!q.orientationTrace && settings.orientationTrace)) {
      q.orientationCount = 0;
      q.orientationLastCapture.clear();
      q.orientationFrame = ~uint64_t(0);
    }
    q.enabled = settings.enabled;
    q.size = settings.size; q.format = settings.format; q.samples = settings.samples;
    q.scale = settings.scale; q.fraction = settings.focus;
    q.focusScale = q.sceneOnly ? settings.focusScale : 1.0;
    q.profileFull = settings.profilePasses == "full";
    q.profileDetails = settings.profileDetails;
    q.sharedGaze = settings.gaze == "shared"; q.gazePath = settings.gazeFile;
    q.profile = settings.profile && m_device->queues().graphics.properties.core.queueFamilyProperties.timestampValidBits != 0;
    q.orientationTrace = settings.orientationTrace && !q.unsafeReplay;
    q.sourceOutline = settings.outline || q.debugOutline;
    q.stencilEnabled = settings.stencil && (q.sceneOnly || q.virtualEnabled);
    q.safeInstancing = settings.safeInstancing && q.sceneOnly;
    q.lazyDepthStencil = settings.lazyDepthStencil && q.sceneOnly;
    q.stereoAtlas = settings.stereoAtlas && q.sceneOnly;
    q.unsafeSceneContinue = settings.unsafeSceneContinue && q.sceneOnly;
    q.unsafeSkipHdrReturn = settings.unsafeSkipHdrReturn && q.unsafeSceneContinue;
    q.resolveReuse = settings.resolveReuse && q.sceneOnly;
    q.unsafeDeferHdrForward = settings.unsafeDeferHdrForward && q.unsafeSkipHdrReturn;
    q.resolveReuseHistory = {}; q.hdrForwardTicket = 0;
    q.hdrForwardCancelledImage = 0;
    q.hdrReturnHistory = {}; q.hdrReturnWriting = {};
    q.unsafeResolveRefreshFrames = settings.unsafeResolveRefreshFrames;
    q.sceneResumeAfterResolve = settings.sceneResumeAfterResolve && q.sceneOnly && !q.unsafeSceneContinue;
    q.unsafeSkipImageSeed = settings.unsafeSkipImageSeed && q.sceneOnly;
    q.unsafeSkipStencilSeed = settings.unsafeSkipStencilSeed && q.sceneOnly;
    q.unsafeSkipStencilCompose = settings.unsafeSkipStencilCompose && q.sceneOnly;
    q.unsafeSkipDepthCompose = settings.unsafeSkipDepthCompose && q.sceneOnly;
    q.colorSinglePass = settings.colorSinglePass && q.sceneOnly;
    q.colorFadePixels = settings.colorFadePixels;
    q.gazeFrame = ~uint64_t(0);
    q.outlineCycle = {};
    q.controlRequested = settings;
    Logger::info(str::format("QVFR control enabled=", q.enabled, " frame=", m_frameCount,
      " profile=", q.profile, " profile_details=", q.profileDetails, " orientation_trace=", q.orientationTrace,
      " gaze=", settings.gaze, " outline=", q.sourceOutline,
      " size=", q.size.width, "x", q.size.height, " samples=", q.samples,
      " format=", q.format, " scale=", q.scale, " focus=", q.fraction,
      " focus_scale=", q.focusScale, " profile_passes=", settings.profilePasses,
      " stereo_atlas=", q.stereoAtlas, " scene_resume_after_resolve=", q.sceneResumeAfterResolve,
      " unsafe_scene_continue=", q.unsafeSceneContinue, " unsafe_resolve_refresh_frames=", q.unsafeResolveRefreshFrames,
      " unsafe_skip_hdr_return=", q.unsafeSkipHdrReturn,
      " resolve_reuse=", q.resolveReuse, " unsafe_defer_hdr_forward=", q.unsafeDeferHdrForward,
      " stencil=", q.stencilEnabled, " safe_instancing=", q.safeInstancing,
      " lazy_ds=", q.lazyDepthStencil, " unsafe_replay=", q.unsafeReplay,
      " unsafe_skip_image_seed=", q.unsafeSkipImageSeed,
      " unsafe_skip_stencil_seed=", q.unsafeSkipStencilSeed,
      " unsafe_skip_stencil_compose=", q.unsafeSkipStencilCompose,
      " unsafe_skip_depth_compose=", q.unsafeSkipDepthCompose,
      " color_single_pass=", q.colorSinglePass, " color_fade_pixels=", q.colorFadePixels,
      " file=", q.controlPath));
    auto plan = qvfr::makePlan(q.size, qvfr::StereoLayout::DoubleWide, q.scale, q.fraction,
      {{{.5, .5}, {.5, .5}}}, q.focusScale);
    if (plan) {
      const auto& eye = plan->eyes[0];
      double pixels = double(eye.peripheral.width) * eye.peripheral.height
        + double(eye.focus.width) * eye.focus.height;
      Logger::info(str::format("QVFR layout per_eye=1 peripheral=", eye.peripheral.width, "x", eye.peripheral.height,
        " focus_texture=", eye.focus.width, "x", eye.focus.height,
        " focus_screen=", eye.composite.width, "x", eye.composite.height,
        " scene_pixels_percent=", pixels * 100.0 / (double(eye.original.width) * eye.original.height)));
    }
  }

  void DxvkContext::pollQvfrControl() {
    if (!m_qvfr || m_qvfrBusy || m_qvfr->controlPath.empty()) return;
    auto& q = *m_qvfr;
    auto now = std::chrono::steady_clock::now();
    if (now < q.nextControlPoll) return;
    q.nextControlPoll = now + std::chrono::milliseconds(250);
    auto desired = readControlSettings(q.controlPath, q.controlDefaults);
    if (!desired) {
      if (!q.controlError)
        Logger::warn(str::format("QVFR control unchanged: unreadable or invalid file=", q.controlPath));
      q.controlError = true;
      return;
    }
    q.controlError = false;
    applyQvfrControl(*desired, false);
  }

  void DxvkContext::drawQvfrSourceOutline(size_t index) {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->sourceOutline) return;
    auto& q = *m_qvfr;
    auto& g = q.cache[index];
    if (!m_state.gp.state.omBlend[0].colorWriteMask() || !q.outlineCycle.firstDraw(g.eye)) return;
    // Stamp once just after this eye's first scene color draw. Pin the same
    // edges to near depth so later depth-tested scene/sky draws preserve them.
    // Copies and postprocesses only propagate the marker; never redraw it.
    bool busy = std::exchange(m_qvfrBusy, true);
    auto targets = m_state.om.renderTargets;
    auto viewports = m_state.vp;
    auto feedback = m_state.gp.state.om.feedbackLoop();
    auto timing = beginQvfrTiming("outline-source");
    auto view = g.atlas.color[0].view;
    DxvkRenderTargets markerTargets = {};
    markerTargets.color[0].view = view;
    markerTargets.depth = g.atlas.depth;
    bindRenderTargets(std::move(markerTargets), 0);
    VkClearValue cyan = {};
    cyan.color.float32[1] = cyan.color.float32[2] = cyan.color.float32[3] = 1;
    // Unity uses reversed Z (near=1). Honor an explicitly forward-Z scene
    // compare; ALWAYS/EQUAL/disabled tests do not encode a direction, so retain
    // the Unity reversed-Z default for those cases.
    auto compare = m_state.dyn.depthStencilState.depthCompareOp();
    VkClearValue nearDepth = {};
    nearDepth.depthStencil.depth = compare == VK_COMPARE_OP_LESS
      || compare == VK_COMPARE_OP_LESS_OR_EQUAL ? 0.0f : 1.0f;
    for (auto edge : qvfr::outline(g.placement.focus, 8)) {
      VkOffset3D offset = {int32_t(edge.x), int32_t(edge.y), 0};
      VkExtent3D extent = {edge.width, edge.height, 1};
      clearImageView(view, offset, extent, VK_IMAGE_ASPECT_COLOR_BIT, cyan);
      // Clear depth only: stencil contents and the surrounding scene survive.
      clearImageView(g.atlas.depth.view, offset, extent, VK_IMAGE_ASPECT_DEPTH_BIT, nearDepth);
    }
    prepareShaderReadableImages(false);
    endQvfrTiming(timing);
    bindRenderTargets(std::move(targets), feedback);
    restoreRaster(*this, viewports);
    m_qvfrBusy = busy;
  }

  bool DxvkContext::skipQvfr(const char* reason) {
    auto& q = *m_qvfr;
    q.lastSkip = reason;
    auto& count = q.skipped[reason];
    count++;
    const auto& c = m_state.om.renderTargets.color[0].view;
    const auto& d = m_state.om.renderTargets.depth.view;
    bool target = c && c->mipLevelExtent(0).width == q.size.width
                    && c->mipLevelExtent(0).height == q.size.height;
    bool firstTarget = target && q.targetSkips.insert(reason).second;
    if (count != 1 && !firstTarget) return false;
    std::ostringstream out;
    out << "QVFR skipped reason=" << reason << " target_size=" << target
        << " frame=" << m_frameCount << " viewports=" << m_state.vp.viewportCount
        << " stencil=" << m_state.dyn.depthStencilState.stencilTest()
        << " queries=" << m_queryManager.hasEnabledQueries()
        << " pipeline_flags=" << uint64_t(m_state.gp.flags.raw())
        << " gs=" << bool(m_state.gp.shaders.gs) << " tcs=" << bool(m_state.gp.shaders.tcs)
        << " tes=" << bool(m_state.gp.shaders.tes);
    if (m_state.gp.shaders.vs)
      out << " vs_flags=" << uint64_t(m_state.gp.shaders.vs->metadata().flags.raw());
    if (m_state.gp.shaders.fs)
      out << " fs_flags=" << uint64_t(m_state.gp.shaders.fs->metadata().flags.raw());
    auto describe = [&] (const char* name, const Rc<DxvkImageView>& v) {
      out << ' ' << name << '=';
      if (!v) { out << "none"; return; }
      auto k = v->info();
      const auto& i = v->image()->info();
      out << i.extent.width << 'x' << i.extent.height
          << "/fmt:" << uint32_t(k.format) << "/base:" << uint32_t(i.format)
          << "/samples:" << uint32_t(i.sampleCount) << "/type:" << uint32_t(k.viewType)
          << "/mip:" << uint32_t(k.mipIndex) << ':' << i.mipLevels
          << "/layer:" << k.layerIndex << ':' << k.layerCount << ':' << i.numLayers
          << "/aspects:" << uint32_t(k.aspects) << "/layout:" << uint32_t(k.layout);
    };
    describe("color", c); describe("depth", d);
    if (m_state.vp.viewportCount) {
      const auto& v = m_state.vp.viewports[0];
      out << " viewport=" << v.x << ',' << v.y << ',' << v.width << ',' << v.height;
      const auto& sc = m_state.vp.scissorRects[0];
      out << " scissor=" << sc.offset.x << ',' << sc.offset.y << ','
          << sc.extent.width << ',' << sc.extent.height;
    }
    Logger::info(out.str());
    return false;
  }

  void DxvkContext::reportQvfr() {
    if (!m_qvfr || (m_frameCount > 2 && m_frameCount % 120)) return;
    const auto& q = *m_qvfr;
    std::ostringstream out;
    out << "QVFR stats frame=" << m_frameCount << " enabled=" << q.enabled
        << " profile=" << q.profile << " orientation_trace=" << q.orientationTrace
        << " scene_only=" << q.sceneOnly << " lazy_ds=" << q.lazyDepthStencil << " scene_ends=" << q.sceneEnds
        << " unsafe_scene_continue=" << q.unsafeSceneContinue
        << " unsafe_skip_hdr_return=" << q.unsafeSkipHdrReturn
        << " resolve_reuse=" << q.resolveReuse << " resolve_reuse_hits=" << q.resolveReuseHits
        << " resolve_reuse_records=" << q.resolveReuseRecords
        << " unsafe_defer_hdr_forward=" << q.unsafeDeferHdrForward
        << " hdr_forward_deferred=" << q.hdrForwardDeferred << " hdr_forward_cancelled=" << q.hdrForwardCancelled
        << " hdr_forward_late_reads=" << q.hdrForwardLateReads
        << " hdr_return_shader_draws=" << q.hdrReturnShaderDraws << " hdr_return_candidates=" << q.hdrReturnCandidates
        << " hdr_return_attempts=" << q.hdrReturnAttempts << " hdr_return_skipped=" << q.hdrReturnSkipped
        << " grab_links=" << q.grabLinks << " grab_indirect_reads=" << q.grabIndirectReads
        << " grab_shader_probes=" << q.grabShaderProbes << " grab_shader_sources=" << q.grabShaderSources
        << " grab_terminals=" << q.grabTerminals << " grab_history=" << q.grabProvenance.nodes.size()
        << " unsafe_resolve_seen=" << q.unsafeResolveSeen
        << " unsafe_resolve_candidates=" << q.unsafeResolveCandidates
        << " unsafe_resolve_reads=" << q.unsafeResolveReads
        << " unsafe_resolve_scans=" << q.unsafeResolveScans
        << " unsafe_resolve_sampled_images=" << q.unsafeResolveSampledImages
        << " dependency_images=" << q.dependencyImages
        << " dependency_duplicates=" << q.dependencyDuplicates
        << " dependency_unrelated=" << q.dependencyUnrelated
        << " unsafe_resolve_history=" << q.staleResolves.size()
        << " unsafe_resolves_skipped=" << q.unsafeResolvesSkipped
        << " unsafe_resolve_readers_learned=" << q.unsafeResolveReadersLearned
        << " unsafe_resolve_refreshes=" << q.unsafeResolveRefreshes
        << " scene_resume_after_resolve=" << q.sceneResumeAfterResolve << " scene_resumes=" << q.sceneResumes
        << " replayed_batches=" << q.replayedBatches
        << " composed_groups=" << q.groups
        << " color_composites=" << q.colorComposites << " depth_composites=" << q.depthComposites
        << " stencil_composites=" << q.stencilComposites << " stencil_batches=" << q.stencilBatches
        << " post_cache_batches=" << q.postCacheBatches << " post_snapshot_batches=" << q.postSnapshotBatches
        << " post_passes=" << q.postPasses
        << " instanced_batches=" << q.instancedBatches << " depth_refreshes=" << q.depthRefreshes
        << " indirect_batches=" << q.indirectBatches << " advanced_batches=" << q.advancedBatches
        << " geometry_batches=" << q.geometryBatches << " tessellation_batches=" << q.tessellationBatches
        << " seeded_groups=" << q.seededGroups << " cache_hits=" << q.cacheHits
        << " color_refreshes=" << q.colorRefreshes
        << " replay_passes=" << q.replayPasses << " paired_in_one_pass=" << q.pairedInOnePass
        << " stereo_atlas=" << q.stereoAtlas << " stereo_atlas_shares=" << q.stereoAtlasShares
        << " stereo_eye_pass_keeps=" << q.stereoEyePassKeeps
        << " stereo_atlas_fallbacks=" << q.stereoAtlasFallbacks
        << " direct_pairs=" << q.directPairs << " atlas_continuations=" << q.atlasContinuations
        << " atlas_eye_switches=" << q.atlasEyeSwitches
        << " atlas_pass_ends=" << q.atlasPassEnds << " atlas_target_breaks=" << q.atlasTargetBreaks
        << " atlas_command_breaks=" << q.atlasCommandBreaks
        << " atlas_hazard_breaks=" << q.atlasHazardBreaks
        << " atlas_descriptor_breaks=" << q.atlasDescriptorBreaks
        << " atlas_resolve_breaks=" << q.atlasResolveBreaks
        << " atlas_other_breaks=" << (q.atlasPassEnds - q.atlasTargetBreaks - q.atlasCommandBreaks
          - q.atlasHazardBreaks - q.atlasDescriptorBreaks - q.atlasResolveBreaks);
    for (const auto& [reason, count] : q.unsafeResolveDecisions)
      out << " unsafe_decision_" << reason << '=' << count;
    for (const auto& entry : q.skipped)
      out << ' ' << entry.first << '=' << entry.second;
    for (const auto& entry : q.compositionReasons)
      out << " compose_" << entry.first << '=' << entry.second;
    out << " discarded_color=" << q.discardedColor << " discarded_depth=" << q.discardedDepth
        << " discarded_stencil=" << q.discardedStencil
        << " virtual_discarded=" << q.virtualDiscarded
        << " external_reads=" << q.externalReads
        << " virtual_copies=" << q.virtualCopies << " virtual_aliases=" << q.virtualAliases << " virtual_resolves=" << q.virtualResolves
        << " virtual_reads=" << q.virtualReads << " virtual_materialized=" << q.virtualMaterialized
        << " virtual_live=" << q.virtualImages.size();
    out << " unsafe_deferred_flushes=" << q.deferredFlushes << " unsafe_dropped_draws=" << q.droppedDraws
        << " unsafe_dropped_compute=" << q.droppedCompute << " unsafe_raw_reads=" << q.rawVirtualReads
        << " unsafe_retained_evictions=" << q.retainedEvictions << " unsafe_dropped_snapshots=" << q.droppedSnapshots
        << " unsafe_dropped_planes=" << q.droppedPlanes << " terminal_normal_batches=" << q.terminalBatches
        << " terminal_images=" << q.terminalCycle.images.size();
    out << " atlas_images_created=" << q.atlasImagesCreated << " atlas_rebuilds=" << q.atlasRebuilds
        << " atlas_seed_reuses=" << q.atlasSeedReuses
        << " seed_mapping_changed=" << q.seedMappingChanged << " seed_mapping_same=" << q.seedMappingSame
        << " compatibility_checks=" << q.compatibilityChecks << " compatibility_barriers=" << q.compatibilityBarriers
        << " compatibility_relocations=" << q.compatibilityRelocations
        << " stencil_write_batches=" << q.stencilWriteBatches << " stencil_readonly_batches=" << q.stencilReadOnlyBatches
        << " native_direct_batches=" << q.nativeDirectBatches << " native_direct_draws=" << q.nativeDirectDraws;
    uint32_t retainedDepth = 0, retainedStencil = 0;
    for (const auto& g : q.cache) {
      retainedDepth += g.pending.depth;
      retainedStencil += g.pending.stencil;
    }
    out << " unsafe_image_seeds_skipped=" << q.unsafeImageSeedsSkipped
        << " unsafe_stencil_seeds_skipped=" << q.unsafeStencilSeedsSkipped
        << " unsafe_stencil_composites_skipped=" << q.unsafeStencilCompositesSkipped;
    out << " unsafe_depth_composites_skipped=" << q.unsafeDepthCompositesSkipped
        << " color_single_pass_composites=" << q.colorSinglePassComposites
        << " color_single_pass_fallbacks=" << q.colorSinglePassFallbacks;
    out << " retained_depth=" << retainedDepth << " retained_stencil=" << retainedStencil;
    if (q.flowFrame == m_frameCount)
      out << " flow_events=" << q.flowEvents << " flow_dropped=" << q.flowDropped;
    Logger::info(out.str());
    if (q.cpuDetail.frame == m_frameCount)
      for (size_t i = 0; i < q.cpuDetail.totals.size(); i++) {
        const auto& detail = q.cpuDetail.totals[i];
        if (!detail.calls) continue;
        Logger::info(str::format("QVFR cpu-detail frame=", m_frameCount,
          " stage=", QvfrCpuStageNames[i], " calls=", detail.calls,
          " inclusive_ms=", detail.inclusiveMs, " exclusive_ms=", detail.exclusiveMs));
      }
    if (q.cpuProfileFrame == m_frameCount)
      Logger::info(str::format("QVFR cpu-record frame=", m_frameCount,
        " qvfr_ms=", q.cpuQvfrMs, " qvfr_calls=", q.cpuQvfrCalls,
        " native_ms=", q.cpuNativeMs, " native_calls=", q.cpuNativeCalls));
  }

  void DxvkContext::noteQvfrProfileDraw(const char* kind, uint32_t count,
      int64_t instances, uint32_t firstInstance) {
    if (!m_qvfr || m_qvfrBusy || !count) return;
    auto& q = *m_qvfr;
    noteQvfrFlowDraw(kind, count, instances);
    if (!q.profileCaptureDetails || q.profileFrame != m_frameCount
     || q.profileReplay == size_t(-1)) return;
    auto& timing = q.timings[q.profileReplay];
    if (timing.phase != "native-pass-total" && timing.phase != "native-pass-sample") return;
    DxvkQvfrCpuDetailScope metadataDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Metadata);
    timing.detailBatches++;
    timing.detailDraws += count;
    const auto& shaders = m_state.gp.shaders;
    std::array<Rc<DxvkShader>, 5> stages = {shaders.vs, shaders.tcs, shaders.tes, shaders.gs, shaders.fs};
    const auto& ds = m_state.dyn.depthStencilState;
    const auto& vp = m_state.vp.viewports[0];
    const auto& sc = m_state.vp.scissorRects[0];
    std::array<uint64_t, 12> key = {};
    for (size_t i = 0; i < stages.size(); i++) key[i] = stages[i] ? DxvkShader::getCookie(stages[i]) : 0;
    key[5] = m_state.gp.flags.raw();
    key[6] = uint64_t(ds.depthTest()) | (uint64_t(ds.depthWrite()) << 1)
      | (uint64_t(ds.stencilTest()) << 2) | (uint64_t(ds.depthCompareOp()) << 3)
      | (uint64_t(m_state.vp.viewportCount) << 8) | (uint64_t(m_state.gp.state.omBlend[0].colorWriteMask()) << 16);
    std::memcpy(&key[7], &vp, sizeof(float) * 4); // x/y/width/height, bit-exact key.
    key[9] = uint64_t(instances);
    key[10] = firstInstance;
    key[11] = uint64_t(uint32_t(sc.offset.x)) ^ (uint64_t(uint32_t(sc.offset.y)) << 32);
    // Include scissor extent too; different clips must not share one snapshot.
    key[6] ^= uint64_t(sc.extent.width) << 32;
    key[10] |= uint64_t(sc.extent.height) << 32;
    std::string_view scene = !q.enabled ? "qvfr-off"
      : q.sceneCycle.frame != m_frameCount || !q.sceneCycle.started ? "before-scene"
      : q.sceneCycle.closed ? "after-scene" : "scene-active";
    std::string_view reason = !q.enabled ? "disabled" : q.lastSkip;
    if (reason == "draw" && scene == "after-scene") reason = "scene-closed";
    for (auto& group : timing.drawGroups)
      if (group.key == key && group.reason == reason && group.scene == scene && group.kind == kind) {
        group.batches++; group.draws += count;
        return;
      }
    // Metadata is bounded separately from GPU queries. Keep counting calls
    // even if a pass contains too many different shaders/state combinations.
    if (timing.drawGroups.size() >= 32 || q.profileDetailGroups >= 4096) {
      timing.detailDropped++;
      return;
    }
    q.profileDetailGroups++;
    const auto& rt = m_state.om.framebufferInfo.attachments();
    auto c = rt.color[0].view, d = rt.depth.view;
    uint32_t colors = 0;
    for (const auto& color : rt.color) colors += bool(color.view);
    bool eyeViewport = m_state.vp.viewportCount == 1
      && qvfr::isEyeViewport(q.size, vp.x, vp.y, vp.width, vp.height);
    auto matchingExtent = [&] (const Rc<DxvkImageView>& view) {
      if (!view) return false;
      auto size = view->mipLevelExtent(0);
      return size.width == q.size.width && size.height == q.size.height;
    };
    const char* gate = !c ? "missing-color" : !d ? "missing-depth" : colors != 1 ? "multiple-color-targets"
      : !matchingExtent(c) || !matchingExtent(d) ? "size" : !eyeViewport ? "viewport"
      : uint32_t(c->info().format) != q.format ? "format"
      : uint32_t(c->image()->info().sampleCount) != q.samples ? "samples"
      : m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
          DxvkGraphicsPipelineFlag::HasTransformFeedback, DxvkGraphicsPipelineFlag::HasLayerExport,
          DxvkGraphicsPipelineFlag::HasRasterizerDiscard, DxvkGraphicsPipelineFlag::HasSampleMaskExport)
        || (!(q.unsafeReplay || (q.sceneOnly && q.unsafeSceneContinue))
          && (shaders.gs || shaders.tcs || shaders.tes)) ? "pipeline-features"
      : "needs-full-validation";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "\"kind\":" << std::quoted(kind) << ",\"reason\":" << std::quoted(std::string(reason))
        << ",\"scene\":" << std::quoted(std::string(scene))
        << ",\"scene_end_reason\":" << std::quoted(q.sceneEndReasonFrame == m_frameCount ? q.sceneEndReason : "none")
        << ",\"static_gate\":" << std::quoted(gate)
        << ",\"instances\":" << instances << ",\"first_instance\":" << firstInstance
        << ",\"pipeline_flags\":" << key[5] << ",\"depth_test\":" << ds.depthTest()
        << ",\"depth_write\":" << ds.depthWrite() << ",\"stencil_test\":" << ds.stencilTest()
        << ",\"depth_compare\":" << uint32_t(ds.depthCompareOp())
        << ",\"viewport_count\":" << m_state.vp.viewportCount << ",\"viewport\":[";
    auto number = [&] (float value) { if (std::isfinite(value)) out << value; else out << "null"; };
    number(vp.x); out << ','; number(vp.y); out << ','; number(vp.width); out << ','; number(vp.height);
    out << "],\"scissor\":[" << sc.offset.x << ',' << sc.offset.y << ',' << sc.extent.width << ',' << sc.extent.height
        << "],\"colors\":[";
    auto attachment = [&] (const Rc<DxvkImageView>& view) {
      if (!view) { out << "null"; return; }
      auto info = view->info(); auto extent = view->mipLevelExtent(0);
      qvfr::writeAttachmentJson(out, {uint64_t(view->image()->getResourceId()), uint64_t(view->image()->handle()),
        uint32_t(info.format), extent.width, extent.height, info.mipIndex, info.layerIndex,
        info.layerCount, uint32_t(view->image()->info().sampleCount)});
    };
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) { if (i) out << ','; attachment(rt.color[i].view); }
    out << "],\"depth\":"; attachment(d);
    out << ",\"shaders\":[";
    for (size_t i = 0; i < stages.size(); i++) {
      if (i) out << ',';
      if (stages[i]) out << std::quoted(stages[i]->debugName()); else out << "null";
    }
    out << ']';
    timing.drawGroups.push_back({key, reason, scene, kind, 1, count, out.str()});
  }

  size_t DxvkContext::beginQvfrTiming(const char* phase) {
    if (!m_qvfr || !m_qvfr->profile || m_frameCount % 120) return size_t(-1);
    auto& q = *m_qvfr;
    // Keep one capture in flight and bound query memory even when the GPU stalls.
    if (!q.timings.empty() && q.profileFrame != m_frameCount) return size_t(-1);
    if (q.profileFrame != m_frameCount) {
      q.replayTimingBudget = {}; q.nativeTimingBudget = {};
      q.profileCaptureFull = q.profileFull;
      q.profileCaptureDetails = q.profileDetails;
      q.profileDetailGroups = 0;
    }
    q.profileFrame = m_frameCount;
    bool replay = std::string_view(phase) == "replay-pass";
    bool native = std::string_view(phase) == "native-pass";
    if (replay || native) {
      auto& budget = replay ? q.replayTimingBudget : q.nativeTimingBudget;
      if (!budget.take(q.profileCaptureFull)) {
        q.profileClipped |= q.profileCaptureFull;
        return size_t(-1);
      }
      phase = replay ? (q.profileCaptureFull ? "replay-pass-total" : "replay-pass-sample")
        : (q.profileCaptureFull ? "native-pass-total" : "native-pass-sample");
    }
    size_t limit = q.profileCaptureFull ? 2 * qvfr::ReplayTimingBudget::FullLimit + 256 : 256;
    if (q.timings.size() >= limit) { q.profileClipped = true; return size_t(-1); }
    DxvkQvfrTiming timing;
    timing.phase = phase;
    timing.cpuBegin = std::chrono::steady_clock::now();
    timing.begin = m_device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    timing.end = m_device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    writeTimestamp(timing.begin);
    auto index = q.timings.size();
    q.timings.push_back(std::move(timing));
    return index;
  }

  void DxvkContext::endQvfrTiming(size_t index) {
    if (index == size_t(-1)) return;
    auto& timing = m_qvfr->timings[index];
    writeTimestamp(timing.end);
    timing.endTrackingId = m_trackingId;
    timing.cpuMs = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - timing.cpuBegin).count();
  }

  void DxvkContext::pollQvfrTimings() {
    if (!m_qvfr || m_qvfr->timings.empty()) return;
    auto& q = *m_qvfr;
    if (m_frameCount <= q.profileFrame + 2) return;
    // Query pools can still contain an old value until the scheduled reset runs.
    // The existing tracking timeline proves this capture's writes have completed.
    auto completed = m_trackingFence->getValue();
    for (const auto& timing : q.timings)
      if (!timing.endTrackingId || completed < timing.endTrackingId) return;
    struct Total { uint32_t samples = 0, failed = 0; double gpuMs = 0, cpuMs = 0; };
    std::map<std::string_view, Total> totals;
    uint32_t bits = m_device->queues().graphics.properties.core.queueFamilyProperties.timestampValidBits;
    uint64_t mask = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
    double msPerTick = m_device->properties().core.properties.limits.timestampPeriod / 1e6;
    struct RankedPass { size_t index; double gpuMs; };
    std::vector<RankedPass> ranked;
    for (size_t index = 0; index < q.timings.size(); index++) {
      const auto& timing = q.timings[index];
      DxvkQueryData begin = {}, end = {};
      auto bs = timing.begin->getData(begin), es = timing.end->getData(end);
      if (bs == DxvkGpuQueryStatus::Pending || es == DxvkGpuQueryStatus::Pending) return;
      auto& total = totals[timing.phase];
      if (bs != DxvkGpuQueryStatus::Available || es != DxvkGpuQueryStatus::Available) {
        total.failed++;
        continue;
      }
      total.samples++;
      double gpuMs = ((end.timestamp.time - begin.timestamp.time) & mask) * msPerTick;
      total.gpuMs += gpuMs;
      total.cpuMs += timing.cpuMs;
      if (q.profileCaptureDetails && (timing.phase == "native-pass-total" || timing.phase == "native-pass-sample"))
        ranked.push_back({index, gpuMs});
    }
    for (const auto& [phase, total] : totals)
      Logger::info(str::format("QVFR timing frame=", q.profileFrame, " phase=", phase,
        " samples=", total.samples, " gpu_ms=", total.gpuMs, " cpu_ms=", total.cpuMs,
        " failed=", total.failed, " clipped=", q.profileClipped,
        " replay_samples=", q.replayTimingBudget.samples, " replay_skipped=", q.replayTimingBudget.skipped,
        " native_samples=", q.nativeTimingBudget.samples, " native_skipped=", q.nativeTimingBudget.skipped,
        " profile_passes=", q.profileCaptureFull ? "full" : "sampled"));
    if (q.profileCaptureDetails) {
      std::stable_sort(ranked.begin(), ranked.end(), [] (const RankedPass& a, const RankedPass& b) { return a.gpuMs > b.gpuMs; });
      size_t emitted = std::min(size_t(16), ranked.size());
      uint64_t metadataDropped = 0, unattributed = 0;
      for (const auto& pass : ranked) {
        metadataDropped += q.timings[pass.index].detailDropped;
        unattributed += q.timings[pass.index].drawGroups.empty();
      }
      Logger::info(str::format("QVFR detail-summary frame=", q.profileFrame,
        " native_passes_ranked=", ranked.size(), " passes_emitted=", emitted,
        " groups_captured=", q.profileDetailGroups,
        " metadata_dropped_batches=", metadataDropped, " unattributed_passes=", unattributed,
        " profile_passes=", q.profileCaptureFull ? "full" : "sampled",
        " clipped=", q.profileClipped, " native_skipped=", q.nativeTimingBudget.skipped));
      for (size_t rank = 0; rank < emitted; rank++) {
        const auto& pass = ranked[rank];
        const auto& timing = q.timings[pass.index];
        Logger::info(str::format("QVFR pass-detail {\"frame\":", q.profileFrame,
          ",\"pass\":", pass.index, ",\"rank\":", rank + 1, ",\"gpu_ms\":", pass.gpuMs,
          ",\"cpu_ms\":", timing.cpuMs, ",\"batches\":", timing.detailBatches,
          ",\"draws\":", timing.detailDraws, ",\"groups\":", timing.drawGroups.size(),
          ",\"dropped_batches\":", timing.detailDropped, "}"));
        for (size_t i = 0; i < timing.drawGroups.size(); i++) {
          const auto& group = timing.drawGroups[i];
          Logger::info(str::format("QVFR draw-group {\"frame\":", q.profileFrame,
            ",\"pass\":", pass.index, ",\"group\":", i, ",\"batches\":", group.batches,
            ",\"draws\":", group.draws, ",", group.metadata, "}"));
        }
      }
    }
    q.timings.clear();
    q.profileClipped = false;
  }

  void DxvkContext::flushQvfr(const char* reason, VkImageAspectFlags aspects) {
    if (!m_qvfr || m_qvfrBusy) return;
    if (qvfr::unsafePublishBoundary(reason)) m_qvfr->sceneResumeBlockedFrame = m_frameCount;
    for (size_t i = 0; i < m_qvfr->cache.size(); i++) flushQvfrGroup(i, reason, aspects);
  }

  void DxvkContext::finishQvfrScene(const char* reason) {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->sceneOnly) return;
    auto& q = *m_qvfr;
    if (!q.sceneCycle.finish(m_frameCount)) return;
    q.sceneEndReasonFrame = m_frameCount;
    q.sceneEndReason = reason;
    // Close before composing. Color is needed by postprocessing now, while
    // private depth/stencil may never be used again before the next clear.
    // Their actual consumers still publish them via the ordinary access hooks.
    flushQvfr("scene-end", q.lazyDepthStencil ? VK_IMAGE_ASPECT_COLOR_BIT
      : VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (q.sceneEnds++ < 16 || !(q.sceneEnds % 120))
      Logger::info(str::format("QVFR scene-end frame=", m_frameCount,
        " reason=", reason, " ends=", q.sceneEnds,
        " vs=", m_state.gp.shaders.vs ? m_state.gp.shaders.vs->debugName() : "none",
        " ps=", m_state.gp.shaders.fs ? m_state.gp.shaders.fs->debugName() : "none",
        " resume=next-endFrame"));
  }

  void DxvkContext::flushQvfrImage(const DxvkImage* image, const char* reason, VkImageAspectFlags aspects) {
    if (!m_qvfr || m_qvfrBusy || !image) return;
    if (deferQvfrPublish(reason)) return;
    if (m_qvfr->sceneOnly && std::string_view(reason) != "replay-target") {
      for (const auto& g : m_qvfr->cache)
        if (g.slot().references(uintptr_t(image))) {
          finishQvfrScene(reason);
          break;
        }
    }
    materializeQvfrVirtual(image, reason);
    for (size_t i = 0; i < m_qvfr->cache.size(); i++)
      if (m_qvfr->cache[i].slot().references(uintptr_t(image))) flushQvfrGroup(i, reason, aspects);
  }

  void DxvkContext::flushQvfrView(const DxvkImageView* view, const char* reason) {
    if (view) flushQvfrImage(view->image(), reason);
  }

  void DxvkContext::flushQvfrDrawTargets(const char* reason) {
    if (!m_qvfr || !m_qvfr->enabled || m_qvfrBusy) return;
    if (deferQvfrPublish(reason)) return;
    auto targets = m_state.om.renderTargets;
    if (m_qvfr->sceneOnly) {
      // Once a native fallback actually writes the original scene target,
      // do not re-enter it later in this frame (e.g. depth-tested UI).
      if (m_qvfr->sceneCycle.frame == m_frameCount && m_qvfr->sceneCycle.closed)
        for (const auto& group : m_qvfr->cache)
          if (targets.color[0].view && group.original.color[0].view
           && targets.color[0].view->image() == group.original.color[0].view->image())
            m_qvfr->sceneResumeBlockedFrame = m_frameCount;
      if (targets.depth.view) {
        auto extent = targets.depth.view->mipLevelExtent(0);
        if (extent.width == m_qvfr->size.width && extent.height == m_qvfr->size.height)
          finishQvfrScene(reason);
      }
      for (const auto& color : targets.color)
        if (color.view) {
          auto extent = color.view->mipLevelExtent(0);
          if (extent.width == m_qvfr->size.width && extent.height == m_qvfr->size.height) {
            finishQvfrScene(reason);
            break;
          }
        }
    }
    auto diagnose = [&] (const char* plane) {
      auto& q = *m_qvfr;
      if (!q.profile || m_frameCount % 120) return;
      if (q.fallbackDiagnosticFrame != m_frameCount) {
        q.fallbackDiagnosticFrame = m_frameCount;
        q.fallbackDiagnosticCount = 0;
      }
      if (q.fallbackDiagnosticCount++ >= 8) return;
      const auto& ds = m_state.dyn.depthStencilState;
      const auto& vp = m_state.vp.viewports[0];
      auto front = ds.stencilOpFront(), back = ds.stencilOpBack();
      Logger::info(str::format("QVFR fallback frame=", m_frameCount, " plane=", plane,
        " reason=", reason, " instances=", q.fallbackInstances, " first_instance=", q.fallbackFirstInstance,
        " pipeline_flags=", uint64_t(m_state.gp.flags.raw()),
        " color_mask=", uint32_t(m_state.gp.state.omBlend[0].colorWriteMask()),
        " depth_test=", ds.depthTest(), " depth_write=", ds.depthWrite(),
        " depth_compare=", uint32_t(ds.depthCompareOp()), " stencil=", ds.stencilTest(),
        " stencil_front=", uint32_t(front.compareOp()), "/", uint32_t(front.writeMask()),
        "/", uint32_t(front.failOp()), "/", uint32_t(front.passOp()), "/", uint32_t(front.depthFailOp()),
        " stencil_back=", uint32_t(back.compareOp()), "/", uint32_t(back.writeMask()),
        "/", uint32_t(back.failOp()), "/", uint32_t(back.passOp()), "/", uint32_t(back.depthFailOp()),
        " viewport=", vp.x, ",", vp.y, ",", vp.width, ",", vp.height,
        " ps=", m_state.gp.shaders.fs ? DxvkShader::getCookie(m_state.gp.shaders.fs) : 0));
    };
    for (uint32_t target = 0; target < MaxNumRenderTargets; target++) {
      const auto& color = targets.color[target];
      // Stencil-only passes frequently keep a color RTV bound with writes
      // disabled. They neither observe nor invalidate its deferred contents.
      if (!color.view || !m_state.gp.state.omBlend[target].colorWriteMask()) continue;
      materializeQvfrVirtual(color.view->image(), "draw-write");
      for (size_t i = 0; i < m_qvfr->cache.size(); i++) {
        auto& group = m_qvfr->cache[i];
        if (!group.pending.any() || !group.original.color[0].view
         || group.original.color[0].view->image() != color.view->image()) continue;
        if (group.pending.color) diagnose("color");
        flushQvfrGroup(i, "fallback-color", VK_IMAGE_ASPECT_COLOR_BIT);
        group.pending.invalidateColor();
      }
    }
    const auto& ds = m_state.dyn.depthStencilState;
    bool depthAccess = ds.depthTest() || ds.depthWrite()
      || m_state.dyn.depthBounds.minDepthBounds > 0.0f
      || m_state.dyn.depthBounds.maxDepthBounds < 1.0f;
    if ((depthAccess || ds.stencilTest()) && targets.depth.view) {
      for (const auto& group : m_qvfr->cache)
        if ((group.pending.depth || group.pending.stencil) && group.original.depth.view
         && group.original.depth.view->image() == targets.depth.view->image()) {
          diagnose(depthAccess ? "depth" : "stencil");
          break;
        }
      if (ds.stencilTest()) {
        flushQvfrView(targets.depth.view.ptr(), "fallback-depth");
      } else {
        // A non-stencil draw cannot observe or change the retained stencil.
        // Keep it on the atlas even when this draw writes original depth.
        auto image = targets.depth.view->image();
        flushQvfrImage(image, "fallback-depth", VK_IMAGE_ASPECT_DEPTH_BIT);
        if (ds.depthTest() && ds.depthWrite())
          for (auto& g : m_qvfr->cache)
            if (g.original.depth.view && g.original.depth.view->image() == image)
              g.pending.invalidateDepth();
      }
    }
  }

  void DxvkContext::flushQvfrShaderImages(bool compute) {
    DxvkQvfrCpuDetailScope cpuDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Dependencies);
    if (!m_qvfr || !m_qvfr->enabled || m_qvfrBusy) return;
    bool pending = !m_qvfr->virtualImages.empty();
    for (const auto& group : m_qvfr->cache) pending |= group.pending.any();
    // Observe candidate reads even when there are no pending planes, and
    // record rejected reader contexts rather than hiding them behind an early return.
    const auto& readerVp = m_state.vp.viewports[0];
    bool sceneReaderShape = !compute && m_state.dyn.depthStencilState.depthTest()
      && m_state.vp.viewportCount == 1 && qvfr::isEyeViewport(m_qvfr->size,
        readerVp.x, readerVp.y, readerVp.width, readerVp.height);
    // Negative reader diagnostics need not add descriptor lookups to every
    // shadow/post draw. Potential scene readers are always observed; other
    // contexts are inspected on the existing sampled profile frames only.
    bool observeResolve = m_qvfr->unsafeSceneContinue && !m_qvfr->staleResolves.empty()
      && (sceneReaderShape || (m_qvfr->profile && !(m_frameCount % 120)));
    bool observeCancelledForward = m_qvfr->hdrForwardCancelledFrame == m_frameCount && m_qvfr->hdrForwardCancelledImage;
    if (!pending && !observeResolve && !observeCancelledForward) return;
    // Only inspect descriptors actually used by the current shader pipeline.
    // Scanning every bound slot would also flush on stale, unused SRVs.
    const DxvkPipelineBindings* layout = nullptr;
    if (compute) {
      auto pipeline = lookupComputePipeline(m_state.cp.shaders);
      if (pipeline) layout = pipeline->getLayout();
    } else {
      // A clean graphics pipeline already has the layout we need. Avoid a
      // second pipeline-table lookup for every scene draw.
      auto pipeline = !m_flags.test(DxvkContextFlag::GpDirtyPipeline)
        ? m_state.gp.pipeline : lookupGraphicsPipeline(m_state.gp.shaders);
      if (pipeline) layout = pipeline->getLayout();
    }
    if (!layout) {
      if (observeResolve) ++m_qvfr->unsafeResolveDecisions["reader_no_layout"];
      return;
    }
    // Only these original images can require publication. Ordinary world and
    // avatar textures used to enter flushQvfrImage and walk every cache slot.
    // Publishing during this scan can remove dependencies but cannot add new
    // application images; keep this conservative snapshot for the whole scan.
    std::array<const DxvkImage*, 8> deferredImages = {};
    size_t deferredCount = 0;
    for (const auto& group : m_qvfr->cache) {
      if (!group.pending.any()) continue;
      if (group.original.color[0].view) deferredImages[deferredCount++] = group.original.color[0].view->image();
      if (group.original.depth.view) deferredImages[deferredCount++] = group.original.depth.view->image();
    }
    qvfr::ImageReadVisits<> visits;
    if (observeResolve) m_qvfr->unsafeResolveScans++;
    auto check = [&] (DxvkPipelineBindingRange range) {
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        const auto& binding = range.bindings[i];
        auto type = binding.getDescriptorType();
        if (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
         || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
          auto view = m_resources[binding.getResourceIndex()].imageView;
          if (!view) continue;
          auto image = view->image();
          if (observeCancelledForward) noteQvfrHdrForwardRead(*image);
          bool sampled = observeResolve && type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          if (sampled) m_qvfr->unsafeResolveSampledImages++;
          // Storage access conservatively observes every plane. Read-only
          // views keep their aspect masks, e.g. depth then stencil separately.
          auto aspects = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            ? image->formatInfo()->aspectMask : view->info().aspects;
          auto visit = visits.visit(uintptr_t(image), aspects, sampled);
          if (!visit.aspects && !visit.observe) {
            m_qvfr->dependencyDuplicates++;
            continue;
          }
          m_qvfr->dependencyImages++;
          if (visit.observe) {
            auto& q = *m_qvfr;
            auto token = qvfrGrabSource(*view->image());
            auto entry = q.staleResolves.find(token ? token.root : view->image()->cookie());
            if (entry != q.staleResolves.end()) {
              if (token && token.hops) q.grabIndirectReads++;
              q.unsafeResolveReads++;
              const auto& rt = m_state.om.renderTargets;
              const auto& vp = m_state.vp.viewports[0];
              auto color = rt.color[0].view, depth = rt.depth.view;
              auto shape = [&] (const Rc<DxvkImageView>& v) {
                if (!v) return false;
                auto extent = v->mipLevelExtent(0);
                return v->info().viewType == VK_IMAGE_VIEW_TYPE_2D && v->info().layerCount == 1
                  && extent.width == q.size.width && extent.height == q.size.height
                  && uint32_t(v->image()->info().sampleCount) == q.samples;
              };
              const char* rejection = qvfr::staleResolveReaderGate(compute,
                m_state.dyn.depthStencilState.depthTest(), m_state.vp.viewportCount == 1
                  && qvfr::isEyeViewport(q.size, vp.x, vp.y, vp.width, vp.height),
                shape(color) && shape(depth) && uint32_t(color->info().format) == q.format,
                entry->second.sourceCookie, entry->second.sourceDepthCookie,
                color ? color->image()->cookie() : 0, depth ? depth->image()->cookie() : 0);
              if (!entry->second.policy.valid) rejection = "reader_uninitialized";
              if (entry->second.policy.external) rejection = "reader_external";
              if (!rejection && !entry->second.policy.sceneReader) {
                entry->second.policy.sceneReader = true;
                q.unsafeResolveReadersLearned++;
                Logger::info(str::format("QVFR unsafe-resolve-learn frame=", m_frameCount,
                  " source_cookie=", entry->second.sourceCookie, " destination_cookie=", entry->first,
                  " consumer_color=", color->image()->cookie(), " consumer_depth=", depth->image()->cookie(),
                  " via_cookie=", view->image()->cookie(), " hops=", token.hops,
                  " skip_supported=", entry->second.supported,
                  " ps=", m_state.gp.shaders.fs ? m_state.gp.shaders.fs->debugName() : "none"));
              } else if (rejection) {
                auto n = ++q.unsafeResolveDecisions[rejection];
                if (n == 1 || (!(m_frameCount % 120) && q.unsafeResolveDecisionLogFrames[rejection] != m_frameCount)) {
                  q.unsafeResolveDecisionLogFrames[rejection] = m_frameCount;
                  Logger::info(str::format("QVFR unsafe-resolve-reader frame=", m_frameCount,
                    " reason=", rejection, " source_cookie=", entry->second.sourceCookie,
                    " destination_cookie=", entry->first, " consumer_color=", color ? color->image()->cookie() : 0,
                    " consumer_depth=", depth ? depth->image()->cookie() : 0,
                    " consumer_color_format=", color ? uint32_t(color->info().format) : 0,
                    " consumer_color_extent=", color ? color->mipLevelExtent(0).width : 0, "x", color ? color->mipLevelExtent(0).height : 0,
                    " consumer_depth_extent=", depth ? depth->mipLevelExtent(0).width : 0, "x", depth ? depth->mipLevelExtent(0).height : 0,
                    " consumer_color_samples=", color ? uint32_t(color->image()->info().sampleCount) : 0,
                    " viewport=", vp.x, ",", vp.y, ",", vp.width, ",", vp.height,
                    " ps=", m_state.gp.shaders.fs ? m_state.gp.shaders.fs->debugName() : "none", " count=", n));
                }
              }
            }
          }
          if (!visit.aspects) continue;
          bool deferred = std::find(deferredImages.begin(), deferredImages.begin() + deferredCount,
            image) != deferredImages.begin() + deferredCount;
          if (!deferred && !m_qvfr->virtualImages.count(image)) {
            m_qvfr->dependencyUnrelated++;
            continue;
          }
          flushQvfrImage(image, compute ? "compute-image" : "shader-image", visit.aspects);
        }
      }
    };
    for (uint32_t s = compute ? 5 : 0; s < (compute ? 6u : 5u); s++)
      if (layout->getNonemptyStageMask() & (1u << s))
        check(layout->getReadOnlyResourcesForStage(VkShaderStageFlagBits(1u << s)));
    check(layout->getReadWriteResources());
  }

  void DxvkContext::flushQvfrSharedImages() {
    if (!m_qvfr || m_qvfrBusy) return;
    if (deferQvfrPublish("shared-image")) return;
    std::vector<const DxvkImage*> sharedVirtual;
    for (const auto& [image, snapshot] : m_qvfr->virtualImages)
      if (image->info().shared) sharedVirtual.push_back(image);
    for (auto image : sharedVirtual) materializeQvfrVirtual(image, "shared-image");
    for (size_t i = 0; i < m_qvfr->cache.size(); i++) {
      const auto& g = m_qvfr->cache[i];
      if (g.pending.any() && (g.original.color[0].view->image()->info().shared
                   || g.original.depth.view->image()->info().shared))
        flushQvfrGroup(i, "shared-image");
    }
  }

  void DxvkContext::noteQvfrExternalImage(const DxvkImage& image, VkImageLayout layout) {
    if (!m_qvfr) return;
    noteQvfrFlowImage("external", image);
    noteQvfrHdrForwardRead(image);
    m_qvfr->hdrReturnHistory.invalidate(image.cookie());
    m_qvfr->resolveReuseHistory.invalidate(image.cookie());
    noteQvfrGrabExternal(image);
    // Any external use blacklists the image, regardless of transition layout.
    if (m_qvfr->unsafeSceneContinue) {
      auto cookie = image.cookie();
      m_qvfr->externalResolveCookies.insert(cookie);
      if (m_qvfr->externalResolveCookies.size() > 256) m_qvfr->externalResolveCookies.erase(m_qvfr->externalResolveCookies.begin());
      auto entry = m_qvfr->staleResolves.find(cookie);
      if (entry != m_qvfr->staleResolves.end()) entry->second.policy.external = true;
    }
    if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return;
    m_qvfr->interopTransition = true;
    const auto& info = image.info();
    // Proton transitions the double-wide, resolved eye image before Submit.
    // Other sizes and intermediate MSAA images do not delimit a VR cycle.
    if (info.type == VK_IMAGE_TYPE_2D && info.extent.width == m_qvfr->size.width
     && info.extent.height == m_qvfr->size.height && info.numLayers == 1
     && info.sampleCount == VK_SAMPLE_COUNT_1_BIT
     && (image.formatInfo()->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
    {
      m_qvfr->outlineCycle.noteSubmission();
      m_qvfr->sceneResumeBlockedFrame = m_frameCount;
      if (m_qvfr->unsafeReplay) m_qvfr->terminalCycle.submitted(uint64_t(image.getResourceId()));
    }
    auto n = ++m_qvfr->externalReads;
    if (n > 16 && n % 240) return;
    Logger::info(str::format("QVFR external-read frame=", m_frameCount,
      " resource=", uint64_t(image.getResourceId()),
      " image=", uint64_t(image.handle()), " format=", uint32_t(info.format),
      " extent=", info.extent.width, "x", info.extent.height,
      " samples=", uint32_t(info.sampleCount)));
  }

  bool DxvkContext::tryQvfrUnsafeSceneResolve(const Rc<DxvkImage>& dst, const Rc<DxvkImage>& src,
      const VkImageResolve& region, VkFormat format, VkResolveModeFlagBits mode, VkResolveModeFlagBits stencilMode) {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->enabled || !m_qvfr->unsafeSceneContinue) return false;
    auto& q = *m_qvfr;
    q.unsafeResolveSeen++;
    bool knownSource = false, pendingColor = false;
    uint64_t sourceDepthCookie = 0, sourceDepthLastUse = 0;
    for (const auto& group : q.cache)
      if (group.original.color[0].view && group.original.color[0].view->image() == src) {
        knownSource = true; pendingColor |= group.pending.color;
        if (group.original.depth.view && group.lastUse >= sourceDepthLastUse) {
          sourceDepthCookie = group.original.depth.view->image()->cookie();
          sourceDepthLastUse = group.lastUse;
        }
      }
    auto decision = [&] (const char* reason) {
      auto n = ++q.unsafeResolveDecisions[reason];
      if (n != 1 && (m_frameCount % 120 || q.unsafeResolveDecisionLogFrames[reason] == m_frameCount)) return;
      q.unsafeResolveDecisionLogFrames[reason] = m_frameCount;
      const auto& si = src->info(); const auto& di = dst->info();
      Logger::info(str::format("QVFR unsafe-resolve-decision frame=", m_frameCount,
        " reason=", reason, " count=", n, " source_cookie=", src->cookie(), " destination_cookie=", dst->cookie(),
        " known_source=", knownSource, " format=", uint32_t(format),
        " source_format=", uint32_t(si.format), " destination_format=", uint32_t(di.format),
        " mode=", uint32_t(mode), " stencil_mode=", uint32_t(stencilMode),
        " source_samples=", uint32_t(si.sampleCount), " destination_samples=", uint32_t(di.sampleCount),
        " source_extent=", si.extent.width, "x", si.extent.height,
        " destination_extent=", di.extent.width, "x", di.extent.height,
        " region=", region.extent.width, "x", region.extent.height, "x", region.extent.depth,
        " source_offset=", region.srcOffset.x, ",", region.srcOffset.y, ",", region.srcOffset.z,
        " destination_offset=", region.dstOffset.x, ",", region.dstOffset.y, ",", region.dstOffset.z,
        " source_aspect=", uint32_t(region.srcSubresource.aspectMask),
        " destination_aspect=", uint32_t(region.dstSubresource.aspectMask),
        " source_mip=", region.srcSubresource.mipLevel, " destination_mip=", region.dstSubresource.mipLevel,
        " source_layer=", region.srcSubresource.baseArrayLayer, " destination_layer=", region.dstSubresource.baseArrayLayer,
        " source_layers=", region.srcSubresource.layerCount, " destination_layers=", region.dstSubresource.layerCount,
        " source_image_layers=", si.numLayers, " destination_image_layers=", di.numLayers,
        " source_resource=", uint64_t(src->getResourceId()), " destination_resource=", uint64_t(dst->getResourceId()),
        " pending_color=", pendingColor));
    };
    // Recognize Unity's mutable HDR backing format as well as the FLOAT view.
    bool hdrAlias = q.format == uint32_t(VK_FORMAT_R16G16B16A16_SFLOAT)
      && format == VK_FORMAT_R16G16B16A16_UNORM
      && src->isViewCompatible(VK_FORMAT_R16G16B16A16_SFLOAT)
      && dst->isViewCompatible(VK_FORMAT_R16G16B16A16_SFLOAT);
    const char* rejection = mode != VK_RESOLVE_MODE_AVERAGE_BIT || stencilMode != VK_RESOLVE_MODE_NONE ? "resolve_mode"
      : uint32_t(format) != q.format && !hdrAlias ? "resolve_format"
      : src->info().sampleCount == VK_SAMPLE_COUNT_1_BIT || dst->info().sampleCount != VK_SAMPLE_COUNT_1_BIT ? "resolve_samples"
      : src == dst ? "resolve_self"
      : region.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || region.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ? "resolve_aspect"
      : region.srcSubresource.mipLevel || region.dstSubresource.mipLevel
        || region.srcSubresource.baseArrayLayer || region.dstSubresource.baseArrayLayer
        || region.srcSubresource.layerCount != 1 || region.dstSubresource.layerCount != 1
        || src->info().numLayers != 1 || dst->info().numLayers != 1 ? "resolve_subresource"
      : region.srcOffset.x || region.srcOffset.y || region.srcOffset.z
        || region.dstOffset.x || region.dstOffset.y || region.dstOffset.z ? "resolve_offset"
      : region.extent.width != q.size.width || region.extent.height != q.size.height || region.extent.depth != 1
        || !src->isFullSubresource(region.srcSubresource, region.extent)
        || !dst->isFullSubresource(region.dstSubresource, region.extent) ? "resolve_extent"
      : !knownSource ? "resolve_unknown_source" : nullptr;
    if (rejection) decision(rejection);
    // Discovery is broader than skip eligibility: keep initialized full-output
    // color candidates even when e.g. format/extent prevents skipping. Their
    // actual readers and rejection reasons remain visible in the next trace.
    bool discover = knownSource && src != dst && src->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
      && dst->info().sampleCount == VK_SAMPLE_COUNT_1_BIT
      && region.srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT
      && region.dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT
      && !region.dstOffset.x && !region.dstOffset.y && !region.dstOffset.z
      && !region.dstSubresource.mipLevel && !region.dstSubresource.baseArrayLayer
      && region.dstSubresource.layerCount == 1 && dst->info().numLayers == 1
      && dst->isFullSubresource(region.dstSubresource, region.extent)
      && mode != VK_RESOLVE_MODE_NONE;
    if (!discover) return false;
    auto cookie = dst->cookie();
    auto it = q.staleResolves.find(cookie);
    if (it == q.staleResolves.end()) {
      if (q.staleResolves.size() >= 32) {
        auto oldest = std::min_element(q.staleResolves.begin(), q.staleResolves.end(),
          [] (const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        q.staleResolves.erase(oldest);
      }
      it = q.staleResolves.emplace(cookie, DxvkQvfrStaleResolve()).first;
    }
    auto& entry = it->second;
    uint64_t sourceStorage = uint64_t(src->getResourceId()), destinationStorage = uint64_t(dst->getResourceId());
    // Renaming the source allocation does not invalidate the already resolved
    // destination contents. Only a different source image or destination
    // storage requires warmup/relearning.
    if (entry.sourceCookie != src->cookie() || entry.destinationStorage != destinationStorage) {
      bool external = entry.policy.external;
      entry = {};
      entry.generation = ++q.grabGeneration;
      entry.policy.external = external;
      entry.sourceCookie = src->cookie(); entry.sourceStorage = sourceStorage; entry.destinationStorage = destinationStorage;
      q.unsafeResolveCandidates++;
      Logger::info(str::format("QVFR unsafe-resolve-candidate frame=", m_frameCount,
        " source_cookie=", src->cookie(), " destination_cookie=", cookie,
        " supported=", rejection == nullptr, " format=", uint32_t(format)));
    }
    entry.sourceStorage = sourceStorage;
    entry.sourceDepthCookie = sourceDepthCookie;
    entry.supported = rejection == nullptr;
    entry.lastUse = m_frameCount;
    entry.policy.external |= dst->info().shared || q.externalResolveCookies.count(cookie);
    bool active = q.sceneCycle.frame == m_frameCount && q.sceneCycle.started && !q.sceneCycle.closed
      && q.sceneResumeBlockedFrame != m_frameCount && pendingColor;
    if (entry.supported && entry.policy.skip(m_frameCount, q.unsafeResolveRefreshFrames, active)) {
      auto n = ++q.unsafeResolvesSkipped;
      if (n <= 16 || !(n % 120))
        Logger::info(str::format("QVFR unsafe-resolve-skip frame=", m_frameCount,
          " source_cookie=", src->cookie(), " destination_cookie=", cookie,
          " age_frames=", m_frameCount - entry.policy.refreshedFrame, " skipped=", n));
      noteQvfrFlowImage("resolve-skip", *dst, src.ptr());
      return true; // No publication, copy, clear, resolve or substitute draw.
    }
    if (!rejection) decision(!entry.policy.valid ? "resolve_warmup"
      : entry.policy.external ? "resolve_external" : !entry.policy.sceneReader ? "resolve_no_scene_reader"
      : !active ? "resolve_inactive_scene" : m_frameCount == entry.policy.refreshedFrame ? "resolve_same_frame"
      : "resolve_refresh_due");
    if (entry.policy.valid && entry.policy.sceneReader && !entry.policy.external) q.unsafeResolveRefreshes++;
    entry.policy.refreshed(m_frameCount);
    entry.refreshPending = true;
    entry.refreshFrame = m_frameCount;
    return false;
  }

  bool DxvkContext::qvfrSceneAllowsDraw() {
    auto& q = *m_qvfr;
    if (q.sceneCycle.allows(m_frameCount)) return true;
    if (!q.sceneResumeAfterResolve || q.sceneResumeBlockedFrame == m_frameCount
     || q.sceneEndReasonFrame != m_frameCount
     || std::string_view(q.sceneEndReason) != "resolve-read"
     || (q.sceneResumeFrame == m_frameCount && q.sceneResumeCount >= 4)) return false;
    const auto& vp = m_state.vp.viewports[0];
    if (!m_state.dyn.depthStencilState.depthTest() || m_state.vp.viewportCount != 1
     || !qvfr::isEyeViewport(q.size, vp.x, vp.y, vp.width, vp.height)) return false;
    const auto& rt = m_state.om.renderTargets;
    // Only a known original pair may re-enter. Postprocess targets, unknown
    // depth attachments and full-width Y-flip/UI passes keep the old boundary.
    for (const auto& group : q.cache)
      if (group.original.color[0].view && group.original.depth.view && group.original == rt) return true;
    return false;
  }

  bool DxvkContext::beginQvfrDraw() {
    DxvkQvfrCpuDetailScope cpuDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Prepare);
    DxvkQvfrCpuDetailScope eligibilityDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Eligibility);
    auto& q = *m_qvfr;
    bool resuming = q.sceneOnly && !q.sceneCycle.allows(m_frameCount);
    if (resuming && !qvfrSceneAllowsDraw()) return false;
    // Native draws since publication may have changed the originals. Respect
    // plane invalidation and import stale data even if unsafe seed skipping was
    // requested; carry this protection to both eyes for the rest of this frame.
    bool skipImageSeed = q.unsafeSkipImageSeed && !resuming
      && !(q.sceneResumeFrame == m_frameCount && q.sceneResumeCount);
    const auto& rt = m_state.om.renderTargets;
    if (!rt.color[0].view || !rt.depth.view) return skipQvfr("missing-attachment");
    if (rt.color[0].shadow || rt.depth.shadow) return skipQvfr("shadow-attachment");
    if (m_state.vp.viewportCount != 1) return skipQvfr("viewport-count");
    if (m_state.dyn.depthStencilState.stencilTest() && !q.stencilEnabled) return skipQvfr("stencil");
    if (m_state.gp.state.om.feedbackLoop()) return skipQvfr("feedback-loop");
    if (m_state.gp.state.useSampleLocations()) return skipQvfr("sample-locations");
    if (m_queryManager.hasEnabledQueries()) return skipQvfr("query");
    for (uint32_t i = 1; i < MaxNumRenderTargets; i++)
      if (rt.color[i].view) return skipQvfr("multiple-color-targets");
    auto c = rt.color[0].view; auto d = rt.depth.view;
    if (q.unsafeReplay && !q.terminalCycle.normal) q.droppedSnapshots += q.virtualImages.erase(c->image());
    else materializeQvfrVirtual(c->image(), "replay-target");
    const auto& ck = c->info(); const auto& dk = d->info();
    const auto& ci = c->image()->info(); const auto& di = d->image()->info();
    if (ck.viewType != VK_IMAGE_VIEW_TYPE_2D || dk.viewType != VK_IMAGE_VIEW_TYPE_2D)
      return skipQvfr("view-type");
    if (ck.mipIndex || dk.mipIndex || ck.layerIndex || dk.layerIndex
     || ck.layerCount != 1 || dk.layerCount != 1 || ci.numLayers != 1 || di.numLayers != 1
     || ci.mipLevels != 1 || di.mipLevels != 1 || ci.type != VK_IMAGE_TYPE_2D || di.type != VK_IMAGE_TYPE_2D)
      return skipQvfr("subresource");
    if (ci.extent.width != q.size.width || ci.extent.height != q.size.height
     || di.extent.width != q.size.width || di.extent.height != q.size.height)
      return skipQvfr("size");
    bool color8 = q.unsafeReplay && (ck.format == VK_FORMAT_R8G8B8A8_UNORM || ck.format == VK_FORMAT_R8G8B8A8_SRGB
      || ck.format == VK_FORMAT_B8G8R8A8_UNORM || ck.format == VK_FORMAT_B8G8R8A8_SRGB);
    if (uint32_t(ck.format) != q.format && !color8) return skipQvfr("format");
    // D3D11 RGBA16_TYPELESS uses an UNORM backing image with a valid FLOAT
    // RTV. Sampling/blitting uses that FLOAT view, and equal-size RGBA16
    // transfers copy the bits unchanged. Requiring identical formats here
    // excludes a normal Unity HDR render target even though both paths work.
    bool hdrAlias = ck.format == VK_FORMAT_R16G16B16A16_SFLOAT
                 && ci.format == VK_FORMAT_R16G16B16A16_UNORM
                 && (ci.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    if ((ck.format != ci.format && !hdrAlias && !(q.unsafeReplay && c->image()->isViewCompatible(ck.format)))
     || dk.format != di.format)
      return skipQvfr("view-base-format");
    if (ci.sampleCount != di.sampleCount || (!q.unsafeReplay && ci.sampleCount != q.samples)) return skipQvfr("samples");
    if (!(dk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
     || dk.aspects != d->image()->formatInfo()->aspectMask) return skipQvfr("depth-aspects");
    if (!(vk::getWritableAspectsForLayout(dk.layout) & VK_IMAGE_ASPECT_DEPTH_BIT))
      return skipQvfr("read-only-depth");
    const auto& ds = m_state.dyn.depthStencilState;
    if (stencilMayWrite(ds)
     && !(vk::getWritableAspectsForLayout(dk.layout) & VK_IMAGE_ASPECT_STENCIL_BIT))
      return skipQvfr("read-only-stencil");
    if (!util::isIdentityMapping(ck.unpackSwizzle())) return skipQvfr("swizzle");

    const auto& vp = m_state.vp.viewports[0];
    const auto& sc = m_state.vp.scissorRects[0];
    uint32_t ew = q.size.width / 2;
    bool ordinaryViewport = qvfr::isEyeViewport(q.size, vp.x, vp.y, vp.width, vp.height);
    bool containedViewport = q.unsafeReplay && vp.width > 0 && vp.height < 0
      && vp.x >= 0 && vp.y <= float(q.size.height) && vp.y + vp.height >= 0
      && (vp.x >= float(ew) ? vp.x + vp.width <= float(q.size.width) : vp.x + vp.width <= float(ew));
    bool fullViewport = q.unsafeReplay && vp.x == 0 && vp.width == float(q.size.width)
      && vp.y == float(q.size.height) && vp.height == -float(q.size.height);
    if ((!ordinaryViewport && !containedViewport && !fullViewport)
     || sc.offset.x < 0 || sc.offset.y < 0) return skipQvfr("viewport");
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline) && !updateGraphicsPipeline())
      return skipQvfr("invalid-pipeline");
    // The final pre-raster stage (VS/TES/GS) still emits its original clip
    // positions. Fixed-function viewport/scissor mapping works after any of
    // these stages; no vertex-only shader rewrite is involved. Keep explicit
    // output/side-effect checks, since replay emits the application draw twice.
    if (m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
        DxvkGraphicsPipelineFlag::HasTransformFeedback, DxvkGraphicsPipelineFlag::HasLayerExport,
        DxvkGraphicsPipelineFlag::HasRasterizerDiscard, DxvkGraphicsPipelineFlag::HasSampleMaskExport)
     || (!(q.unsafeReplay || (q.sceneOnly && q.unsafeSceneContinue))
       && (m_state.gp.shaders.gs || m_state.gp.shaders.tcs || m_state.gp.shaders.tes)))
      return skipQvfr("pipeline-features");
    if (!m_state.gp.shaders.vs || !m_state.gp.shaders.fs) return skipQvfr("missing-shader");
    if (m_state.gp.shaders.vs->metadata().flags.test(DxvkShaderFlag::ExportsViewport)
     || m_state.gp.shaders.fs->metadata().flags.any(DxvkShaderFlag::UsesFragmentCoverage,
          DxvkShaderFlag::ExportsStencilRef)) return skipQvfr("shader-outputs");

    for (const auto& stage : {m_state.gp.shaders.gs, m_state.gp.shaders.tcs, m_state.gp.shaders.tes})
      if (stage && stage->metadata().flags.test(DxvkShaderFlag::ExportsViewport))
        return skipQvfr("shader-outputs");
    uint32_t eye = fullViewport ? q.replayEye : vp.x < float(ew) ? 0 : 1;
    eligibilityDetail.scope.finish();
    DxvkQvfrCpuDetailScope cacheDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::CachePlan);
    std::array<qvfr::CacheSlot, 4> slots;
    for (size_t i = 0; i < slots.size(); i++) slots[i] = q.cache[i].slot();
    size_t index = qvfr::chooseDeferredSlot(slots, uintptr_t(c->image()), uintptr_t(d->image()), eye);
    auto& g = q.cache[index];
    // With a fixed gaze the atlas mapping does not change at a frame boundary.
    // Dependencies and writes invalidate its planes explicitly. Do not expand
    // surviving stencil/color merely to seed the identical mapping again.
    bool reuse = g.pending.any() && g.original == rt && g.eye == eye
      && (g.frame == m_frameCount || (q.virtualEnabled && !q.sharedGaze));
    if (!reuse) {
      flushQvfrGroup(index, "cache-reuse");
      // Seeding reads only this eye, but another target may share its depth.
      for (size_t i = 0; i < q.cache.size(); i++) {
        auto slot = q.cache[i].slot();
        if (i != index && slot.eye == eye
         && (slot.references(uintptr_t(c->image())) || slot.references(uintptr_t(d->image()))))
          flushQvfrGroup(i, "alias-seed");
      }
      if (q.gazeFrame != m_frameCount) {
        q.frameGaze = {};
        q.gazeValid = q.readGaze(q.frameGaze);
        q.gazeFrame = m_frameCount;
      }
      if (!q.gazeValid) return skipQvfr("gaze");
      auto plan = qvfr::makeScenePlan(q.size, q.scale, q.fraction, q.frameGaze, q.focusScale);
      if (!plan) return skipQvfr("plan");
      auto atlas = qvfr::scratchAtlas(plan->eyes[eye], q.stereoAtlas, eye);
      const auto& limits = m_device->properties().core.properties.limits;
      auto fits = [&] (const qvfr::ScratchAtlas& a) {
        return a.extent.width <= std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth)
            && a.extent.height <= std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight);
      };
      bool stereo = q.stereoAtlas && fits(atlas);
      if (q.stereoAtlas && !stereo) {
        atlas = qvfr::scratchAtlas(plan->eyes[eye]);
        q.stereoAtlasFallbacks++;
      }
      if (!fits(atlas)) return skipQvfr("atlas-size");
      if (g.atlas.color[0].view) {
        auto a = g.plan.composite, b = plan->eyes[eye].composite;
        if (a.x != b.x || a.y != b.y) q.seedMappingChanged++;
        else q.seedMappingSame++;
      }
      g.plan = plan->eyes[eye]; g.eye = eye; g.frame = m_frameCount;
      g.placement = atlas; g.stereoAtlas = stereo;
    }

    if (q.atlasPass && m_flags.test(DxvkContextFlag::GpRenderPassActive)) {
      if (q.active != index && q.cache[q.active].eye != eye) q.atlasEyeSwitches++;
      if (reuse && m_state.om.framebufferInfo.attachments() == g.atlas
       && !m_flags.test(DxvkContextFlag::GpRenderPassNeedsFlush)) {
        q.atlasContinuations++;
        q.stereoEyePassKeeps += q.active != index && q.cache[q.active].eye != eye;
      }
    }
    cacheDetail.scope.finish();
    q.active = index;
    g.lastUse = ++q.cacheClock;
    m_qvfrBusy = true;
    q.savedTargets = rt;
    // Eligibility above guarantees one viewport; avoid copying all 16 slots.
    q.savedVp.viewportCount = 1;
    q.savedVp.viewports[0] = vp;
    q.savedVp.scissorRects[0] = sc;
    q.pairPass = ~uint64_t(0);
    if (!reuse) {
      DxvkQvfrCpuDetailScope seedDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Seed);
      endCurrentPass(true);
      auto timing = beginQvfrTiming("seed");
      prepareShaderReadableImages(false); // Materialize clears before reading original attachments.
      DxvkImageUsageInfo usage = {};
      usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      if (!ensureImageCompatibility(c->image(), usage) || !ensureImageCompatibility(d->image(), usage)) {
        endQvfrTiming(timing);
        m_qvfrBusy = false;
        return skipQvfr("image-usage");
      }
      auto atlas = g.placement;
      auto low = atlas.peripheral; auto focus = atlas.focus;
      auto compatible = [&] (const DxvkQvfrGroup& candidate) {
        if (!candidate.atlas.color[0].view || !candidate.atlas.depth.view) return false;
        const auto& extent = candidate.atlas.color[0].view->image()->info().extent;
        return candidate.colorFormat == ck.format && candidate.depthFormat == dk.format
            && candidate.sampleCount == ci.sampleCount
            && candidate.lowW == low.width && candidate.lowH == low.height
            && candidate.focusW == focus.width && candidate.focusH == focus.height
            && extent.width == atlas.extent.width && extent.height == atlas.extent.height;
      };
      const DxvkQvfrGroup* peer = nullptr;
      bool foreignOwner = false;
      for (size_t i = 0; i < q.cache.size(); i++) {
        if (i == index) continue;
        const auto& candidate = q.cache[i];
        // Never share between different original attachment pairs. A cache
        // eviction may leave the other row alive in a different group.
        if (g.atlas.color[0].view && candidate.atlas == g.atlas
         && candidate.original != q.savedTargets) foreignOwner = true;
        if (g.stereoAtlas && candidate.stereoAtlas && candidate.eye != eye
         && candidate.original == q.savedTargets && compatible(candidate)) peer = &candidate;
      }
      if (peer) {
        if (g.atlas != peer->atlas) q.stereoAtlasShares++;
        g.atlas = peer->atlas;
        q.atlasSeedReuses++;
      } else if (!compatible(g) || foreignOwner) {
        const char* cause = !g.atlas.color[0].view ? "initial"
          : g.colorFormat != ck.format || g.depthFormat != dk.format ? "format"
          : g.sampleCount != ci.sampleCount ? "samples" : "dimensions";
        q.atlasRebuilds += bool(g.atlas.color[0].view);
        if (q.atlasImagesCreated < 16 || !(q.atlasImagesCreated % 240))
          Logger::info(str::format("QVFR atlas-create frame=", m_frameCount, " slot=", index,
            " eye=", eye, " cause=", cause, " stereo_atlas=", g.stereoAtlas,
            " extent=", atlas.extent.width, "x", atlas.extent.height,
            " focus_xy=", g.plan.composite.x, ",", g.plan.composite.y,
            " color_format=", uint32_t(ck.format), " depth_format=", uint32_t(dk.format),
            " samples=", uint32_t(ci.sampleCount)));
        auto create = [&] (VkFormat format, qvfr::Rect extent, bool depth) {
          DxvkImageCreateInfo info = {};
          info.type = VK_IMAGE_TYPE_2D; info.format = format; info.sampleCount = ci.sampleCount;
          info.extent = {extent.width, extent.height, 1}; info.numLayers = info.mipLevels = 1;
          info.usage = usage.usage | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
          info.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
          info.access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
          info.layout = VK_IMAGE_LAYOUT_GENERAL;
          auto image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
          q.atlasImagesCreated++;
          initImage(image, VK_IMAGE_LAYOUT_UNDEFINED);
          DxvkImageViewKey key = {};
          key.viewType = VK_IMAGE_VIEW_TYPE_2D; key.format = format;
          key.aspects = image->formatInfo()->aspectMask;
          key.mipCount = key.layerCount = 1; key.layout = VK_IMAGE_LAYOUT_GENERAL;
          key.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
          return image->createView(key);
        };
        qvfr::Rect extent = {0, 0, atlas.extent.width, atlas.extent.height, 0};
        g.atlas.color[0].view = create(ck.format, extent, false);
        g.atlas.depth.view = create(dk.format, extent, true);
      } else q.atlasSeedReuses++;
      g.colorFormat = ck.format; g.depthFormat = dk.format; g.sampleCount = ci.sampleCount;
      g.lowW = low.width; g.lowH = low.height; g.focusW = focus.width; g.focusH = focus.height;
      // Explicitly unsafe: retain previous/initial atlas contents, without a
      // substitute clear. The original attachments are deliberately not imported.
      if (!skipImageSeed) {
        stretch(*this, g.atlas.color[0].view, low, c, g.plan.original);
        stretch(*this, g.atlas.depth.view, low, d, g.plan.original);
        copyCrop(*this, g.atlas.color[0].view, focus, c, g.plan.composite);
        copyCrop(*this, g.atlas.depth.view, focus, d, g.plan.composite);
      } else q.unsafeImageSeedsSkipped++;
      g.original = q.savedTargets;
      g.pending.refreshColor();
      g.pending.refreshDepth();
      g.stencilValid = false;
      q.seededGroups++;
      endQvfrTiming(timing);
    } else {
      if (g.pending.colorStale && skipImageSeed) {
        g.pending.refreshColor();
        q.unsafeImageSeedsSkipped++;
      }
      if (g.pending.colorStale) {
        endCurrentPass(true);
        auto timing = beginQvfrTiming("refresh-color");
        prepareShaderReadableImages(false);
        auto atlas = g.placement;
        stretch(*this, g.atlas.color[0].view, atlas.peripheral, c, g.plan.original);
        copyCrop(*this, g.atlas.color[0].view, atlas.focus, c, g.plan.composite);
        g.pending.refreshColor();
        q.colorRefreshes++;
        endQvfrTiming(timing);
      }
      if (g.pending.depthStale && skipImageSeed) {
        g.pending.refreshDepth();
        q.unsafeImageSeedsSkipped++;
      }
      if (g.pending.depthStale) {
        endCurrentPass(true);
        auto timing = beginQvfrTiming("refresh-depth");
        prepareShaderReadableImages(false);
        auto atlas = g.placement;
        stretch(*this, g.atlas.depth.view, atlas.peripheral, d, g.plan.original);
        copyCrop(*this, g.atlas.depth.view, atlas.focus, d, g.plan.composite);
        g.pending.refreshDepth();
        q.depthRefreshes++;
        endQvfrTiming(timing);
      }
      q.cacheHits++;
    }

    // Most interruptions do not use stencil. Do not run eight bit-copy
    // passes on every seed just because the original depth format has stencil.
    if (q.stencilEnabled && ds.stencilTest() && !g.stencilValid && q.unsafeSkipStencilSeed) {
      g.stencilValid = true; // Treat the omitted import as consumed for this seed.
      q.unsafeStencilSeedsSkipped++;
    }
    if (q.stencilEnabled && ds.stencilTest() && !g.stencilValid) {
      endCurrentPass(true);
      auto timing = beginQvfrTiming("seed-stencil");
      prepareShaderReadableImages(false);
      auto atlas = g.placement;
      copyQvfrStencil(g.atlas.depth.view, atlas.peripheral, d, g.plan.original);
      copyQvfrStencil(g.atlas.depth.view, atlas.focus, d, g.plan.composite);
      g.stencilValid = true;
      endQvfrTiming(timing);
    }

    auto raster = qvfr::replayAtlasViewport(g.plan, {uint32_t(sc.offset.x), uint32_t(sc.offset.y),
      sc.extent.width, sc.extent.height, 0}, false, vp.x, vp.y, vp.width, vp.height, &g.placement);
    DxvkViewport mapped = {};
    mapped.viewport = {float(raster.x), float(raster.y), float(raster.width), float(raster.height), vp.minDepth, vp.maxDepth};
    mapped.scissor = {{int32_t(raster.scissor.x), int32_t(raster.scissor.y)}, {raster.scissor.width, raster.scissor.height}};
    bindRenderTargets(DxvkRenderTargets(g.atlas), 0);
    // endQvfrDraw restored logical application targets, while the physical
    // atlas pass may still be active. Cancel only that redundant target
    // update; real target changes and pending resolves keep the normal path.
    if (q.atlasPass && m_flags.test(DxvkContextFlag::GpRenderPassActive)
     && !m_flags.test(DxvkContextFlag::GpRenderPassNeedsFlush)
     && m_state.om.framebufferInfo.attachments() == g.atlas)
      m_flags.clr(DxvkContextFlag::GpDirtyRenderTargets);
    setViewports(1, &mapped);
    if (resuming && q.sceneCycle.resume(m_frameCount)) {
      if (q.sceneResumeFrame != m_frameCount) { q.sceneResumeFrame = m_frameCount; q.sceneResumeCount = 0; }
      q.sceneResumeCount++;
      q.sceneResumes++;
      if (q.sceneResumes <= 16 || !(q.sceneResumes % 120))
        Logger::info(str::format("QVFR scene-resume frame=", m_frameCount,
          " reason=resolve-read eye=", eye, " resumes=", q.sceneResumes,
          " frame_resumes=", q.sceneResumeCount, " stale_image_seed_skip=disabled"));
    }
    return true;
  }

  void DxvkContext::focusQvfrDraw() {
    DxvkQvfrCpuDetailScope cpuDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Focus);
    auto& q = *m_qvfr;
    auto& g = q.cache[q.active];
    q.pairPass = m_flags.test(DxvkContextFlag::GpRenderPassActive)
      ? q.replayPasses : ~uint64_t(0);
    const auto& vp = q.savedVp.viewports[0];
    const auto& sc = q.savedVp.scissorRects[0];
    auto raster = qvfr::replayAtlasViewport(g.plan, {uint32_t(sc.offset.x), uint32_t(sc.offset.y),
      sc.extent.width, sc.extent.height, 0}, true, vp.x, vp.y, vp.width, vp.height, &g.placement);
    DxvkViewport mapped = {};
    mapped.viewport = {float(raster.x), float(raster.y), float(raster.width), float(raster.height), vp.minDepth, vp.maxDepth};
    mapped.scissor = {{int32_t(raster.scissor.x), int32_t(raster.scissor.y)}, {raster.scissor.width, raster.scissor.height}};
    setViewports(1, &mapped);
  }

  void DxvkContext::endQvfrDraw() {
    DxvkQvfrCpuDetailScope cpuDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Finish);
    auto& q = *m_qvfr;
    auto& g = q.cache[q.active];
    if (q.pairPass == q.replayPasses && m_flags.test(DxvkContextFlag::GpRenderPassActive))
      q.pairedInOnePass++;
    // Restore logical D3D state without spilling the physical atlas pass.
    // updateRenderTargets recognizes the same atlas on the following draw.
    bindRenderTargets(DxvkRenderTargets(q.savedTargets), 0);
    restoreRaster(*this, q.savedVp);
    const auto& ds = m_state.dyn.depthStencilState;
    bool stencil = stencilMayWrite(ds);
    g.pending.draw(stencil);
    drawQvfrSourceOutline(q.active);
    q.stencilBatches += ds.stencilTest();
    q.stencilWriteBatches += stencil;
    q.stencilReadOnlyBatches += ds.stencilTest() && !stencil;
    q.advancedBatches += bool(m_state.gp.shaders.gs || m_state.gp.shaders.tcs || m_state.gp.shaders.tes);
    q.geometryBatches += bool(m_state.gp.shaders.gs);
    q.tessellationBatches += bool(m_state.gp.shaders.tcs || m_state.gp.shaders.tes);
    if ((m_state.gp.shaders.gs && q.geometryBatches == 1)
     || ((m_state.gp.shaders.tcs || m_state.gp.shaders.tes) && q.tessellationBatches == 1))
      Logger::info(str::format("QVFR advanced-replay frame=", m_frameCount,
        " gs=", m_state.gp.shaders.gs ? m_state.gp.shaders.gs->debugName() : "none",
        " tcs=", m_state.gp.shaders.tcs ? m_state.gp.shaders.tcs->debugName() : "none",
        " tes=", m_state.gp.shaders.tes ? m_state.gp.shaders.tes->debugName() : "none",
        " unsafe_scene_continue=", q.unsafeSceneContinue));
    q.replayedBatches++;
    if (q.sceneOnly) q.sceneCycle.rendered(m_frameCount);
    m_qvfrBusy = false;
  }

  void DxvkContext::flushQvfrGroup(size_t index, const char* reason, VkImageAspectFlags aspects) {
    DxvkQvfrCpuDetailScope cpuDetail(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Publish);
    if (!m_qvfr || m_qvfrBusy) return;
    auto& q = *m_qvfr;
    auto& g = q.cache[index];
    if (deferQvfrPublish(reason)) {
      if (std::string_view(reason) == "cache-reuse") {
        // Scratch reuse must not overwrite the only retained color copy.
        // Snapshot both eyes compactly when available; missing planes may be
        // lost in this explicitly destructive-to-visual-quality mode.
        if (g.pending.color && g.original.color[0].view) {
          auto image = Rc<DxvkImage>(g.original.color[0].view->image());
          VkImageSubresourceLayers sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          if (tryQvfrVirtualCopy(image, image, sub, {}, sub, {}, image->info().extent, false))
            q.retainedEvictions++;
          else q.droppedPlanes++;
        }
        q.droppedPlanes += g.pending.depth + g.pending.stencil;
        g.pending = {}; g.stencilValid = false;
      }
      return;
    }
    bool color = g.pending.color && (aspects & VK_IMAGE_ASPECT_COLOR_BIT);
    bool depth = g.pending.depth && (aspects & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool stencil = g.pending.stencil && (aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!color && !depth && !stencil) return;
    if (color) q.hdrReturnHistory.invalidate(g.original.color[0].view->image()->cookie());
    if (color) q.resolveReuseHistory.invalidate(g.original.color[0].view->image()->cookie());
    if (color) noteQvfrFlowImage("compose-color", *g.original.color[0].view->image(), g.atlas.color[0].view->image());
    m_qvfrBusy = true;
    auto targets = m_state.om.renderTargets;
    auto viewports = m_state.vp;
    auto feedback = m_state.gp.state.om.feedbackLoop();
    endCurrentPass(true);
    auto atlas = g.placement;
    auto low = atlas.peripheral; auto focus = atlas.focus;
    // A color-only resolve must not materialize depth or destroy the scratch
    // continuation. Keep both tiles until a real depth dependency or write.
    if (color) {
      auto timing = beginQvfrTiming("compose-color");
      if (!q.colorSinglePass || !composeQvfrColor(index)) {
        if (q.colorSinglePass) q.colorSinglePassFallbacks++;
        stretch(*this, g.original.color[0].view, g.plan.original, g.atlas.color[0].view, low);
        copyCrop(*this, g.original.color[0].view, g.plan.composite, g.atlas.color[0].view, focus);
      }
      endQvfrTiming(timing);
      q.colorComposites++;
    }
    if (depth && q.unsafeSkipDepthCompose) q.unsafeDepthCompositesSkipped++;
    if (depth && !q.unsafeSkipDepthCompose) {
      auto timing = beginQvfrTiming("compose-depth");
      stretch(*this, g.original.depth.view, g.plan.original, g.atlas.depth.view, low);
      copyCrop(*this, g.original.depth.view, g.plan.composite, g.atlas.depth.view, focus);
      endQvfrTiming(timing);
      q.depthComposites++;
    }
    if (stencil && q.unsafeSkipStencilCompose) q.unsafeStencilCompositesSkipped++;
    if (stencil && !q.unsafeSkipStencilCompose) {
      auto timing = beginQvfrTiming("compose-stencil");
      copyQvfrStencil(g.original.depth.view, g.plan.original, g.atlas.depth.view, low);
      copyQvfrStencil(g.original.depth.view, g.plan.composite, g.atlas.depth.view, focus);
      endQvfrTiming(timing);
      q.stencilComposites++;
    }
    bindRenderTargets(std::move(targets), feedback);
    restoreRaster(*this, viewports);
    g.pending.complete(color, depth, stencil);
    q.compositionReasons[reason]++;
    if (q.groups++ < 8)
      Logger::info(str::format("QVFR composed reason=", reason, " eye=", g.eye, " low=", g.lowW, "x", g.lowH,
        " focus=", g.focusW, "x", g.focusH, " color=", color, " depth=", depth, " stencil=", stencil));
    m_qvfrBusy = false;
  }

  bool DxvkContext::composeQvfrColor(size_t index) {
    auto& q = *m_qvfr;
    const auto& g = q.cache[index];
    auto dst = g.original.color[0].view;
    auto src = g.atlas.color[0].view;
    // The scene atlas is 2D floating-point HDR. Other view types retain the
    // established two-step path instead of selecting an incompatible shader.
    if (src->info().viewType != VK_IMAGE_VIEW_TYPE_2D
     || dst->info().viewType != VK_IMAGE_VIEW_TYPE_2D
     || src->info().layerCount != 1 || dst->info().layerCount != 1
     || src->info().format != dst->info().format
     || src->info().format != VK_FORMAT_R16G16B16A16_SFLOAT
     || src->image()->info().sampleCount != dst->image()->info().sampleCount)
      return false;
    endCurrentPass(true);
    invalidateState();
    dst = ensureImageViewCompatibility(dst, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    src = ensureImageViewCompatibility(sampled(src), VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!dst || !src) return false;

    std::array<DxvkResourceAccess, 2> access = {{
      DxvkResourceAccess(*dst, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, false),
      DxvkResourceAccess(*src, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT, false),
    }};
    syncResources(DxvkCmdBuffer::ExecBuffer, access.size(), access.data());
    DxvkMetaBlit::Key key = { };
    key.srcViewType = VK_IMAGE_VIEW_TYPE_2D;
    key.dstFormat = dst->info().format;
    key.dstSamples = dst->image()->info().sampleCount;
    key.srcSamples = src->image()->info().sampleCount;
    key.qvfrComposite = true;
    auto meta = m_common->metaBlit().getPipeline(key);

    const auto& r = g.plan.original;
    VkViewport viewport = {float(r.x), float(r.y), float(r.width), float(r.height), 0.0f, 1.0f};
    VkRect2D area = {{int32_t(r.x), int32_t(r.y)}, {r.width, r.height}};
    VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = dst->handle();
    attachment.imageLayout = dst->getLayout();
    // Only this eye's render area is overwritten. Preserve the other eye.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = area;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    m_cmd->cmdBeginRendering(DxvkCmdBuffer::ExecBuffer, &rendering);
    m_cmd->cmdSetViewport(1, &viewport);
    m_cmd->cmdSetScissor(1, &area);
    m_cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, meta.pipeline);
    DxvkDescriptorWrite descriptor = { };
    descriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor.descriptor = src->getDescriptor();
    auto atlas = g.placement;
    DxvkMetaBlit::Args args = { };
    args.srcCoord0 = {int32_t(atlas.peripheral.x), int32_t(atlas.peripheral.y), 0};
    args.srcCoord1 = {int32_t(atlas.peripheral.x + atlas.peripheral.width),
      int32_t(atlas.peripheral.y + atlas.peripheral.height), 1};
    args.layerCount = 1;
    args.focusDst = {int32_t(g.plan.composite.x), int32_t(g.plan.composite.y)};
    args.focusSrc = {int32_t(atlas.focus.x), int32_t(atlas.focus.y)};
    args.focusExtent = {g.plan.composite.width, g.plan.composite.height};
    args.focusSourceExtent = {atlas.focus.width, atlas.focus.height};
    args.focusFade = float(std::min(q.colorFadePixels,
      std::min(args.focusExtent.width, args.focusExtent.height) / 2));
    m_cmd->bindResources(DxvkCmdBuffer::ExecBuffer, meta.layout,
      1u, &descriptor, sizeof(args), &args);
    m_cmd->cmdDraw(3u, 1u, 0u, 0u);
    m_cmd->cmdEndRendering(DxvkCmdBuffer::ExecBuffer);
    q.colorSinglePassComposites++;
    return true;
  }

  void DxvkContext::copyQvfrStencil(const Rc<DxvkImageView>& dst, qvfr::Rect d,
      const Rc<DxvkImageView>& src, qvfr::Rect s) {
    auto dk = dst->info(), sk = src->info();
    VkImageSubresourceLayers ds = {VK_IMAGE_ASPECT_STENCIL_BIT, dk.mipIndex, dk.layerIndex, 1};
    VkImageSubresourceLayers ss = {VK_IMAGE_ASPECT_STENCIL_BIT, sk.mipIndex, sk.layerIndex, 1};
    VkOffset3D to = {int32_t(d.x), int32_t(d.y), 0}, from = {int32_t(s.x), int32_t(s.y), 0};
    if (d.width == s.width && d.height == s.height) {
      copyImage(dst->image(), ds, to, src->image(), ss, from, {s.width, s.height, 1});
    } else {
      // The existing copy shader supports stencil export or its bitwise
      // fallback. Scale its source coordinates instead of expanding the atlas.
      copyImageFb(dst->image(), ds, to, src->image(), ss, from,
        {d.width, d.height, 1}, {s.width, s.height, 1});
    }
  }
}
