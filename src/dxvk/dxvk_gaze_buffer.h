#pragma once

#include <array>
#include <cstdint>

namespace dxvk
{
  // Non-blocking ownership of a front attachment and a back upload. Values
  // come from GPU-completion signals, including every read of either slot.
  struct GazeVrsBufferState
  {
    int32_t active = -1;
    int32_t pending = -1;
    std::array<uint64_t, 2> lastUse = {};

    bool promote(const std::array<uint64_t, 2> &completed)
    {
      if (pending < 0 || completed[pending] < lastUse[pending])
        return false;
      active = pending;
      pending = -1;
      return true;
    }

    int32_t writable(const std::array<uint64_t, 2> &completed) const
    {
      const int32_t back = active == 0 ? 1 : 0;
      return pending < 0 && completed[back] >= lastUse[back] ? back : -1;
    }

    uint64_t upload(int32_t slot)
    {
      pending = slot;
      return ++lastUse[slot];
    }

    uint64_t read()
    {
      return ++lastUse[active];
    }
  };
}
