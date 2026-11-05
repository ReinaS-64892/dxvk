#include "dxvk_context.h"
#include "dxvk_qvfr.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace dxvk {
  namespace {
    void imageJson(std::ostream& out, const DxvkImage& image) {
      const auto& i = image.info();
      out << "{\"cookie\":" << image.cookie() << ",\"storage\":" << uint64_t(image.getResourceId())
          << ",\"format\":" << uint32_t(i.format) << ",\"width\":" << i.extent.width
          << ",\"height\":" << i.extent.height << ",\"samples\":" << uint32_t(i.sampleCount)
          << ",\"shared\":" << bool(i.shared) << '}';
    }
    void viewJson(std::ostream& out, const DxvkImageView* view) {
      if (!view) { out << "null"; return; }
      auto i = view->info(); auto extent = view->mipLevelExtent(0);
      out << "{\"image\":"; imageJson(out, *view->image());
      out << ",\"format\":" << uint32_t(i.format) << ",\"width\":" << extent.width
          << ",\"height\":" << extent.height << ",\"mip\":" << uint32_t(i.mipIndex)
          << ",\"layer\":" << uint32_t(i.layerIndex) << ",\"layers\":" << uint32_t(i.layerCount)
          << ",\"aspects\":" << uint32_t(i.aspects) << '}';
    }
    void offsetJson(std::ostream& out, VkOffset3D p) { out << '[' << p.x << ',' << p.y << ',' << p.z << ']'; }
    void subJson(std::ostream& out, VkImageSubresourceLayers s) {
      out << '[' << s.mipLevel << ',' << s.baseArrayLayer << ',' << s.layerCount << ',' << s.aspectMask << ']';
    }
  }

  bool DxvkContext::takeQvfrFlowEvent() {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->profile || !m_qvfr->profileDetails || m_frameCount % 120) return false;
    auto& q = *m_qvfr;
    if (q.flowFrame != m_frameCount) {
      q.flowFrame = m_frameCount; q.flowSequence = 0; q.flowEvents = q.flowDropped = 0;
    }
    ++q.flowSequence;
    if (q.flowEvents == 128) { ++q.flowDropped; return false; }
    ++q.flowEvents;
    return true;
  }

  void DxvkContext::noteQvfrFlowDraw(const char* kind, uint32_t count, int64_t instances) {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->profile || !m_qvfr->profileDetails || m_frameCount % 120) return;
    auto view = m_state.om.renderTargets.color[0].view;
    // Investigate screen processing, not thousands of ordinary scene draws.
    if (!view || m_state.dyn.depthStencilState.depthTest()) return;
    auto extent = view->mipLevelExtent(0);
    if (extent.width < m_qvfr->size.width / 4 || extent.height < m_qvfr->size.height / 4) return;
    if (!takeQvfrFlowEvent()) return;
    DxvkQvfrCpuDetailScope metadata(m_qvfr.get(), m_frameCount, DxvkQvfrCpuStage::Metadata);
    auto& q = *m_qvfr;
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "QVFR flow {\"frame\":" << m_frameCount << ",\"seq\":" << q.flowSequence
        << ",\"kind\":\"draw\",\"draw_kind\":" << std::quoted(kind)
        << ",\"count\":" << count << ",\"instances\":" << instances << ",\"pass\":";
    if (q.profileFrame == m_frameCount && q.profileReplay != size_t(-1)) out << q.profileReplay;
    else out << "null";
    out << ",\"output\":"; viewJson(out, view.ptr());
    out << ",\"depth\":"; viewJson(out, m_state.om.renderTargets.depth.view.ptr());
    out << ",\"blend\":" << m_state.gp.state.omBlend[0].blendEnable()
        << ",\"write_mask\":" << uint32_t(m_state.gp.state.omBlend[0].colorWriteMask())
        << ",\"stencil\":" << m_state.dyn.depthStencilState.stencilTest()
        << ",\"viewport\":[";
    const auto& vp = m_state.vp.viewports[0];
    auto number = [&] (float v) { if (std::isfinite(v)) out << v; else out << "null"; };
    number(vp.x); out << ','; number(vp.y); out << ','; number(vp.width); out << ','; number(vp.height);
    const auto& sc = m_state.vp.scissorRects[0];
    out << "],\"scissor\":[" << sc.offset.x << ',' << sc.offset.y << ',' << sc.extent.width << ',' << sc.extent.height
        << "],\"shaders\":[";
    std::array<Rc<DxvkShader>, 5> shaders = {m_state.gp.shaders.vs, m_state.gp.shaders.tcs,
      m_state.gp.shaders.tes, m_state.gp.shaders.gs, m_state.gp.shaders.fs};
    for (size_t i = 0; i < shaders.size(); i++) {
      if (i) out << ',';
      if (shaders[i]) out << std::quoted(shaders[i]->debugName()); else out << "null";
    }
    out << "],\"inputs\":[";
    auto layout = m_state.gp.pipeline ? m_state.gp.pipeline->getLayout() : nullptr;
    uint32_t images = 0, dropped = 0;
    if (layout) for (uint32_t stage = 0; stage < 5; stage++) {
      if (!(layout->getNonemptyStageMask() & (1u << stage))) continue;
      auto range = layout->getReadOnlyResourcesForStage(VkShaderStageFlagBits(1u << stage));
      for (uint32_t i = 0; i < range.bindingCount; i++) {
        auto binding = range.bindings[i];
        if (binding.getDescriptorType() != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        auto input = m_resources[binding.getResourceIndex()].imageView;
        if (!input) continue;
        if (images == 16) { dropped++; continue; }
        if (images++) out << ',';
        out << "{\"stage\":" << stage << ",\"backend_slot\":" << binding.getResourceIndex() << ",\"view\":";
        viewJson(out, input.ptr()); out << '}';
      }
    }
    out << "],\"inputs_dropped\":" << dropped << ",\"samplers\":[";
    uint32_t samplers = 0;
    if (layout) {
      auto range = layout->getSamplers(getActivePipelineLayoutType(VK_PIPELINE_BIND_POINT_GRAPHICS));
      for (uint32_t i = 0; i < range.bindingCount && samplers < 16; i++) {
        auto slot = range.bindings[i].getResourceIndex(); auto sampler = m_samplers[slot];
        if (!sampler) continue;
        if (samplers++) out << ',';
        const auto& p = sampler->key().u.p;
        out << "{\"backend_slot\":" << slot << ",\"min\":" << p.minFilter << ",\"mag\":" << p.magFilter
            << ",\"mip\":" << p.mipMode << ",\"address\":[" << p.addressU << ',' << p.addressV << ',' << p.addressW
            << "],\"pixel_coordinates\":" << p.pixelCoord << '}';
      }
    }
    out << "]}"; Logger::info(out.str());
  }

  void DxvkContext::noteQvfrFlowImage(const char* kind, const DxvkImage& image, const DxvkImage* source) {
    if (!takeQvfrFlowEvent()) return;
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "QVFR flow {\"frame\":" << m_frameCount << ",\"seq\":" << m_qvfr->flowSequence
        << ",\"kind\":" << std::quoted(kind) << ",\"output\":"; imageJson(out, image);
    if (source) { out << ",\"source\":"; imageJson(out, *source); }
    out << '}'; Logger::info(out.str());
  }

  void DxvkContext::noteQvfrFlowTransfer(const char* kind, const DxvkImage& src, const DxvkImage& dst,
      const VkImageSubresourceLayers& srcSub, VkOffset3D src0, VkOffset3D src1,
      const VkImageSubresourceLayers& dstSub, VkOffset3D dst0, VkOffset3D dst1) {
    if (!m_qvfr || m_qvfrBusy || !m_qvfr->profile || !m_qvfr->profileDetails || m_frameCount % 120) return;
    if (src.info().extent.width < m_qvfr->size.width / 4 && dst.info().extent.width < m_qvfr->size.width / 4) return;
    if (!takeQvfrFlowEvent()) return;
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "QVFR flow {\"frame\":" << m_frameCount << ",\"seq\":" << m_qvfr->flowSequence
        << ",\"kind\":\"transfer-request\",\"operation\":" << std::quoted(kind) << ",\"source\":";
    imageJson(out, src); out << ",\"output\":"; imageJson(out, dst);
    out << ",\"source_sub\":"; subJson(out, srcSub); out << ",\"output_sub\":"; subJson(out, dstSub);
    out << ",\"source_rect\":["; offsetJson(out, src0); out << ','; offsetJson(out, src1);
    out << "],\"output_rect\":["; offsetJson(out, dst0); out << ','; offsetJson(out, dst1);
    out << "]}"; Logger::info(out.str());
  }
}
