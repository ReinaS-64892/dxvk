#include "dxvk_context.h"
#include "dxvk_device.h"
#include "dxvk_qvfr.h"
#include "dxvk_shader_ir.h"

#include <iomanip>
#include <locale>
#include <sstream>

namespace dxvk {
  namespace {
    VkOffset3D probeOffset(qvfr::Extent size, uint32_t point) {
      return {int32_t(size.width * ((point & 1) ? 3u : 1u) / 4u),
        int32_t(size.height * ((point & 2) ? 3u : 1u) / 4u), 0};
    }
  }

  bool DxvkContext::beginQvfrOrientationProbe() {
    auto& q = *m_qvfr;
    if (!q.orientationTrace || m_qvfrBusy || q.size.width < 8 || q.size.height < 8
     || q.orientationCount >= 1024 || q.orientationPending.size() >= 8) return false;
    if (q.orientationFrame != m_frameCount) {
      q.orientationFrame = m_frameCount;
      q.orientationFrameCount = 0;
    }
    const auto& rt = m_state.om.renderTargets;
    if (!rt.color[0].view || rt.color[0].shadow || m_state.vp.viewportCount != 1) return false;
    for (uint32_t i = 1; i < MaxNumRenderTargets; i++)
      if (rt.color[i].view) return false;
    auto target = rt.color[0].view;
    bool terminalCandidate = q.terminalCycle.images.count(uint64_t(target->image()->getResourceId()));
    // Reserve two extra captures for learned external targets so earlier
    // image effects cannot consume the final flip pass's entire budget.
    if (q.orientationFrameCount >= (terminalCandidate ? 4u : 2u)) return false;
    const auto& ti = target->image()->info();
    const auto& tk = target->info();
    const auto& v = m_state.vp.viewports[0];
    const auto& s = m_state.vp.scissorRects[0];
    bool full = v.x == 0 && v.width == float(q.size.width);
    bool eye = v.width == float(q.size.width / 2)
      && (v.x == 0 || v.x == float(q.size.width / 2));
    if (ti.extent.width != q.size.width || ti.extent.height != q.size.height
     || ti.sampleCount != VK_SAMPLE_COUNT_1_BIT || tk.mipIndex || tk.layerIndex
     || tk.mipCount != 1 || tk.layerCount != 1 || tk.viewType != VK_IMAGE_VIEW_TYPE_2D
     || (!full && !eye) || v.y != float(q.size.height)
     || v.height != -float(q.size.height) || s.offset.x < 0 || s.offset.y < 0) return false;
    const auto& ds = m_state.dyn.depthStencilState;
    if (ds.depthTest() || ds.stencilTest() || m_queryManager.hasEnabledQueries()
     || m_state.dyn.depthBounds.minDepthBounds > 0 || m_state.dyn.depthBounds.maxDepthBounds < 1)
      return false;
    if (m_flags.test(DxvkContextFlag::GpDirtyPipeline) && !updateGraphicsPipeline()) return false;
    const auto& gp = m_state.gp;
    if (!gp.shaders.fs || !gp.shaders.vs || gp.shaders.gs || gp.shaders.tcs || gp.shaders.tes
     || gp.state.om.feedbackLoop() || gp.state.om.enableLogicOp() || gp.state.useSampleLocations()
     || !gp.state.omBlend[0].colorWriteMask()
     || gp.state.ms.enableAlphaToCoverage() || !(gp.state.ms.sampleMask() & 1)
     || gp.flags.any(DxvkGraphicsPipelineFlag::HasStorageDescriptors,
          DxvkGraphicsPipelineFlag::HasTransformFeedback, DxvkGraphicsPipelineFlag::HasLayerExport,
          DxvkGraphicsPipelineFlag::HasRasterizerDiscard, DxvkGraphicsPipelineFlag::HasSampleMaskExport)
     || gp.shaders.vs->metadata().flags.test(DxvkShaderFlag::ExportsViewport)
     || gp.shaders.fs->metadata().flags.any(DxvkShaderFlag::UsesFragmentCoverage,
          DxvkShaderFlag::ExportsStencilRef, DxvkShaderFlag::HasSampleRateShading)) return false;

    auto shader = gp.shaders.fs;
    uint32_t cookie = DxvkShader::getCookie(shader);
    std::array<bool, qvfr::VirtualSlots> used = {};
    auto pipeline = lookupGraphicsPipeline(gp.shaders);
    if (!pipeline) return false;
    auto resources = pipeline->getLayout()->getReadOnlyResourcesForStage(VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 0; i < resources.bindingCount; i++) {
      const auto& binding = resources.bindings[i];
      uint32_t index = binding.getResourceIndex();
      if (binding.getDescriptorType() == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && index < used.size())
        used[index] = true;
    }
    for (uint32_t slot = 0; slot < qvfr::VirtualSlots; slot++) {
      if (!used[slot]) continue;
      auto source = m_resources[slot].imageView;
      if (!source || source->image() == target->image()) continue;
      auto si = source->mipLevelExtent(0);
      if (si.width != q.size.width || si.height != q.size.height
       || source->info().viewType != VK_IMAGE_VIEW_TYPE_2D
       || source->info().aspects != VK_IMAGE_ASPECT_COLOR_BIT
       || source->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) continue;
      std::array<uint64_t, 6> key = {DxvkShader::getCookie(gp.shaders.vs), cookie,
        uint64_t(source->image()->getResourceId()), uint64_t(target->image()->getResourceId()), slot,
        full ? 0u : v.x == 0 ? 1u : 2u};
      auto last = q.orientationLastCapture.find(key);
      if (last != q.orientationLastCapture.end() && m_frameCount < last->second + 120) continue;
      if (last == q.orientationLastCapture.end() && q.orientationLastCapture.size() >= 512) continue;
      auto shaderKey = std::make_pair(cookie, slot);
      auto cached = q.orientationShaders.find(shaderKey);
      if (cached == q.orientationShaders.end()) {
        if (q.orientationShaders.size() >= 256) continue;
        auto ir = dynamic_cast<DxvkIrShader*>(shader.ptr());
        DxvkQvfrOrientationShader probe;
        if (ir) probe.shader = ir->createQvfrOrientationProbe(slot, probe.samplers);
        cached = q.orientationShaders.emplace(shaderKey, std::move(probe)).first;
        if (!cached->second.shader)
          Logger::info(str::format("QVFR orientation unsupported ps=", shader->debugName(),
            " slot=", slot, " reason=ambiguous-or-unavailable-uv"));
      }
      if (!cached->second.shader) continue;
      bool normalized = true;
      for (uint32_t i = 0; i < 16; i++) if (cached->second.samplers & (1u << i))
        normalized &= m_samplers[i] && !m_samplers[i]->key().u.p.pixelCoord;
      if (!normalized) continue;

      if (!q.orientationTarget) {
        constexpr VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
        auto features = m_device->getFormatFeatures(format).optimal;
        if (!(features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
          q.orientationTrace = false;
          Logger::err("QVFR orientation trace disabled: RGBA32F color attachment unavailable");
          return false;
        }
        DxvkImageCreateInfo info = {};
        info.type = VK_IMAGE_TYPE_2D; info.format = format;
        info.extent = {q.size.width, q.size.height, 1};
        info.mipLevels = info.numLayers = 1; info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
          | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        info.layout = VK_IMAGE_LAYOUT_GENERAL;
        auto image = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        initImage(image, VK_IMAGE_LAYOUT_UNDEFINED);
        DxvkImageViewKey vk = {};
        vk.viewType = VK_IMAGE_VIEW_TYPE_2D; vk.format = format;
        vk.aspects = VK_IMAGE_ASPECT_COLOR_BIT; vk.mipCount = vk.layerCount = 1;
        vk.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; vk.layout = VK_IMAGE_LAYOUT_GENERAL;
        q.orientationTarget = image->createView(vk);
      }
      // Preserve real texture-dependent UV calculations. The diagnostic pass
      // reads materialized application images, never atlas-adjusted coordinates.
      flushQvfrShaderImages(false);
      DxvkQvfrOrientationCapture capture;
      capture.frame = m_frameCount; capture.pass = m_qvfrPass;
      capture.sequence = q.orientationSequence;
      capture.source = key[2]; capture.target = key[3]; capture.slot = slot;
      capture.height = q.size.height;
      capture.terminalCandidate = terminalCandidate;
      capture.viewport = m_state.vp;
      capture.vs = gp.shaders.vs->debugName(); capture.ps = shader->debugName();
      DxvkBufferCreateInfo buffer = {};
      buffer.size = sizeof(qvfr::OrientationPixel) * 16;
      buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      buffer.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
      buffer.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
      buffer.debugName = "QVFR orientation UV readback";
      capture.readback = m_device->createBuffer(buffer,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      q.orientationPending.push_back(std::move(capture));
      q.orientationLastCapture[key] = m_frameCount;
      q.orientationCount++; q.orientationFrameCount++;
      q.orientationSavedTargets = rt;
      q.orientationSavedVp = m_state.vp;
      q.orientationSavedShader = shader;
      q.orientationSavedBlend = m_state.gp.state.omBlend[0];
      m_qvfrBusy = true;
      // Diagnostic UV output must not inherit application alpha blending or
      // write masks. Restore the original state before the actual app draw.
      m_state.gp.state.omBlend[0] = DxvkOmAttachmentBlend(false,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 15);
      m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
      // A full-size attachment keeps SV_Position, interpolation, and VS output
      // identical to the app. Only four small scissor patches are rasterized.
      for (uint32_t point = 0; point < 4; point++)
        clearImageView(q.orientationTarget, probeOffset(q.size, point), {2, 2, 1},
          VK_IMAGE_ASPECT_COLOR_BIT, VkClearValue());
      DxvkRenderTargets targets;
      targets.color[0].view = q.orientationTarget;
      bindRenderTargets(std::move(targets), 0);
      bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(cached->second.shader));
      return true;
    }
    return false;
  }

  void DxvkContext::nextQvfrOrientationProbe(uint32_t point) {
    auto& q = *m_qvfr;
    auto offset = probeOffset(q.size, point);
    DxvkViewport vp = {};
    vp.viewport = q.orientationSavedVp.viewports[0];
    const auto& original = q.orientationSavedVp.scissorRects[0];
    int32_t left = std::max(offset.x, original.offset.x);
    int32_t top = std::max(offset.y, original.offset.y);
    int32_t right = std::min(offset.x + 2, original.offset.x + int32_t(original.extent.width));
    int32_t bottom = std::min(offset.y + 2, original.offset.y + int32_t(original.extent.height));
    vp.scissor = {{left, top}, {uint32_t(std::max(0, right-left)), uint32_t(std::max(0, bottom-top))}};
    setViewports(1, &vp);
  }

  void DxvkContext::endQvfrOrientationProbe() {
    auto& q = *m_qvfr;
    auto& capture = q.orientationPending.back();
    for (uint32_t point = 0; point < 4; point++)
      copyImageToBuffer(capture.readback, point * sizeof(qvfr::OrientationPixel) * 4,
        1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, Rc<DxvkImage>(q.orientationTarget->image()),
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, probeOffset(q.size, point), {2, 2, 1});
    flushBarriers();
    emitMemoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    capture.tracking = m_trackingId;
    bindRenderTargets(std::move(q.orientationSavedTargets), 0);
    bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(std::move(q.orientationSavedShader));
    m_state.gp.state.omBlend[0] = q.orientationSavedBlend;
    m_flags.set(DxvkContextFlag::GpDirtyPipelineState);
    DxvkViewport vp = {};
    vp.viewport = q.orientationSavedVp.viewports[0];
    vp.scissor = q.orientationSavedVp.scissorRects[0];
    setViewports(1, &vp);
    m_qvfrBusy = false;
  }

  void DxvkContext::pollQvfrOrientationProbes() {
    if (!m_qvfr || m_qvfr->orientationPending.empty()) return;
    auto& q = *m_qvfr;
    auto completed = m_trackingFence->getValue();
    for (auto i = q.orientationPending.begin(); i != q.orientationPending.end();) {
      if (!i->tracking || completed < i->tracking) { ++i; continue; }
      std::array<qvfr::OrientationPixel, 16> pixels;
      std::memcpy(pixels.data(), i->readback->mapPtr(0), sizeof(pixels));
      std::ostringstream out;
      out.imbue(std::locale::classic()); out << std::setprecision(9);
      out << "QVFR orientation frame=" << i->frame << " pass=" << i->pass
          << " sequence=" << i->sequence
          << " source=" << i->source << " target=" << i->target << " slot=" << i->slot
          << " terminal_candidate=" << i->terminalCandidate
          << " viewport=" << i->viewport.viewports[0].x << ',' << i->viewport.viewports[0].y
          << ',' << i->viewport.viewports[0].width << ',' << i->viewport.viewports[0].height
          << " vs=" << i->vs << " ps=" << i->ps
          << " left=" << qvfr::classifyOrientation(pixels, 0, i->height)
          << " right=" << qvfr::classifyOrientation(pixels, 1, i->height)
          << " uv_tl_tr_bl_br=";
      for (uint32_t point = 0; point < 4; point++) {
        if (point) out << ';';
        const auto& p = pixels[point*4];
        if (std::isfinite(p.u) && std::isfinite(p.v) && p.valid == 1.f && p.alpha == 1.f)
          out << p.u << ',' << p.v;
        else out << "unknown";
      }
      Logger::info(out.str());
      i = q.orientationPending.erase(i);
    }
  }

  void DxvkContext::traceQvfrOrientationTransfer(const char* kind, const DxvkImage& src, const DxvkImage& dst,
      const VkImageSubresourceLayers& srcSub, VkOffset3D src0, VkOffset3D src1,
      const VkImageSubresourceLayers& dstSub, VkOffset3D dst0, VkOffset3D dst1) {
    noteQvfrFlowTransfer(kind, src, dst, srcSub, src0, src1, dstSub, dst0, dst1);
    if (!m_qvfr || !m_qvfr->orientationTrace || m_qvfrBusy) return;
    auto& q = *m_qvfr;
    auto sequence = ++q.orientationSequence;
    if (q.orientationTransferCount >= 4096) return;
    auto matches = [&] (const DxvkImage& image) {
      return image.info().extent.width == q.size.width && image.info().extent.height == q.size.height;
    };
    if (!matches(src) && !matches(dst)) return;
    std::array<uint64_t, 3> key = {uint64_t(src.getResourceId()), uint64_t(dst.getResourceId()),
      kind[0] == 'c' ? 0u : kind[0] == 'r' ? 1u : 2u};
    auto last = q.orientationTransfers.find(key);
    if (last != q.orientationTransfers.end() && m_frameCount < last->second + 120) return;
    if (last == q.orientationTransfers.end() && q.orientationTransfers.size() >= 512) return;
    q.orientationTransfers[key] = m_frameCount;
    q.orientationTransferCount++;
    Logger::info(str::format("QVFR orientation-transfer frame=", m_frameCount,
      " sequence=", sequence,
      " kind=", kind, " source=", key[0], " target=", key[1],
      " samples=", uint32_t(src.info().sampleCount), "->", uint32_t(dst.info().sampleCount),
      " source_sub=", srcSub.mipLevel, ",", srcSub.baseArrayLayer, ",", srcSub.layerCount, ",", srcSub.aspectMask,
      " target_sub=", dstSub.mipLevel, ",", dstSub.baseArrayLayer, ",", dstSub.layerCount, ",", dstSub.aspectMask,
      " source_rect=", src0.x, ",", src0.y, ",", src0.z, ":", src1.x, ",", src1.y, ",", src1.z,
      " target_rect=", dst0.x, ",", dst0.y, ",", dst0.z, ":", dst1.x, ",", dst1.y, ",", dst1.z));
  }
}
