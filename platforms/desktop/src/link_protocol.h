#pragma once

#include <cstddef>
#include <cstdint>

namespace moto::sim::link {

// The virtual BLE link carries one complete encoded BLE frame per message.
// A real GATT write/notification also delivers exactly one frame, so the
// Android transport and this simulator only need an explicit length prefix.
//
//   offset  size  field
//   0       4     magic 0x4D4F544F ("MOTO"), big endian
//   4       1     version (1)
//   5       1     kind (1 = encoded BLE frame)
//   6       2     payload length, big endian
//   8       N     payload
constexpr std::uint32_t kMagic = 0x4D4F544FU;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kKindBleFrame = 1;
constexpr std::size_t kHeaderSize = 8;
// Keep in step with ble_protocol::kMaxBleAttributeValueSize.
constexpr std::size_t kMaxPayload = 512;

void encode_header(std::uint16_t payload_size,
                   std::uint8_t out[kHeaderSize]) noexcept;

[[nodiscard]] bool decode_header(const std::uint8_t in[kHeaderSize],
                                 std::uint16_t* payload_size) noexcept;

}  // namespace moto::sim::link
