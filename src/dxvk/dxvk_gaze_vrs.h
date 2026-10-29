#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dxvk_include.h"

namespace dxvk
{

  class DxvkDevice;
  class DxvkCommandList;

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
    // Must match gaze_shm_data in WiVRn's foveation.cpp
    struct GazeShm
    {
      uint32_t magic; // 'GAZE'
      uint32_t seq;   // even = stable
      float cx[2];    // fovea center, fraction of eye width,  display space
      float cy[2];    // fovea center, fraction of eye height, display space, from top
      uint32_t valid;
      int32_t flipX[2]; // source rect has negative width
      int32_t flipY[2]; // source rect has negative height
    };

    struct Gaze
    {
      float cx[2];
      float cy[2];
      int32_t flipX[2];
      int32_t flipY[2];
    };

    static constexpr uint32_t MaxTiles = 512u;
    static constexpr uint32_t MaxLayers = 2u;

    DxvkDevice *m_device = nullptr;

    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMem = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_bufferMem = VK_NULL_HANDLE;
    bool m_ok = false;
    bool m_layoutInit = false;

    VkExtent2D m_texel = {16u, 16u};

    void *m_file = nullptr;
    void *m_map = nullptr;
    const volatile GazeShm *m_shm = nullptr;
    std::chrono::steady_clock::time_point m_nextOpen = {};

    uint32_t m_lastTilesX = 0u;
    uint32_t m_lastTilesY = 0u;
    uint32_t m_lastFlags = ~0u;
    std::array<int32_t, 4> m_lastCenter = {-1, -1, -1, -1};

    // Configuration
    float m_r1 = 0.20f;
    float m_r2 = 0.40f;
    float m_fullArea = -1.0f;
    float m_midArea = -1.0f;
    uint32_t m_coarse = 5u; // raw rate code of the outer zone, 5 = 2x2, 10 = 4x4
    bool m_autoCoarse = false;
    uint32_t m_autoCoarseRate = 0u;
    VkSampleCountFlagBits m_autoCoarseSamples = VkSampleCountFlagBits(0);
    uint32_t m_lastCoarse = ~0u;
    uint32_t m_minSize = 700u;
    bool m_flipY = false; // extra global Y flip
    bool m_swapEyes = false;
    int m_forceFlipX[2] = {-1, -1}; // -1 = use WiVRn's value
    int m_forceFlipY[2] = {-1, -1};
    bool m_debug = false;
    bool m_dot = false;
    int32_t m_dotRadius = 24;
    bool m_loggedNoGaze = false;
    float m_gainY = 1.0f;             // multiplies vertical gaze offset from center
    float m_winMin[2] = {1.0f, 1.0f}; // cx/cy range seen since last log line
    float m_winMax[2] = {0.0f, 0.0f};
    float m_winMinX[2] = {1.0f, 1.0f};
    float m_winMaxX[2] = {0.0f, 0.0f};
    uint32_t m_onlySamples = 0u; // 0 = any; else only passes with this color sample count
    uint32_t m_onlyFormat = 0u;  // 0 = any; else only passes with this VkFormat value
    float m_fixedY = 0.25f;      // y used by DXVK_GAZE_FAKE=fixed
    int m_fake = 0;              // 0 = real gaze, 1 = sweep x, 2 = sweep y, 3 = circle
    uint32_t m_forceW = 0u;
    uint32_t m_forceH = 0u;
    std::string m_path;

    std::map<std::array<uint32_t, 5>, uint32_t> m_shapes; // (w, h, layers, fmt, samples) -> pass count
    std::chrono::steady_clock::time_point m_nextSummary = {};
    std::chrono::steady_clock::time_point m_nextGazeLog = {};

    bool createResources();
    void openSource();
    bool readGaze(Gaze &g);
  };

}