#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dxvk
{
  struct GazeMapRequest
  {
    // Width, height, layers, format, sample count.
    std::array<uint32_t, 5> shape = {};
    std::array<uint32_t, 2> texel = {16u, 16u};
    std::array<float, 2> fx = {}, fy = {};
    std::array<int32_t, 4> center = {}, dotCenter = {};
    uint32_t flags = 0u, coarse = 5u;
    float r1 = 0.20f, r2 = 0.40f;
    float fullArea = -1.0f, midArea = -1.0f;

    bool sameMap(const GazeMapRequest &other) const
    {
      // Preserve the existing tile-granularity gaze update cache.
      return shape == other.shape && texel == other.texel
          && center == other.center && flags == other.flags && coarse == other.coarse
          && r1 == other.r1 && r2 == other.r2
          && fullArea == other.fullArea && midArea == other.midArea;
    }
  };

  struct GazeMap
  {
    GazeMapRequest request;
    bool valid = true;
    std::vector<uint8_t> data;
  };

  inline void generateGazeMap(const GazeMapRequest &request, GazeMap &result)
  {
    result.request = request;
    result.valid = true;
    const uint32_t width = request.shape[0], height = request.shape[1];
    const uint32_t layers = request.shape[2];
    const bool sbs = layers == 1u;
    const uint32_t tilesX = (width + request.texel[0] - 1u) / request.texel[0];
    const uint32_t tilesY = (height + request.texel[1] - 1u) / request.texel[1];
    const uint32_t layerStride = (tilesX * tilesY + 3u) & ~3u;
    result.data.resize(size_t(layerStride) * layers);
    const float eyeW = sbs ? float(width) * 0.5f : float(width);
    const float minDim = std::min(eyeW, float(height));
    auto radius = [&](float area, float fallback) {
      if (area < 0.0f)
        return fallback;
      const float a = std::clamp(area, 0.0f, 100.0f) * 0.01f;
      return a >= 1.0f ? 10.0f : std::sqrt(a * eyeW * float(height) / 3.14159265f) / minDim;
    };
    const float r1 = radius(request.fullArea, request.r1);
    const float r2 = radius(request.midArea, request.r2);
    const uint32_t mid = std::min(5u, request.coarse);
    for (uint32_t l = 0u; l < layers; l++)
    {
      for (uint32_t ty = 0u; ty < tilesY; ty++)
      {
        for (uint32_t tx = 0u; tx < tilesX; tx++)
        {
          const float px = (float(tx) + 0.5f) * float(request.texel[0]);
          const float py = (float(ty) + 0.5f) * float(request.texel[1]);
          const uint32_t eye = sbs ? (px < eyeW ? 0u : 1u) : l;
          const float lx = sbs ? px - float(eye) * eyeW : px;
          const float d = std::hypot(lx - request.fx[eye] * eyeW,
              py - request.fy[eye] * float(height)) / minDim;
          result.data[size_t(l) * layerStride + size_t(ty) * tilesX + tx]
              = d >= r1 ? uint8_t(d >= r2 ? request.coarse : mid) : 0u;
        }
      }
      std::fill(result.data.begin() + size_t(l) * layerStride + tilesX * tilesY,
          result.data.begin() + size_t(l + 1u) * layerStride, 0u);
    }
  }

  struct GazeMapGenerator
  {
    bool operator()(const GazeMapRequest &request, GazeMap &result) const
    {
      generateGazeMap(request, result);
      return true;
    }
  };

  // Thread primitives are supplied by DXVK in production and by the native
  // test harness in tests. Only this worker allocates/fills CPU tile maps.
  template<typename Thread = std::thread, typename Mutex = std::mutex,
           typename Condition = std::condition_variable, typename Generator = GazeMapGenerator>
  class GazeMapWorker
  {
  public:
    explicit GazeMapWorker(std::function<void()> init = {}, Generator generate = {},
        std::chrono::milliseconds poll = std::chrono::milliseconds(0))
    : m_poll(poll), m_generate(std::move(generate)), m_thread([this, init = std::move(init)] {
        try {
          if (init) init();
          run();
        } catch (...) {
          m_failed.store(true, std::memory_order_release);
        }
      }) { }

    ~GazeMapWorker()
    {
      {
        std::lock_guard<Mutex> lock(m_mutex);
        m_stopped = true;
      }
      m_condition.notify_one();
      if (m_thread.joinable()) m_thread.join();
    }

    bool failed() const { return m_failed.load(std::memory_order_acquire); }

    // Never wait on the render thread, even for the mailbox mutex. Requests
    // replace the queued request; a running job is allowed to finish so that
    // continuous gaze motion cannot starve publication of completed maps.
    bool exchange(const GazeMapRequest &request, bool needed, std::unique_ptr<GazeMap> &result)
    {
      std::unique_lock<Mutex> lock(m_mutex, std::try_to_lock);
      if (!lock.owns_lock()) return false;
      const bool ready = m_ready;
      if (ready)
      {
        result.swap(m_result);
        m_ready = false;
      }
      if (needed && (!m_requested || !request.sameMap(m_request)))
      {
        m_request = request;
        m_requested = true;
        m_hasJob = true;
        m_condition.notify_one();
      }
      return ready;
    }

  private:
    Mutex m_mutex;
    Condition m_condition;
    GazeMapRequest m_request;
    bool m_requested = false, m_hasJob = false, m_stopped = false, m_ready = false;
    std::unique_ptr<GazeMap> m_result;
    std::atomic<bool> m_failed = {false};
    std::chrono::milliseconds m_poll;
    Generator m_generate;
    Thread m_thread;

    void run()
    {
      std::unique_ptr<GazeMap> building;
      for (;;)
      {
        GazeMapRequest request;
        {
          std::unique_lock<Mutex> lock(m_mutex);
          auto wake = [this] { return m_stopped || m_hasJob; };
          if (m_requested && m_poll.count())
            m_condition.wait_for(lock, m_poll, wake);
          else
            m_condition.wait(lock, wake);
          if (m_stopped) return;
          request = m_request;
          m_hasJob = false;
          // Recycle the previous CPU map returned by the render thread.
          if (!building && !m_ready) building.swap(m_result);
        }
        if (!building) building = std::make_unique<GazeMap>();
        if (!m_generate(request, *building)) continue;
        {
          std::lock_guard<Mutex> lock(m_mutex);
          // Keep the last completed result even when newer gaze is queued.
          building.swap(m_result);
          m_ready = true;
        }
      }
    }
  };
}
