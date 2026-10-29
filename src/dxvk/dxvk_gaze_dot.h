#pragma once

#include <cstdint>

namespace dxvk
{

  struct GazeDotRect
  {
    int32_t x = 0, y = 0, r = 24; // center and half-size, framebuffer pixels
    float rgb[3] = {1.0f, 0.0f, 1.0f};
  };

  // Debug overlay drawn at the end of eye passes with vkCmdClearAttachments.
  struct GazeDotState
  {
    bool passActive = false;
    int32_t width = 0;
    int32_t height = 0;

    int32_t x[2] = {0, 0};
    int32_t y[2] = {0, 0};
    int32_t radius = 24;

    uint32_t count = 0;
    GazeDotRect dots[16];
  };

  inline GazeDotState g_gazeDot;

}