#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "moto/ble_protocol/ble_protocol.hpp"
#include "tcp_link.h"

namespace moto::sim {

// Deterministic 8-component layout used by the layout and fragmentation cases.
[[nodiscard]] moto::ble::UiLayout make_test_layout(std::uint16_t revision,
                                                   std::uint16_t map_height,
                                                   std::uint8_t components);

// Scripted phone used by every simulator case. It speaks the real v1 protocol
// through the same link framing as the Android debug transport.
class NavPhoneClient {
 public:
  explicit NavPhoneClient(std::uint16_t port,
                          std::uint32_t session_id = 0x51A7U);
  ~NavPhoneClient();
  NavPhoneClient(const NavPhoneClient&) = delete;
  NavPhoneClient& operator=(const NavPhoneClient&) = delete;

  bool begin();
  bool handshake();
  bool restart(std::uint32_t session_id);
  void drop_link();

  bool send_message(const moto::ble::Message& message, std::uint8_t flags);
  bool send_raw(moto::ble::MessageType type, moto::ble::ByteView payload,
                std::uint8_t flags);
  bool send_status(moto::ble::ConnectionState state);
  bool send_snapshot(const moto::ble::NavigationSnapshot& snapshot, bool ack);
  bool send_geometry_chunks(const std::vector<moto::ble::GeoPointE6>& points,
                            std::uint32_t route_token,
                            std::uint32_t route_generation,
                            std::uint16_t chunk_count);
  bool send_layout_variant(std::uint16_t revision, std::uint16_t map_height,
                           std::uint8_t components,
                           moto::ble::AckStatus* status);
  bool await_layout_state(std::uint16_t revision,
                          const moto::ble::NavigationSnapshot& snapshot,
                          int timeout_ms);
  bool send_map_scene(std::uint32_t revision, int roads,
                      int points_per_road, int buildings,
                      int points_per_building,
                      moto::ble::AckStatus* status = nullptr);
  bool send_max_map_scene(std::uint32_t revision);
  bool send_media();
  // Lets the liveness case stop heartbeats without closing the link.
  void set_heartbeat_enabled(bool enabled) { heartbeat_enabled_ = enabled; }
  bool send_wifi_config(const std::string& ssid, std::uint8_t action, bool ack);
  bool wait_wifi_state(std::uint8_t state, std::uint8_t* error,
                       int timeout_ms);
  bool wait_ack(std::uint16_t sequence, int timeout_ms);
  bool wait_ack_status(std::uint16_t sequence, moto::ble::AckStatus* status,
                       int timeout_ms);
  bool expect(moto::ble::MessageType type, moto::ble::Message* message,
              int timeout_ms);
  bool inbox_has(moto::ble::MessageType type) const;

  bool fail(const std::string& message);
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] std::uint16_t last_sequence() const { return last_sequence_; }
  [[nodiscard]] std::uint32_t session_id() const { return session_id_; }

 private:
  bool connect();
  void maybe_heartbeat();
  bool next_message(moto::ble::Message* message, int timeout_ms);

  std::uint16_t port_ = 0;
  std::uint32_t session_id_ = 0;
  TcpClient client_;
  moto::ble::Reassembler inbound_{};
  moto::ble::SequenceGenerator sequence_{1};
  std::uint64_t connected_at_ms_ = 0;
  std::uint64_t last_heartbeat_ms_ = 0;
  std::uint16_t last_sequence_ = 0;
  bool session_established_ = false;
  bool heartbeat_enabled_ = true;
  std::vector<moto::ble::Message> inbox_;
  std::string step_;
  std::string error_;
};

}  // namespace moto::sim
