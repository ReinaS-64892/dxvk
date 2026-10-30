#include "dxvk_gaze_source.h"
#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"
#include <cstdlib>

#ifdef _WIN32
#include "../util/util_win32_compat.h"
#endif

namespace dxvk
{
  static float getEnvFloat(const char *name, float fallback)
  {
    std::string v = env::getEnvVar(name);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
  }

  // -1 = unset, 0 / 1 = forced
  static int getEnvTri(const char *name)
  {
    std::string v = env::getEnvVar(name);
    if (v.empty())
      return -1;
    return v == "1" ? 1 : 0;
  }

  DxvkGazeSource::DxvkGazeSource()
  {
    m_debug = env::getEnvVar("DXVK_GAZE_DEBUG") == "1";
    m_flipY = env::getEnvVar("DXVK_GAZE_FLIP_Y") == "1";
    m_swapEyes = env::getEnvVar("DXVK_GAZE_SWAP_EYES") == "1";
    m_gainY = getEnvFloat("DXVK_GAZE_GAIN_Y", 1.0f);
    m_path = env::getEnvVar("DXVK_GAZE_FILE");
    if (m_path.empty()) m_path = "Z:\\dev\\shm\\vrchat_dfr_gaze";
    std::string fake = env::getEnvVar("DXVK_GAZE_FAKE");
    m_fake = fake == "x" ? 1 : fake == "y" ? 2 : fake == "circle" ? 3
        : fake == "fixed" ? 4 : fake == "grid" ? 5 : 0;
    m_fixedY = getEnvFloat("DXVK_GAZE_FIXED_Y", 0.25f);
    m_forceFlipX[0] = getEnvTri("DXVK_GAZE_FLIPX_L");
    m_forceFlipX[1] = getEnvTri("DXVK_GAZE_FLIPX_R");
    m_forceFlipY[0] = getEnvTri("DXVK_GAZE_FLIPY_L");
    m_forceFlipY[1] = getEnvTri("DXVK_GAZE_FLIPY_R");
  }

  DxvkGazeSource::~DxvkGazeSource()
  {
    closeSource();
  }

  void DxvkGazeSource::openSource()
  {
#ifdef _WIN32
    auto now = std::chrono::steady_clock::now();

    if (now < m_nextOpen)
      return;

    m_nextOpen = now + std::chrono::seconds(2);

    HANDLE file = CreateFileA(m_path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
      return;

    if (GetFileSize(file, nullptr) < sizeof(GazeShm))
    {
      CloseHandle(file);
      return;
    }

    HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);

    if (!map)
    {
      CloseHandle(file);
      return;
    }

    void *view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(GazeShm));

    if (!view)
    {
      CloseHandle(map);
      CloseHandle(file);
      return;
    }

    m_file = file;
    m_map = map;
    m_shm = static_cast<const GazeShm *>(view);
    m_sourceFreshness = gaze_bridge::Freshness{};

