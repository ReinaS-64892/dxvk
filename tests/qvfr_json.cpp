#include "../src/util/util_qvfr_json.h"

#include <iostream>

// Exercise the real serializer with both problematic input types. In DXVK,
// VkFormat has an ostream overload that prints a symbolic name rather than JSON.
enum Format { Srgb = 43 };
std::ostream& operator << (std::ostream& out, Format) {
  return out << "VK_FORMAT_R8G8B8A8_SRGB";
}

int main() {
  for (uint8_t mip : {0, 1, 255}) {
    dxvk::qvfr::writeAttachmentJson(std::cout,
      {26778077, 93825511318432ull, Srgb, 5184, 2592, mip, 0, 1, 4});
    std::cout << '\n';
  }
}
