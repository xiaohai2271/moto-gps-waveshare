#include "link_protocol.h"

namespace moto::sim::link {

void encode_header(std::uint16_t payload_size,
                   std::uint8_t out[kHeaderSize]) noexcept {
  out[0] = static_cast<std::uint8_t>((kMagic >> 24U) & 0xFFU);
  out[1] = static_cast<std::uint8_t>((kMagic >> 16U) & 0xFFU);
  out[2] = static_cast<std::uint8_t>((kMagic >> 8U) & 0xFFU);
  out[3] = static_cast<std::uint8_t>(kMagic & 0xFFU);
  out[4] = kVersion;
  out[5] = kKindBleFrame;
  out[6] = static_cast<std::uint8_t>((payload_size >> 8U) & 0xFFU);
  out[7] = static_cast<std::uint8_t>(payload_size & 0xFFU);
}

bool decode_header(const std::uint8_t in[kHeaderSize],
                   std::uint16_t* payload_size) noexcept {
  const std::uint32_t magic = (static_cast<std::uint32_t>(in[0]) << 24U) |
                              (static_cast<std::uint32_t>(in[1]) << 16U) |
                              (static_cast<std::uint32_t>(in[2]) << 8U) |
                              static_cast<std::uint32_t>(in[3]);
  if(magic != kMagic || in[4] != kVersion || in[5] != kKindBleFrame) {
    return false;
  }
  const std::uint16_t size =
      static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[6]) << 8U) |
                                 static_cast<std::uint16_t>(in[7]));
  if(size > kMaxPayload) {
    return false;
  }
  if(payload_size != nullptr) {
    *payload_size = size;
  }
  return true;
}

}  // namespace moto::sim::link
