#pragma once

#include "dxvk_gaze_map.h"
#include "dxvk_gaze_shm.h"
#include <chrono>
#include <string>

namespace dxvk
{
  // Owned by the map worker. Mapping, heartbeat checks, gaze transforms,
  // fake gaze and map generation never touch the rendering thread.
  class DxvkGazeSource
  {
  public:
    DxvkGazeSource();
    ~DxvkGazeSource();
    DxvkGazeSource(const DxvkGazeSource &) = delete;
    DxvkGazeSource &operator=(const DxvkGazeSource &) = delete;
    bool generate(const GazeMapRequest &pass, GazeMap &result);

  private:
    using GazeShm = gaze_bridge::SharedGaze;
    struct Gaze
    {
      float cx[2] = {}, cy[2] = {};
      int32_t flipX[2] = {}, flipY[2] = {};
    };
    void *m_file = nullptr, *m_map = nullptr;
    const GazeShm *m_shm = nullptr;
    std::chrono::steady_clock::time_point m_nextOpen = {};
    gaze_bridge::Freshness m_sourceFreshness;
    bool m_debug = false, m_flipY = false, m_swapEyes = false;
    int m_forceFlipX[2] = {-1, -1}, m_forceFlipY[2] = {-1, -1};
    float m_gainY = 1.0f, m_fixedY = 0.25f;
    int m_fake = 0;
    std::string m_path;
    bool m_loggedNoGaze = false, m_lastValid = false;
    GazeMapRequest m_lastRequest;
    float m_winMin[2] = {1.0f, 1.0f}, m_winMax[2] = {0.0f, 0.0f};
    float m_winMinX[2] = {1.0f, 1.0f}, m_winMaxX[2] = {0.0f, 0.0f};
    std::chrono::steady_clock::time_point m_nextGazeLog = {};
    void openSource();
    void closeSource();
    bool readGaze(Gaze &g);
  };
}
