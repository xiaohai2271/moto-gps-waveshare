#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "moto/ble_protocol/ble_protocol.hpp"

namespace moto::sim {

// Platform-neutral stand-in for the NimBLE transport used by the firmware.
//
// It reproduces the transport-level contract that PhoneNavBridge relies on:
//   * the three-step v1 handshake (Phone Starting -> Device Ready ->
//     Phone Ready -> Device Ready),
//   * per-message fragmentation at the negotiated frame size,
//   * application ACKs and command ACK retries,
//   * device heartbeats and the phone liveness watchdog,
//   * link-active transitions separate from the physical connection.
//
// Application messages are handed to the real PhoneNavBridge callback, so the
// virtual device exercises the same code as the ESP32 build.
class VirtualBleTransport {
 public:
  using SendFrame = std::function<bool(const moto::ble::Bytes&)>;
  using MessageCallback = moto::ble::AckStatus (*)(
      const moto::ble::ReassembledMessage& message, void* context);
  using LinkCallback = void (*)(bool active, void* context);

  // 185-byte ATT MTU minus the 3-byte notification header, matching the
  // firmware and the measured phone link.
  static constexpr std::uint16_t kAdvertisedMtu = 185;
  static constexpr std::size_t kLocalFrameSize = 182;

  void set_callbacks(MessageCallback message, LinkCallback link,
                     void* context) noexcept;
  void set_send(SendFrame send) { send_ = std::move(send); }

  void on_peer_connected();
  void on_peer_disconnected();
  void on_frame(const moto::ble::Bytes& frame, std::uint64_t now_ms);
  void tick(std::uint64_t now_ms);

  [[nodiscard]] bool send_message(const moto::ble::Message& message,
                                  std::uint8_t frame_flags = 0);
  static bool send_from_bridge(const moto::ble::Message& message,
                               std::uint8_t frame_flags, void* context);

  [[nodiscard]] bool protocol_ready() const { return protocol_ready_; }
  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }
  [[nodiscard]] bool link_active() const { return link_active_; }
  [[nodiscard]] bool peer_connected() const { return connected_; }

 private:
  struct PendingAck {
    bool active = false;
    std::uint16_t sequence = 0;
    std::uint16_t command_id = 0;
    std::uint8_t retries = 0;
    std::uint64_t deadline_ms = 0;
    std::vector<moto::ble::Bytes> frames;
  };

  [[nodiscard]] std::size_t negotiated_frame_size() const noexcept;
  bool send_frames(const std::vector<moto::ble::Bytes>& frames);
  void send_ack(std::uint16_t sequence, moto::ble::AckStatus status,
                std::uint16_t command_id = 0);
  void accept_ack(const moto::ble::Ack& ack);
  void service_pending_ack(std::uint64_t now_ms);
  void send_connection_status(moto::ble::ConnectionState state);
  void note_link(bool active);

  MessageCallback message_ = nullptr;
  LinkCallback link_ = nullptr;
  void* context_ = nullptr;
  SendFrame send_;

  bool connected_ = false;
  bool protocol_ready_ = false;
  bool link_active_ = false;
  bool has_last_rx_ack_ = false;
  std::uint32_t session_id_ = 0;
  std::uint16_t peer_frame_size_ = 20;
  std::uint16_t last_rx_ack_sequence_ = 0;
  std::uint16_t last_rx_ack_command_id_ = 0;
  moto::ble::AckStatus last_rx_ack_status_ = moto::ble::AckStatus::Failed;
  std::uint64_t last_tx_ms_ = 0;

  moto::ble::Reassembler reassembler_{};
  moto::ble::SequenceGenerator tx_sequence_{};
  moto::ble::LinkWatchdog watchdog_{3'500};
  PendingAck pending_ack_{};
};

}  // namespace moto::sim
