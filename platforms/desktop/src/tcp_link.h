#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "moto/ble_protocol/ble_protocol.hpp"

namespace moto::sim {

// Loopback TCP transport used by the host virtual device and its scripted
// phone. The wire format is documented in link_protocol.h: one length-prefixed
// encoded BLE frame per message.
class TcpServer {
 public:
  TcpServer() = default;
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  bool listen(std::uint16_t port, std::string* error);

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] bool connected() const;
  bool accept_pending();
  void drop_peer();

  [[nodiscard]] bool send_frame(const moto::ble::Bytes& frame);
  // Reads every complete frame currently available. Returns false if the peer
  // was dropped because of an I/O or framing error.
  bool read_frames(std::vector<moto::ble::Bytes>* frames);

 private:
  std::intptr_t listener_ = -1;
  std::intptr_t peer_ = -1;
  std::uint16_t port_ = 0;
  std::vector<std::uint8_t> rx_buffer_;
};

// Blocking client with a receive timeout, used by the self-test script and by
// anyone who wants a minimal command-line phone simulator.
class TcpClient {
 public:
  TcpClient() = default;
  ~TcpClient();
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  bool connect(const std::string& host, std::uint16_t port,
               std::string* error);
  void close();
  [[nodiscard]] bool connected() const;

  [[nodiscard]] bool send_frame(const moto::ble::Bytes& frame);
  // 1 = frame, 0 = timeout, -1 = closed or protocol error.
  int read_frame(moto::ble::Bytes* frame, int timeout_ms);

 private:
  std::intptr_t socket_ = -1;
};

}  // namespace moto::sim
