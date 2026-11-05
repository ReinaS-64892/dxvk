#include "dxvk_context.h"
#include "dxvk_qvfr.h"

namespace dxvk {
  qvfr::GrabToken DxvkContext::qvfrGrabSource(const DxvkImage& image, bool external) {
    if (!m_qvfr || (!m_qvfr->enabled && !external) || !m_qvfr->unsafeSceneContinue || m_qvfrBusy) return {};
    auto& q = *m_qvfr;
    auto cookie = image.cookie();
    auto it = q.staleResolves.find(cookie);
    if (it != q.staleResolves.end()) {
      auto& history = it->second;
      auto storage = uint64_t(image.getResourceId());
      history.policy.observeStorage(history.destinationStorage, storage,
        history.refreshPending, history.refreshFrame, m_frameCount);
      history.destinationStorage = storage;
      history.refreshPending = false;
      if (history.policy.valid) return {cookie, history.generation, m_frameCount, 0};
    }
    auto token = q.grabProvenance.find(cookie, uint64_t(image.getResourceId()), m_frameCount, external);
    if (!token) return {};
    it = q.staleResolves.find(token->root);
    if (it == q.staleResolves.end() || it->second.generation != token->generation || !it->second.policy.valid) return {};
    return *token;
  }

  void DxvkContext::noteQvfrGrabTransfer(const DxvkImage& dst, qvfr::GrabToken source, const char* kind) {
    if (!source || !m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeSceneContinue || m_qvfrBusy) return;
    auto& q = *m_qvfr;
    const auto& info = dst.info();
    auto reject = [&] (const char* reason) {
      auto n = ++q.unsafeResolveDecisions[reason];
      if (n == 1 || (!(m_frameCount % 120) && q.unsafeResolveDecisionLogFrames[reason] != m_frameCount)) {
        q.unsafeResolveDecisionLogFrames[reason] = m_frameCount;
        Logger::info(str::format("QVFR grab-reject frame=", m_frameCount, " reason=", reason,
          " kind=", kind, " root_cookie=", source.root, " destination_cookie=", dst.cookie(), " count=", n));
      }
    };
    if (info.type != VK_IMAGE_TYPE_2D || info.sampleCount != VK_SAMPLE_COUNT_1_BIT
     || info.numLayers != 1 || info.mipLevels != 1) { reject("grab_transfer_shape"); return; }
    auto root = q.staleResolves.find(source.root);
    if (root == q.staleResolves.end() || root->second.generation != source.generation || !root->second.policy.valid) {
      reject("grab_transfer_stale_root"); return;
    }
    if (!q.grabProvenance.link(dst.cookie(), uint64_t(dst.getResourceId()), m_frameCount, source)) {
      reject("grab_transfer_limit"); return;
    }
    auto n = ++q.grabLinks;
    if (n <= 16 || !(m_frameCount % 120 && n % 240))
      Logger::info(str::format("QVFR grab-link frame=", m_frameCount, " kind=", kind,
        " root_cookie=", source.root, " destination_cookie=", dst.cookie(), " hops=", source.hops + 1, " links=", n));
    if (info.shared || q.externalResolveCookies.count(dst.cookie())) noteQvfrGrabExternal(dst);
  }

  void DxvkContext::invalidateQvfrGrabImage(const DxvkImage& image) {
    if (!m_qvfr || m_qvfrBusy) return;
    auto& q = *m_qvfr;
    if (q.resolveReuse) q.resolveReuseHistory.invalidate(image.cookie());
    if (q.hdrForwardCancelledImage == image.cookie()) q.hdrForwardCancelledImage = 0;
    if (!q.unsafeSceneContinue) return;
    q.grabProvenance.invalidate(image.cookie());
    if (q.unsafeSkipHdrReturn)
      q.hdrReturnHistory.invalidate(image.cookie(), q.hdrReturnWriting.temporary ? &q.hdrReturnWriting : nullptr);
    auto root = q.staleResolves.find(image.cookie());
    if (root == q.staleResolves.end()) return;
    auto& history = root->second;
    // Writes belonging to the pending real resolve initialize this image.
    if (history.refreshPending && history.refreshFrame == m_frameCount) return;
    history.policy.valid = history.policy.sceneReader = false;
    history.generation = ++q.grabGeneration;
  }

  void DxvkContext::finishQvfrGrabResolve(const DxvkImage& image) {
    if (!m_qvfr || !m_qvfr->unsafeSceneContinue || m_qvfrBusy) return;
    auto it = m_qvfr->staleResolves.find(image.cookie());
    if (it == m_qvfr->staleResolves.end() || !it->second.refreshPending || it->second.refreshFrame != m_frameCount) return;
    it->second.destinationStorage = uint64_t(image.getResourceId());
    it->second.refreshPending = false;
    // Some inline resolves materialize later; qvfrGrabSource also observes
    // any rename at the actual read. Do not retain an image reference here.
  }

