#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_qvfr.h"
#include "../util/util_qvfr_post.h"

namespace dxvk {
  namespace {
    qvfr::ReplayRaster postRaster(const qvfr::EyePlan& eye, const DxvkViewportState& vp,
        bool focus, const qvfr::Rect* destination = nullptr) {
      const auto& v = vp.viewports[0];
      const auto& s = vp.scissorRects[0];
      auto r = qvfr::replayAtlasViewport(eye,
        {uint32_t(s.offset.x), uint32_t(s.offset.y), s.extent.width, s.extent.height, 0},
        focus, v.x, v.y, v.width, v.height);
      if (destination) {
        auto local = qvfr::scratchAtlas(eye);
        auto tile = focus ? local.focus : local.peripheral;
        int32_t dx = int32_t(destination->x) - int32_t(tile.x);
        int32_t dy = int32_t(destination->y) - int32_t(tile.y);
        r.x += dx; r.y += dy;
        r.scissor.x = uint32_t(int32_t(r.scissor.x) + dx);
        r.scissor.y = uint32_t(int32_t(r.scissor.y) + dy);
      }
      return r;
    }
  }

  bool DxvkContext::beginQvfrPostDraw() {
    if (!m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafePost || m_qvfrBusy) return false;
    auto& q = *m_qvfr;
    const auto& rt = m_state.om.renderTargets;
    if (!rt.color[0].view || rt.color[0].shadow || m_state.vp.viewportCount != 1) return false;
    if (!m_state.gp.state.omBlend[0].colorWriteMask()) return false;
    for (uint32_t i = 1; i < MaxNumRenderTargets; i++)
      if (rt.color[i].view) return false;
    const auto& ds = m_state.dyn.depthStencilState;
    if (m_queryManager.hasEnabledQueries()) return false;
    const auto& v = m_state.vp.viewports[0];
    const auto& sc = m_state.vp.scissorRects[0];
    bool fullViewport = v.x == 0 && v.y == float(q.size.height) && v.width == float(q.size.width)
      && v.height == -float(q.size.height);
    if (!qvfr::postReplayEligible(fullViewport, ds.depthTest(), ds.stencilTest(),
        m_state.dyn.depthBounds.minDepthBounds > 0 || m_state.dyn.depthBounds.maxDepthBounds < 1)
     || sc.offset.x < 0 || sc.offset.y < 0) return false;
    auto color = rt.color[0].view;
    auto k = color->info();
    const auto& i = color->image()->info();
    bool hdr = k.format == VK_FORMAT_R16G16B16A16_SFLOAT && q.format == uint32_t(k.format);
    bool color8 = q.unsafeReplay && i.sampleCount == VK_SAMPLE_COUNT_1_BIT
      && (k.format == VK_FORMAT_R8G8B8A8_UNORM || k.format == VK_FORMAT_R8G8B8A8_SRGB
       || k.format == VK_FORMAT_B8G8R8A8_UNORM || k.format == VK_FORMAT_B8G8R8A8_SRGB);
    if ((!hdr && !color8)
     || k.aspects != VK_IMAGE_ASPECT_COLOR_BIT || i.tiling != VK_IMAGE_TILING_OPTIMAL
     || k.viewType != VK_IMAGE_VIEW_TYPE_2D || k.mipIndex || k.layerIndex || k.mipCount != 1 || k.layerCount != 1
     || i.type != VK_IMAGE_TYPE_2D || i.mipLevels != 1 || i.numLayers != 1
     || i.extent.width != q.size.width || i.extent.height != q.size.height
     || !util::isIdentityMapping(k.unpackSwizzle()) || (i.shared && !q.unsafeReplay)
     || (color->image()->memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return false;
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline) && !updateGraphicsPipeline()) return false;
    if (m_state.gp.state.om.feedbackLoop() || m_state.gp.state.useSampleLocations()
     || m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
        DxvkGraphicsPipelineFlag::HasTransformFeedback, DxvkGraphicsPipelineFlag::HasLayerExport,
        DxvkGraphicsPipelineFlag::HasRasterizerDiscard, DxvkGraphicsPipelineFlag::HasSampleMaskExport)
     || (!q.unsafeReplay && (m_state.gp.shaders.gs || m_state.gp.shaders.tcs || m_state.gp.shaders.tes))
     || !m_state.gp.shaders.vs || !m_state.gp.shaders.fs) return false;
    if (m_state.gp.shaders.vs->metadata().flags.test(DxvkShaderFlag::ExportsViewport)
     || m_state.gp.shaders.fs->metadata().flags.any(DxvkShaderFlag::UsesFragmentCoverage,
          DxvkShaderFlag::ExportsStencilRef)) return false;
    for (const auto& stage : {m_state.gp.shaders.gs, m_state.gp.shaders.tcs, m_state.gp.shaders.tes})
      if (stage && stage->metadata().flags.test(DxvkShaderFlag::ExportsViewport)) return false;

