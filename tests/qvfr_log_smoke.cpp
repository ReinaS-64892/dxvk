// Windows/Wine smoke test using the production serializer AND DXVK Logger.
// Optional argument: replay only QVFR records from a binary DXVK log, to
// reproduce the original NUL/newline bug without starting a game or Vulkan.
#include "../src/util/log/log.h"
#include "../src/util/util_qvfr_json.h"

#include <sstream>

namespace dxvk {
  Logger Logger::s_instance("qvfr-smoke.log");
}

int main(int argc, char** argv) {
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 2;
    std::string line;
    while (std::getline(file, line)) {
      auto marker = line.find("QVFR {");
      if (marker != std::string::npos)
        dxvk::Logger::info(line.substr(marker));
    }
  } else {
    for (uint32_t draw = 0; draw < 256; draw++) {
      std::ostringstream out;
      out << "QVFR {\"seq\":" << draw << ",\"colors\":[";
      // Alternate short and >1018 byte records to exercise Wine log splitting.
      uint32_t attachments = draw % 2 ? 8 : 1;
      for (uint32_t i = 0; i < attachments; i++) {
        if (i) out << ',';
        uint8_t mip = draw % 16;
        dxvk::qvfr::writeAttachmentJson(out,
          {26778077, 93825511318432ull, 43, 5184, 2592, mip, 0, 1, 4});
      }
      out << "]}";
      dxvk::Logger::info(out.str());
    }
  }
  return 0;
}
