#pragma once

#include "dxvk_gaze_buffer.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

#include "dxvk_include.h"

namespace dxvk
{

  class DxvkDevice;
  class DxvkCommandList;
  struct GazeMap;

  bool dxvkGazeVrsEnabled(const DxvkDevice *device);

  class DxvkGazeVrs
  {

  public:
    explicit DxvkGazeVrs(DxvkDevice *device);
    ~DxvkGazeVrs();

    bool prepare(
        DxvkCommandList *cmd,
        VkExtent2D extent,
        uint32_t layers,
        VkFormat colorFormat,
        VkSampleCountFlagBits samples,
        VkRenderingFragmentShadingRateAttachmentInfoKHR &info);

  private:
    static constexpr uint32_t MaxLayers = 2u;

    DxvkDevice *m_device = nullptr;

    // Each slot owns both the attachment and its upload buffer. Completion is
    // polled; an in-flight slot is never overwritten or waited on.
    struct Slot : public RcObject
    {
      explicit Slot(DxvkDevice *device) : device(device) { }
      ~Slot();
      DxvkDevice *device;
      VkImage image = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
      VkDeviceMemory imageMem = VK_NULL_HANDLE;
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory bufferMem = VK_NULL_HANDLE;
      Rc<sync::Fence> completion = new sync::Fence();
      bool layoutInit = false;
      std::array<uint32_t, 5> shape = {};
      uint32_t flags = ~0u;
      uint32_t coarse = ~0u;
      std::array<int32_t, 4> center = {-1, -1, -1, -1};
      std::array<int32_t, 4> dotCenter = {};
    };
    std::array<Rc<Slot>, 2> m_slots;
    GazeVrsBufferState m_buffers;
    bool m_ok = false;
    struct Worker;
    std::unique_ptr<Worker> m_mapWorker;
    std::unique_ptr<GazeMap> m_cpuMap;
    bool m_loggedWorkerFailure = false;

    VkExtent2D m_texel = {16u, 16u};
    VkExtent2D m_maxTiles = {512u, 512u};

    // Configuration
    float m_r1 = 0.20f;
    float m_r2 = 0.40f;
    float m_fullArea = -1.0f;
    float m_midArea = -1.0f;
    uint32_t m_coarse = 5u; // raw rate code of the outer zone, 5 = 2x2, 10 = 4x4
    bool m_autoCoarse = false;
    uint32_t m_autoCoarseRate = 0u;
    VkSampleCountFlagBits m_autoCoarseSamples = VkSampleCountFlagBits(0);
    uint32_t m_minSize = 700u;
    bool m_debug = false;
    bool m_dot = false;
    bool m_grid = false;
    int32_t m_dotRadius = 24;
    uint32_t m_onlySamples = 0u; // 0 = any; else only passes with this color sample count
    uint32_t m_onlyFormat = 0u;  // 0 = any; else only passes with this VkFormat value
    uint32_t m_forceW = 0u;
    uint32_t m_forceH = 0u;

    std::map<std::array<uint32_t, 5>, uint32_t> m_shapes; // (w, h, layers, fmt, samples) -> pass count
    std::chrono::steady_clock::time_point m_nextSummary = {};

    bool createResources();
  };

}