    bool snapshot = i.sampleCount == VK_SAMPLE_COUNT_1_BIT;
    // Virtual sampling uses a shader variant; identify the app's original PS.
    auto sourceShader = q.virtualSavedShader ? q.virtualSavedShader : m_state.gp.shaders.fs;
    auto vsName = m_state.gp.shaders.vs->debugName();
    auto psName = sourceShader->debugName();
    bool preserveSource = !m_state.gp.shaders.gs && !m_state.gp.shaders.tcs && !m_state.gp.shaders.tes
      && qvfr::measuredIdentityPost(vsName, psName, uint32_t(k.format));
    uint32_t sourceSlot = 0;
    uint64_t sourceImage = 0;
    std::array<qvfr::EyePlan, 2> eyes;
    std::array<uint32_t, 2> sourceY = {};
    qvfr::VirtualLayout layout = {};
    if (!snapshot) {
      if (!q.unsafeReplay && i.sampleCount != q.samples) return false;
      std::array<bool, 2> found = {};
      for (size_t index = 0; index < q.cache.size(); index++) {
        auto& g = q.cache[index];
        if (!g.pending.any() || g.pending.colorStale || !g.original.color[0].view
         || g.original.color[0].view != color || (rt.depth.view && g.original.depth != rt.depth)) continue;
        q.postGroups[g.eye] = index;
        eyes[g.eye] = g.plan;
        found[g.eye] = true;
      }
      if (!found[0] || !found[1]) return false;
    } else {
      // Only replace-style, full-image postprocess outputs are eligible for a
      // new snapshot. The explicitly unsafe mode assumes full-screen geometry;
      // uncovered pixels start at zero instead of preserving the old target.
      if (!q.unsafeReplay && (m_state.gp.state.omBlend[0].blendEnable()
       || m_state.gp.state.omBlend[0].colorWriteMask() != 15 || m_state.gp.state.om.enableLogicOp()
       || sc.offset.x || sc.offset.y || sc.extent.width < q.size.width || sc.extent.height < q.size.height))
        return false;
      bool source = false;
      for (const auto& [slot, original] : q.virtualBindings) {
        // The measured identity coordinates belong to PS t0. Do not apply
        // that observation to a different input merely because it is first.
        if (preserveSource && (slot != 0 || original->info().format != VK_FORMAT_R16G16B16A16_SFLOAT)) continue;
        auto entry = q.virtualImages.find(original->image());
        if (entry != q.virtualImages.end()) {
          eyes = entry->second.eyes;
          sourceSlot = slot;
          sourceImage = uint64_t(original->image()->getResourceId());
          source = true;
          break;
        }
      }
      if (!source) return false;
      sourceY = {eyes[0].composite.y, eyes[1].composite.y};
      // Preserve the measured identity pass's source ROI for rasterization
      // AND output mapping. Other passes keep the existing unsafe heuristic.
      if (!preserveSource && !q.unsafeReplay && (q.gazeFrame != m_frameCount || !q.gazeValid)) return false;
      auto output = qvfr::postSnapshotEyes(q.size, q.scale, q.fraction,
        eyes, q.frameGaze, preserveSource || q.unsafeReplay);
      if (!output) return false;
      eyes = *output;
      layout = qvfr::virtualLayout(q.size, eyes);
    }
    const auto& limits = m_device->properties().core.properties.limits;
    if (snapshot && (layout.extent.width > std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth)
     || layout.extent.height > std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight))) return false;
    for (uint32_t eye = 0; eye < 2; eye++) for (bool focus : {false, true}) {
      auto r = postRaster(eyes[eye], m_state.vp, focus,
        snapshot ? &(focus ? layout.focus[eye] : layout.low[eye]) : nullptr);
      if (r.width > limits.maxViewportDimensions[0] || -r.height > limits.maxViewportDimensions[1]
       || r.x < limits.viewportBoundsRange[0] || r.x + r.width > limits.viewportBoundsRange[1]
       || r.y + r.height < limits.viewportBoundsRange[0] || r.y > limits.viewportBoundsRange[1]) return false;
    }

    if (snapshot) {
      // Establish compatibility before the logical Vulkan image can be exported.
      DxvkImageUsageInfo usage = {};
      usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      m_qvfrBusy = true;
      bool compatible = ensureImageCompatibility(color->image(), usage);
      m_qvfrBusy = false;
      if (!compatible) return false;
      q.virtualImages.erase(color->image());
      flushQvfrImage(color->image(), "post-overwrite");
      if (q.unsafeReplay)
        for (auto& g : q.cache)
          if (g.original.color[0].view && g.original.color[0].view->image() == color->image()) {
            // The new compact output supersedes the old scratch color. Do
            // not let an external handoff later publish the stale plane over it.
            q.discardedColor += g.pending.color;
            g.pending.complete(true, false);
            g.pending.invalidateColor();
          }
      if (q.virtualImages.size() >= 8) {
        if (q.unsafeReplay && !q.terminalCycle.normal) {
          q.virtualImages.erase(q.virtualImages.begin()); q.droppedSnapshots++;
        } else materializeQvfrVirtual(q.virtualImages.begin()->first, "post-evict");
      }
    }
    q.savedTargets = rt;
    q.savedVp = m_state.vp;
    q.postDepthDisabled = snapshot && q.unsafeReplay;
    if (q.postDepthDisabled) {
      q.postSavedDepth = m_state.dyn.depthStencilState;
      q.postSavedBounds = m_state.dyn.depthBounds;
      setDepthStencilState(DxvkDepthStencilState{});
      setDepthBounds({0.f, 1.f});
    }
    m_qvfrBusy = true;
    q.postTiming = beginQvfrTiming("post-replay");
    if (snapshot) {
      endCurrentPass(true);
      DxvkImageCreateInfo info = {};
      info.type = VK_IMAGE_TYPE_2D; info.format = k.format;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent = {layout.extent.width, layout.extent.height, 1};
      info.mipLevels = info.numLayers = 1;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      info.access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      info.layout = VK_IMAGE_LAYOUT_GENERAL;
      auto image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      initImage(image, VK_IMAGE_LAYOUT_UNDEFINED);
      auto key = k;
      key.layout = VK_IMAGE_LAYOUT_GENERAL;
      key.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      auto atlas = image->createView(key);
      clearImageView(atlas, {}, info.extent, VK_IMAGE_ASPECT_COLOR_BIT, VkClearValue());
      q.postSnapshot = DxvkQvfrVirtualImage{Rc<DxvkImage>(color->image()), atlas, layout, eyes};
      // Count actual snapshot passes, not desktop frame numbers: the XR post
      // pass need not coincide with report frames or initialization frames.
      if (q.postSnapshotBatches < 8 || !(q.postSnapshotBatches % 120))
        Logger::info(str::format("QVFR post-output frame=", m_frameCount,
          " format=", uint32_t(k.format),
          " mapping=", preserveSource ? "measured-identity-y" : q.unsafeReplay ? "unsafe-inherit" : "default-flip-y",
          " bridge_v=", q.frameGaze[0].v, ",", q.frameGaze[1].v,
          " source=", sourceImage, " target=", uint64_t(color->image()->getResourceId()),
          " source_slot=", sourceSlot, " vs=", vsName, " ps=", psName,
          " source_y=", sourceY[0], ",", sourceY[1],
          " output_y=", eyes[0].composite.y, ",", eyes[1].composite.y));
    }
    return true;
  }

  void DxvkContext::nextQvfrPostDraw(uint32_t pass) {
    auto& q = *m_qvfr;
    uint32_t eye = pass / 2;
    bool focus = pass & 1;
    DxvkRenderTargets targets;
    qvfr::ReplayRaster r;
    if (q.postSnapshot) {
      auto& s = *q.postSnapshot;
      targets.color[0].view = s.atlas;
      r = postRaster(s.eyes[eye], q.savedVp, focus,
        &(focus ? s.layout.focus[eye] : s.layout.low[eye]));
    } else {
      auto& g = q.cache[q.postGroups[eye]];
      targets = g.atlas;
      if (!q.savedTargets.depth.view) targets.depth = {};
      r = postRaster(g.plan, q.savedVp, focus);
    }
    const auto& v = q.savedVp.viewports[0];
    DxvkViewport viewport = {};
    viewport.viewport = {float(r.x), float(r.y), float(r.width), float(r.height), v.minDepth, v.maxDepth};
    viewport.scissor = {{int32_t(r.scissor.x), int32_t(r.scissor.y)}, {r.scissor.width, r.scissor.height}};
    bindRenderTargets(std::move(targets), 0);
    setViewports(1, &viewport);
    q.postPasses++;
  }

  void DxvkContext::endQvfrPostDraw() {
    auto& q = *m_qvfr;
    if (q.postSnapshot) {
      auto image = q.postSnapshot->logical.ptr();
      q.virtualImages.emplace(image, std::move(*q.postSnapshot));
      q.postSnapshot.reset();
      q.postSnapshotBatches++;
    } else {
      for (auto index : q.postGroups) {
        auto& g = q.cache[index];
        g.pending.color = true;
        // The source marker travels through the application's postprocess.
        // Do not stamp another marker at the old ROI after that transform.
        g.pending.refreshColor();
        g.lastUse = ++q.cacheClock;
      }
      q.postCacheBatches++;
    }
    bindRenderTargets(std::move(q.savedTargets), 0);
    if (q.postDepthDisabled) {
      setDepthStencilState(q.postSavedDepth);
      setDepthBounds(q.postSavedBounds);
      q.postDepthDisabled = false;
    }
    std::array<DxvkViewport, DxvkLimits::MaxNumViewports> vp;
    for (uint32_t i = 0; i < q.savedVp.viewportCount; i++)
      vp[i] = {q.savedVp.viewports[i], q.savedVp.scissorRects[i]};
    setViewports(q.savedVp.viewportCount, vp.data());
    endQvfrTiming(q.postTiming);
    q.postTiming = size_t(-1);
    m_qvfrBusy = false;
  }
}
