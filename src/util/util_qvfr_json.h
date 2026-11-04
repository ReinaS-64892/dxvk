#pragma once

#include <cstdint>
#include <ostream>

namespace dxvk::qvfr {

  // Convert API enums and packed uint8_t fields to numeric types before
  // streaming: enum overloads can print names, and uint8_t prints a character.
  // A NUL in a log line also hides its newline from Wine's C-string logger.
  struct TraceAttachment {
    uint64_t image, vkImage;
    uint32_t format, width, height, mip, layer, layers, samples;
  };

  inline void writeAttachmentJson(std::ostream& out, const TraceAttachment& a) {
    out << "{\"image\":" << a.image << ",\"vk_image\":" << a.vkImage
        << ",\"format\":" << a.format
        << ",\"width\":" << a.width << ",\"height\":" << a.height
        << ",\"mip\":" << a.mip << ",\"layer\":" << a.layer
        << ",\"layers\":" << a.layers << ",\"samples\":" << a.samples << '}';
  }

}
