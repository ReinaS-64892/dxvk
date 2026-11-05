#include "dxvk_context.h"
#include "dxvk_qvfr.h"

namespace dxvk {
  void DxvkContext::noteQvfrHdrForwardRead(const DxvkImage& image) {
    if (!m_qvfr || m_qvfrBusy || m_qvfr->hdrForwardCancelledFrame != m_frameCount
     || m_qvfr->hdrForwardCancelledImage != image.cookie()) return;
    auto& q = *m_qvfr;
    q.hdrForwardLateReads++;
    q.unsafeDeferHdrForward = false;
    q.hdrForwardCancelledImage = 0;
    Logger::warn(str::format("QVFR hdr-forward-late-read frame=", m_frameCount,
      " temporary_cookie=", image.cookie(), " defer_disabled=1 current_frame_may_use_old_temporary=1"));
  }

  bool DxvkContext::qvfrResolveReuseKey(const DxvkImage& dst, const DxvkImage& src,
      const VkImageResolve& r, VkFormat format, VkResolveModeFlagBits mode, VkResolveModeFlagBits stencilMode) const {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->enabled || !m_qvfr->resolveReuse) return false;
    const auto& q = *m_qvfr;
    const auto& s = src.info(); const auto& d = dst.info();
    if (format != VK_FORMAT_R16G16B16A16_SFLOAT || q.format != uint32_t(format)
     || mode != VK_RESOLVE_MODE_AVERAGE_BIT || stencilMode != VK_RESOLVE_MODE_NONE
     || s.sampleCount == VK_SAMPLE_COUNT_1_BIT || d.sampleCount != VK_SAMPLE_COUNT_1_BIT
     || s.type != VK_IMAGE_TYPE_2D || d.type != VK_IMAGE_TYPE_2D || s.shared || d.shared
     || (src.memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (dst.memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
     || s.numLayers != 1 || d.numLayers != 1 || s.mipLevels != 1 || d.mipLevels != 1
     || s.extent.width != q.size.width || s.extent.height != q.size.height
     || d.extent.width != q.size.width || d.extent.height != q.size.height
     || r.srcOffset.x || r.srcOffset.y || r.srcOffset.z || r.dstOffset.x || r.dstOffset.y || r.dstOffset.z
     || r.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT
     || r.srcSubresource.mipLevel || r.dstSubresource.mipLevel
     || r.srcSubresource.baseArrayLayer || r.dstSubresource.baseArrayLayer
     || r.srcSubresource.layerCount != 1 || r.dstSubresource.layerCount != 1
     || r.extent.width != q.size.width || r.extent.height != q.size.height || r.extent.depth != 1
     || q.externalResolveCookies.count(src.cookie()) || q.externalResolveCookies.count(dst.cookie())) return false;
    // An original can be unchanged while its private atlas has newer content.
    // Such a resolve must still publish the atlas before any reuse is allowed.
    for (const auto& g : q.cache)
      if (g.pending.color && g.original.color[0].view && g.original.color[0].view->image() == &src) return false;
    return true;
  }

  bool DxvkContext::prepareQvfrHdrReturn(bool suitable) {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->enabled || !m_qvfr->unsafeSkipHdrReturn || !suitable) return false;
    auto& q = *m_qvfr;
    q.hdrReturnWriting = {};
    if (!q.sceneOnly || !q.unsafeSceneContinue || q.sceneCycle.frame != m_frameCount
     || !q.sceneCycle.started || !q.sceneCycle.closed || m_queryManager.hasEnabledQueries()) return false;
    const auto& gp = m_state.gp;
    const auto& ds = m_state.dyn.depthStencilState;
    if (ds.depthTest() || ds.depthWrite() || ds.stencilTest()
     || m_state.vp.viewportCount != 1 || gp.state.om.feedbackLoop() || gp.state.om.enableLogicOp()
     || gp.state.omBlend[0].blendEnable() || gp.state.omBlend[0].colorWriteMask() != 15
     || gp.state.ms.enableAlphaToCoverage() || gp.state.useSampleLocations()) return false;
    if (!gp.shaders.vs || !gp.shaders.fs || gp.shaders.gs || gp.shaders.tcs || gp.shaders.tes
     || gp.shaders.fs->debugName() != "fs.f0bde3d56fe0f0cceffcb66b525c3952"
     || gp.shaders.vs->debugName() != "vs.09f46cdd7650c27388161a211b5e17d5") return false;
    ++q.hdrReturnShaderDraws;
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline) && !updateGraphicsPipeline()) return false;
    if (!gp.pipeline || gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
        DxvkGraphicsPipelineFlag::HasTransformFeedback, DxvkGraphicsPipelineFlag::HasLayerExport,
        DxvkGraphicsPipelineFlag::HasRasterizerDiscard, DxvkGraphicsPipelineFlag::HasSampleMaskExport)
     || gp.shaders.vs->metadata().flags.test(DxvkShaderFlag::ExportsViewport)
     || gp.shaders.fs->metadata().flags.any(DxvkShaderFlag::UsesFragmentCoverage, DxvkShaderFlag::ExportsStencilRef)) return false;
    const auto& rt = m_state.om.renderTargets;
    auto target = rt.color[0].view;
    if (!target) return false;
    for (uint32_t i = 1; i < MaxNumRenderTargets; i++) if (rt.color[i].view) return false;
    auto eligible = [&] (const Rc<DxvkImageView>& view) {
      if (!view) return false;
      const auto& v = view->info(); const auto& i = view->image()->info();
      return v.format == VK_FORMAT_R16G16B16A16_SFLOAT && v.viewType == VK_IMAGE_VIEW_TYPE_2D
        && i.type == VK_IMAGE_TYPE_2D && util::isIdentityMapping(v.unpackSwizzle())
        && !v.mipIndex && v.mipCount == 1 && !v.layerIndex && v.layerCount == 1
        && v.aspects == VK_IMAGE_ASPECT_COLOR_BIT && i.mipLevels == 1 && i.numLayers == 1
        && i.extent.width == q.size.width && i.extent.height == q.size.height
        && !i.shared && !q.externalResolveCookies.count(view->image()->cookie());
    };
    if (!eligible(target)) return false; // In particular, never skip final sRGB output.
    const auto& vp = m_state.vp.viewports[0]; const auto& sc = m_state.vp.scissorRects[0];
    if (!qvfr::isEyeViewport(q.size, vp.x, vp.y, vp.width, vp.height)
     || vp.minDepth != 0 || vp.maxDepth != 1
     || sc.offset.x < 0 || sc.offset.y < 0 || float(sc.offset.x) > vp.x || sc.offset.y != 0
     || uint64_t(sc.offset.x) + sc.extent.width < uint64_t(vp.x + vp.width)
     || sc.extent.height < q.size.height) return false;
    uint32_t eye = vp.x == 0 ? 0 : 1;
    Rc<DxvkImageView> input;
    auto layout = gp.pipeline->getLayout();
    for (uint32_t stage = 0; stage < 5; stage++) {
      if (!(layout->getNonemptyStageMask() & (1u << stage))) continue;
      auto range = layout->getReadOnlyResourcesForStage(VkShaderStageFlagBits(1u << stage));
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        auto binding = range.bindings[i];
        if (binding.getDescriptorType() != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        if (stage != 4 || binding.getResourceIndex() || input) return false;
        input = m_resources[0].imageView;
      }
    }
    if (!eligible(input) || input->image() == target->image()
     || input->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT || !m_samplers[0]) return false;
    const auto& sampler = m_samplers[0]->key().u.p;
    if (sampler.minFilter != VK_FILTER_LINEAR || sampler.magFilter != VK_FILTER_LINEAR
     || sampler.addressU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || sampler.addressV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
     || sampler.pixelCoord || sampler.compareEnable || sampler.anisotropy
     || sampler.reduction != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE) return false;
    auto samples = uint32_t(target->image()->info().sampleCount);
    if (samples > 16) return false;
    auto mask = (1u << samples) - 1;
    if ((gp.state.ms.sampleMask() & mask) != mask) return false;
    if (samples == 1) {
      // First leg: read a freshly resolved original, then execute normally.
      auto token = qvfrGrabSource(*input->image());
      if (!token || token.hops) return false;
      auto source = q.staleResolves.find(token.root);
      if (source == q.staleResolves.end() || source->second.policy.external
       || source->second.policy.refreshedFrame != m_frameCount) return false;
      const auto& s = source->second;
      bool originalKnown = false;
      for (const auto& group : q.cache) {
        auto view = group.original.color[0].view;
        if (view && view->image()->cookie() == s.sourceCookie
         && uint64_t(view->image()->getResourceId()) == s.sourceStorage
         && view->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT && !group.pending.color)
          originalKnown = true;
      }
      if (!originalKnown) return false;
      q.hdrReturnWriting = {m_frameCount, target->image()->cookie(), uint64_t(target->image()->getResourceId()),
        s.sourceCookie, s.sourceStorage, input->image()->cookie(), uint64_t(input->image()->getResourceId()), eye};
      return false;
    }
    for (const auto& group : q.cache)
      if (group.original.color[0].view && group.original.color[0].view->image() == target->image() && group.pending.color)
        return false;
    ++q.hdrReturnAttempts;
    if (!q.hdrReturnHistory.consume(m_frameCount, input->image()->cookie(), uint64_t(input->image()->getResourceId()),
        target->image()->cookie(), uint64_t(target->image()->getResourceId()), eye)) return false;
    auto n = ++q.hdrReturnSkipped;
    const auto& forward = q.hdrForwardPath;
    if (q.unsafeDeferHdrForward && q.hdrForwardTracking == m_trackingId
     && forward.frame == m_frameCount && forward.eye == eye
     && forward.temporary == input->image()->cookie() && forward.original == target->image()->cookie()
     && m_cmd->cancelQvfrDraw(q.hdrForwardTicket)) {
      q.hdrForwardCancelled++;
      q.hdrForwardCancelledFrame = m_frameCount;
      q.hdrForwardCancelledImage = input->image()->cookie();
      q.hdrForwardTicket = 0;
      q.nativeDirectBatches--; q.nativeDirectDraws--;
      noteQvfrFlowImage("hdr-forward-cancel", *input->image());
    }
    if (n <= 8 || !(m_frameCount % 120))
      Logger::info(str::format("QVFR hdr-return-skip frame=", m_frameCount, " eye=", eye,
        " temporary_cookie=", input->image()->cookie(), " original_cookie=", target->image()->cookie(), " skipped=", n));
    noteQvfrFlowImage("hdr-return-skip", *target->image(), input->image());
    return true;
  }

  void DxvkContext::finishQvfrHdrReturn() {
    if (!m_qvfr || !m_qvfr->hdrReturnWriting.temporary) return;
    auto& q = *m_qvfr;
    auto output = m_state.om.renderTargets.color[0].view;
    auto input = m_resources[0].imageView;
    const auto& path = q.hdrReturnWriting;
    if (output && input && output->image()->cookie() == path.temporary
     && uint64_t(output->image()->getResourceId()) == path.temporaryStorage
     && input->image()->cookie() == path.resolved && uint64_t(input->image()->getResourceId()) == path.resolvedStorage
     && q.hdrReturnHistory.record(path)) {
      auto n = ++q.hdrReturnCandidates;
      if (n <= 8 || !(m_frameCount % 120))
        Logger::info(str::format("QVFR hdr-return-capture frame=", m_frameCount, " eye=", path.eye,
          " original_cookie=", path.original, " resolved_cookie=", path.resolved,
          " temporary_cookie=", path.temporary, " candidates=", n));
    }
  }
}