  void DxvkContext::noteQvfrGrabExternal(const DxvkImage& image) {
    auto token = qvfrGrabSource(image, true);
    if (!token || !m_qvfr) return;
    auto& q = *m_qvfr;
    auto it = q.staleResolves.find(token.root);
    if (it == q.staleResolves.end() || it->second.policy.external) return;
    it->second.policy.external = true;
    q.externalResolveCookies.insert(token.root);
    if (q.externalResolveCookies.size() > 256) q.externalResolveCookies.erase(q.externalResolveCookies.begin());
    auto n = ++q.grabTerminals;
    Logger::info(str::format("QVFR grab-terminal frame=", m_frameCount,
      " root_cookie=", token.root, " external_cookie=", image.cookie(), " terminals=", n));
  }

  void DxvkContext::invalidateQvfrGrabWrites(bool compute) {
    if (!m_qvfr || (!m_qvfr->unsafeSceneContinue && !m_qvfr->resolveReuse) || m_qvfrBusy) return;
    if (!compute)
      for (const auto& color : m_state.om.renderTargets.color)
        if (color.view) invalidateQvfrGrabImage(*color.view->image());
    const DxvkPipelineBindings* layout = nullptr;
    if (compute) {
      auto pipeline = lookupComputePipeline(m_state.cp.shaders);
      if (pipeline) layout = pipeline->getLayout();
    } else if (m_state.gp.pipeline) layout = m_state.gp.pipeline->getLayout();
    if (!layout) return;
    auto range = layout->getReadWriteResources();
    for (uint32_t i = 0; i < range.bindingCount; i++) {
      auto binding = range.bindings[i];
      if (binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        auto view = m_resources[binding.getResourceIndex()].imageView;
        if (view) invalidateQvfrGrabImage(*view->image());
      }
    }
  }

  qvfr::GrabToken DxvkContext::prepareQvfrGrabDraw(bool suitable, Rc<DxvkImage>* destination) {
    if (!suitable || !m_qvfr || !m_qvfr->enabled || !m_qvfr->unsafeSceneContinue || m_qvfrBusy) return {};
    auto& q = *m_qvfr;
    q.grabShaderProbes++;
    const auto& rt = m_state.om.renderTargets;
    const auto& ds = m_state.dyn.depthStencilState;
    if (!rt.color[0].view || m_state.vp.viewportCount != 1 || ds.depthTest() || ds.depthWrite() || ds.stencilTest()
     || m_state.gp.state.omBlend[0].blendEnable() || m_state.gp.state.omBlend[0].colorWriteMask() != 15
     || m_state.gp.shaders.gs || m_state.gp.shaders.tcs || m_state.gp.shaders.tes
     || m_state.gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors, DxvkGraphicsPipelineFlag::HasTransformFeedback)) return {};
    for (size_t i = 1; i < MaxNumRenderTargets; i++) if (rt.color[i].view) return {};
    auto view = rt.color[0].view;
    auto info = view->info(); auto size = view->mipLevelExtent(0);
    const auto& vp = m_state.vp.viewports[0]; const auto& sc = m_state.vp.scissorRects[0];
    if (info.mipIndex || info.layerIndex || info.layerCount != 1 || info.viewType != VK_IMAGE_VIEW_TYPE_2D
     || view->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT
     || vp.x != 0 || vp.width != float(size.width)
     || !((vp.y == float(size.height) && vp.height == -float(size.height)) || (vp.y == 0 && vp.height == float(size.height)))
     || sc.offset.x != 0 || sc.offset.y != 0 || sc.extent.width < size.width || sc.extent.height < size.height
     || !m_state.gp.pipeline) return {};
    qvfr::GrabToken source;
    auto layout = m_state.gp.pipeline->getLayout();
    for (uint32_t stage = 0; stage < 5; stage++) {
      if (!(layout->getNonemptyStageMask() & (1u << stage))) continue;
      auto range = layout->getReadOnlyResourcesForStage(VkShaderStageFlagBits(1u << stage));
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        auto binding = range.bindings[i];
        if (binding.getDescriptorType() != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        auto input = m_resources[binding.getResourceIndex()].imageView;
        if (!input || input->image() == view->image()) continue;
        auto token = qvfrGrabSource(*input->image());
        if (!token) continue;
        if (source && (source.root != token.root || source.generation != token.generation)) {
          ++q.unsafeResolveDecisions["grab_shader_multiple_roots"];
          return {};
        }
        if (!source || token.hops > source.hops) source = token;
      }
    }
    if (source) {
      q.grabShaderSources++;
      *destination = Rc<DxvkImage>(view->image());
    } else ++q.unsafeResolveDecisions["grab_shader_no_source"];
    return source;
  }
}