    Logger::info("GazeVRS: gaze source opened");
#endif
  }

  void DxvkGazeSource::closeSource()
  {
#ifdef _WIN32
    if (m_shm) UnmapViewOfFile(const_cast<GazeShm *>(m_shm));
    if (m_map) CloseHandle(HANDLE(m_map));
    if (m_file) CloseHandle(HANDLE(m_file));
#endif
    m_shm = nullptr;
    m_map = nullptr;
    m_file = nullptr;
  }

  bool DxvkGazeSource::readGaze(Gaze &g)
  {
    if (!m_shm)
      openSource();
    if (!m_shm)
      return false;

    const auto now = std::chrono::steady_clock::now();
    gaze_bridge::Sample sample;
    uint32_t sequence = 0;
    if (gaze_bridge::snapshot(m_shm, sample, sequence)
        && m_sourceFreshness.observe(sequence, now) && sample.valid)
    {
      for (uint32_t eye = 0; eye < 2; eye++)
      {
        g.cx[eye] = sample.u[eye];
        g.cy[eye] = sample.v[eye];
        g.flipX[eye] = 0;
        g.flipY[eye] = 0;
      }
      return true;
    }

    // Includes a writer killed during an odd sequence, and replaced files.
    // Never keep using the last valid packet after the heartbeat stops.
    if (m_sourceFreshness.expired(now))
      closeSource();
    return false;
  }

  bool DxvkGazeSource::generate(const GazeMapRequest &pass, GazeMap &result)
  {
    const auto now = std::chrono::steady_clock::now();
    const uint32_t width = pass.shape[0], height = pass.shape[1];
    const uint32_t texelWidth = pass.texel[0], texelHeight = pass.texel[1];
    const bool sbs = pass.shape[2] == 1u;
    const uint32_t tilesX = (width + texelWidth - 1u) / texelWidth;
    const uint32_t tilesY = (height + texelHeight - 1u) / texelHeight;
    Gaze g = {};

    if (m_fake)
    {
      float t = std::chrono::duration<float>(now.time_since_epoch()).count();
      float a = 0.35f * std::sin(t * 6.2831853f / 6.0f);
      float b = 0.35f * std::cos(t * 6.2831853f / 6.0f);
      float x = 0.5f, y = 0.5f;

      if (m_fake == 1)
        x += a;
      else if (m_fake == 2)
        y += a;
      else if (m_fake == 4)
        y = m_fixedY; // fixed spot, no motion
      else if (m_fake == 5)
      {
      } // grid: fixed center
      else
      {
        x += a;
        y += b;
      }

      for (uint32_t e = 0; e < 2u; e++)
      {
        g.cx[e] = x;
        g.cy[e] = y;
      }
    }
    else if (!readGaze(g))
    {
      if (m_debug && !m_loggedNoGaze)
      {
        m_loggedNoGaze = true;
        Logger::info("GazeVRS: candidate pass but no valid gaze data");
      }
      if (!m_lastValid) return false;
      m_lastValid = false;
      result.valid = false;
      result.request = pass;
      return true;
    }

    for (uint32_t e = 0; e < 2u; e++)
    {
      m_winMin[e] = std::min(m_winMin[e], g.cy[e]);
      m_winMax[e] = std::max(m_winMax[e], g.cy[e]);
      m_winMinX[e] = std::min(m_winMinX[e], g.cx[e]);
      m_winMaxX[e] = std::max(m_winMaxX[e], g.cx[e]);
    }

    if (m_gainY != 1.0f)
    {
      for (uint32_t e = 0; e < 2u; e++)
        g.cy[e] = std::clamp(0.5f + (g.cy[e] - 0.5f) * m_gainY, 0.0f, 1.0f);
    }

    if (m_swapEyes)
    {
      std::swap(g.cx[0], g.cx[1]);
      std::swap(g.cy[0], g.cy[1]);
      std::swap(g.flipX[0], g.flipX[1]);
      std::swap(g.flipY[0], g.flipY[1]);
    }

    // Fovea position per eye in framebuffer space (0..1, origin top-left of the eye image).
    // WiVRn reports display space and tells us whether the source image is mirrored.
    float fx[2], fy[2];
    uint32_t flags = 0u;

    for (uint32_t e = 0; e < 2u; e++)
    {
      bool flipX = m_forceFlipX[e] >= 0 ? m_forceFlipX[e] != 0 : g.flipX[e] != 0;
      bool flipY = m_forceFlipY[e] >= 0 ? m_forceFlipY[e] != 0 : g.flipY[e] != 0;

      if (m_flipY)
        flipY = !flipY;

      fx[e] = flipX ? 1.0f - g.cx[e] : g.cx[e];
      fy[e] = flipY ? 1.0f - g.cy[e] : g.cy[e];

      flags |= (flipX ? 1u : 0u) << (e * 2u) | (flipY ? 1u : 0u) << (e * 2u + 1u);
    }

    const float eyeW = sbs ? float(width) * 0.5f : float(width);

    const std::array<int32_t, 4> center = {
        int32_t(fx[0] * eyeW), int32_t(fy[0] * float(height)),
        int32_t(fx[1] * eyeW), int32_t(fy[1] * float(height))};

    // Quantize to tile granularity so we only re-upload when something visible changes
    const std::array<int32_t, 4> centerTiles = {
        center[0] / int32_t(texelWidth), center[1] / int32_t(texelHeight),
        center[2] / int32_t(texelWidth), center[3] / int32_t(texelHeight)};

    if (m_debug && now >= m_nextGazeLog)
    {
      m_nextGazeLog = now + std::chrono::seconds(1);
      Logger::info(str::format("GazeVRS: ", sbs ? "sbs " : "array ", width, "x", height,
                               " display L(", g.cx[0], ",", g.cy[0], ") R(", g.cx[1], ",", g.cy[1], ")",
                               " wivrn flipX=", g.flipX[0], g.flipX[1], " flipY=", g.flipY[0], g.flipY[1],
                               " -> fb L(", fx[0], ",", fy[0], ") R(", fx[1], ",", fy[1], ")",
                               " tiles ", tilesX, "x", tilesY));
      Logger::info(str::format("GazeVRS: raw range last 1s: cx L[", m_winMinX[0], ",", m_winMaxX[0],
                               "] R[", m_winMinX[1], ",", m_winMaxX[1], "]  cy L[", m_winMin[0], ",", m_winMax[0],
                               "] R[", m_winMin[1], ",", m_winMax[1], "]  gainY=", m_gainY));

      for (uint32_t e = 0; e < 2u; e++)
      {
        m_winMin[e] = m_winMinX[e] = 1.0f;
        m_winMax[e] = m_winMaxX[e] = 0.0f;
      }
    }

    GazeMapRequest request = pass;
    request.fx = {fx[0], fx[1]};
    request.fy = {fy[0], fy[1]};
    request.center = centerTiles;
    request.dotCenter = center;
    request.flags = flags;
    if (m_lastValid && request.sameMap(m_lastRequest)) return false;
    generateGazeMap(request, result);
    m_lastRequest = request;
    m_lastValid = true;
    return true;
  }
}
