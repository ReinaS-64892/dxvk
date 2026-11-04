#include "dxvk_context.h"

#include "../util/util_env.h"
#include "../util/util_qvfr.h"
#include "../util/util_qvfr_json.h"

#include <atomic>
#include <locale>
#include <sstream>

namespace dxvk {
  namespace {
    struct TraceConfig {
      bool enabled = false;
      uint32_t limit = 4096;
      uint32_t skip = 0;
      std::optional<qvfr::Extent> size;

      TraceConfig() {
        if (env::getEnvVar("DXVK_QVFR_TRACE") != "1")
          return;
        auto sizeText = env::getEnvVar("DXVK_QVFR_TRACE_SIZE");
        auto limitText = env::getEnvVar("DXVK_QVFR_TRACE_LIMIT");
        auto skipText = env::getEnvVar("DXVK_QVFR_TRACE_SKIP");
        if (!sizeText.empty()) {
          size = qvfr::parseSize(sizeText);
          if (!size) {
            Logger::err("QVFR trace disabled: invalid DXVK_QVFR_TRACE_SIZE (expected WxH, 1..16384)");
            return;
          }
        }
        if (!limitText.empty()) {
          auto n = qvfr::parseUint(limitText, 1000000);
          if (!n || !*n) {
            Logger::err("QVFR trace disabled: invalid DXVK_QVFR_TRACE_LIMIT (expected 1..1000000)");
            return;
          }
          limit = *n;
        }
        if (!skipText.empty()) {
          auto n = qvfr::parseUint(skipText, 1000000000);
          if (!n) {
            Logger::err("QVFR trace disabled: invalid DXVK_QVFR_TRACE_SKIP (expected 0..1000000000)");
            return;
          }
          skip = *n;
        }
        enabled = true;
        Logger::info("QVFR trace enabled: diagnostic only; draw records do not identify stereo automatically");
      }
    };

    const TraceConfig& config() {
      static const TraceConfig value;
      return value;
    }

    void jsonString(std::ostream& out, const std::string& value) {
      constexpr char hex[] = "0123456789abcdef";
      out << '"';
      for (unsigned char c : value) {
        if (c == '"' || c == '\\')
          out << '\\' << char(c);
        else if (c < 32)
          out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else
          out << char(c);
      }
      out << '"';
    }

    void jsonFloat(std::ostream& out, float value) {
      if (std::isfinite(value)) out << value;
      else out << "null";
    }

    void attachment(std::ostream& out, const Rc<DxvkImageView>& view) {
      if (!view) {
        out << "null";
        return;
      }
      const auto& info = view->info();
      auto extent = view->mipLevelExtent(0);
      qvfr::writeAttachmentJson(out, {
        uint64_t(view->image()->getResourceId()), uint64_t(view->image()->handle()),
        uint32_t(info.format), extent.width, extent.height, info.mipIndex,
        info.layerIndex, info.layerCount, uint32_t(view->image()->info().sampleCount) });
    }
  }

  void DxvkContext::initQvfrTrace() {
    m_qvfrTraceEnabled = config().enabled;
  }

  void DxvkContext::traceQvfrDraw(const char* kind, uint32_t count,
      int64_t instances, uint32_t firstInstance) {
    const auto& cfg = config();
    const auto& fb = m_state.om.framebufferInfo;
    auto size = fb.size();
    if (cfg.size && (cfg.size->width != size.width || cfg.size->height != size.height))
      return;

    // Shared across contexts. This also bounds deferred/replayed command lists.
    static std::atomic<uint64_t> sequence { 0 };
    uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    if (seq < cfg.skip)
      return;
    if (seq - cfg.skip >= cfg.limit) {
      m_qvfrTraceEnabled = false;
      return;
    }

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(9);
    out << "QVFR {\"version\":1,\"seq\":" << seq
        << ",\"context\":" << uintptr_t(this) << ",\"pass\":" << m_qvfrPass
        << ",\"frame\":" << m_frameCount << ",\"kind\":\"" << kind
        << "\",\"count\":" << count << ",\"instances\":";
    if (instances < 0) out << "null";
    else out << instances;
    out << ",\"first_instance\":";
    if (instances < 0) out << "null";
    else out << firstInstance;
    out << ",\"width\":" << size.width << ",\"height\":" << size.height
        << ",\"layers\":" << size.layers
        << ",\"storage\":" << (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasStorageDescriptors) ? "true" : "false")
        << ",\"xfb\":" << (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback) ? "true" : "false")
        << ",\"layer_export\":" << (m_state.gp.flags.test(DxvkGraphicsPipelineFlag::HasLayerExport) ? "true" : "false")
        << ",\"colors\":[";
    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      if (i) out << ',';
      attachment(out, fb.getColorTarget(i).view);
    }
    out << "],\"depth\":";
    attachment(out, fb.getDepthTarget().view);
    out << ",\"viewports\":[";
    for (uint32_t i = 0; i < m_state.vp.viewportCount; i++) {
      if (i) out << ',';
      auto vp = m_state.vp.viewports[i];
      auto sc = m_state.vp.scissorRects[i];
      out << "{\"rect\":[";
      jsonFloat(out, vp.x); out << ',';
      jsonFloat(out, vp.y); out << ',';
      jsonFloat(out, vp.width); out << ',';
      jsonFloat(out, vp.height);
      out << "],\"depth\":[";
      jsonFloat(out, vp.minDepth); out << ',';
      jsonFloat(out, vp.maxDepth);
      out << "],\"scissor\":[" << sc.offset.x << ',' << sc.offset.y << ','
          << sc.extent.width << ',' << sc.extent.height << "]}";
    }
    out << "],\"shaders\":[";
    const auto& s = m_state.gp.shaders;
    std::array<Rc<DxvkShader>, 5> shaders { s.vs, s.tcs, s.tes, s.gs, s.fs };
    for (uint32_t i = 0; i < shaders.size(); i++) {
      if (i) out << ',';
      if (shaders[i]) jsonString(out, shaders[i]->debugName());
      else out << "null";
    }
    out << "]}";
    Logger::info(out.str());
  }
}
