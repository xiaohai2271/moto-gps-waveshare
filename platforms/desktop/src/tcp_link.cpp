#include "tcp_link.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "link_protocol.h"
#include "sim_log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace moto::sim {
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

void close_socket(socket_t socket) { closesocket(socket); }
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool interrupted() { return WSAGetLastError() == WSAEINTR; }

bool ensure_sockets(std::string* error) {
  static const int result = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data);
  }();
  if(result != 0) {
    if(error != nullptr) {
      *error = "WSAStartup failed with code " + std::to_string(result);
    }
    return false;
  }
  return true;
}

void ignore_sigpipe() {}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

void close_socket(socket_t socket) { ::close(socket); }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool interrupted() { return errno == EINTR; }

bool ensure_sockets(std::string*) { return true; }

void ignore_sigpipe() {
  static const bool once = [] {
    std::signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)once;
}
#endif

socket_t from_handle(std::intptr_t handle) {
  return static_cast<socket_t>(handle);
}

std::intptr_t to_handle(socket_t socket) {
  return static_cast<std::intptr_t>(socket);
}

// 1 = readable, 0 = timeout, -1 = error.
int wait_readable(socket_t socket, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
  const int result = select(0, &set, nullptr, nullptr, &timeout);
#else
  const int result = select(socket + 1, &set, nullptr, nullptr, &timeout);
#endif
  if(result < 0) {
    return interrupted() ? 0 : -1;
  }
  return result == 0 ? 0 : 1;
}

void set_nonblocking(socket_t socket, bool enabled) {
#ifdef _WIN32
  u_long mode = enabled ? 1UL : 0UL;
  ioctlsocket(socket, FIONBIO, &mode);
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  if(flags >= 0) {
    fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK)
                                   : (flags & ~O_NONBLOCK));
  }
#endif
}

void set_no_delay(socket_t socket) {
  const int enabled = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}

void set_receive_timeout(socket_t socket, int timeout_ms) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(timeout_ms);
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

bool send_all(socket_t socket, const std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0;
  while(offset < size) {
    const std::size_t remaining = size - offset;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1U << 20U));
    const int written =
        static_cast<int>(::send(socket,
                                reinterpret_cast<const char*>(data + offset),
                                chunk, 0));
    if(written <= 0) {
      if(written < 0 && interrupted()) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool receive_exact(socket_t socket, std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0;
  while(offset < size) {
    const int received =
        static_cast<int>(::recv(socket,
                                reinterpret_cast<char*>(data + offset),
                                static_cast<int>(size - offset), 0));
    if(received == 0) {
      return false;
    }
    if(received < 0) {
      if(interrupted()) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

}  // namespace

TcpServer::~TcpServer() {
  drop_peer();
  if(listener_ >= 0) {
    close_socket(from_handle(listener_));
    listener_ = -1;
  }
}

bool TcpServer::listen(std::uint16_t port, std::string* error) {
  ignore_sigpipe();
  if(!ensure_sockets(error)) {
    return false;
  }

  const socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(listener == kInvalidSocket) {
    if(error != nullptr) *error = "socket() failed";
    return false;
  }

  const int enabled = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&enabled), sizeof(enabled));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(::bind(listener, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
    if(error != nullptr) *error = "bind(127.0.0.1:" + std::to_string(port) + ") failed";
    close_socket(listener);
    return false;
  }

  sockaddr_in bound{};
#ifdef _WIN32
  int bound_size = sizeof(bound);
#else
  socklen_t bound_size = sizeof(bound);
#endif
  if(getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) !=
     0) {
    if(error != nullptr) *error = "getsockname failed";
    close_socket(listener);
    return false;
  }
  port_ = ntohs(bound.sin_port);

  if(::listen(listener, 1) != 0) {
    if(error != nullptr) *error = "listen failed";
    close_socket(listener);
    return false;
  }
  set_nonblocking(listener, true);
  listener_ = to_handle(listener);
  log_line("I", "tcp", "listening on 127.0.0.1:%u", static_cast<unsigned>(port_));
  return true;
}

bool TcpServer::connected() const { return peer_ >= 0; }

bool TcpServer::accept_pending() {
  if(peer_ >= 0) {
    return true;
  }
  if(listener_ < 0) {
    return false;
  }
  if(wait_readable(from_handle(listener_), 0) != 1) {
    return false;
  }

  sockaddr_in remote{};
#ifdef _WIN32
  int remote_size = sizeof(remote);
#else
  socklen_t remote_size = sizeof(remote);
#endif
  const socket_t peer =
      ::accept(from_handle(listener_), reinterpret_cast<sockaddr*>(&remote),
               &remote_size);
  if(peer == kInvalidSocket) {
    return false;
  }
  set_no_delay(peer);
  set_receive_timeout(peer, 1'000);
  peer_ = to_handle(peer);
  rx_buffer_.clear();
  log_line("I", "tcp", "phone connected");
  return true;
}

void TcpServer::drop_peer() {
  if(peer_ >= 0) {
    close_socket(from_handle(peer_));
    peer_ = -1;
    log_line("I", "tcp", "phone disconnected");
  }
  rx_buffer_.clear();
}

bool TcpServer::send_frame(const moto::ble::Bytes& frame) {
  if(peer_ < 0) {
    return false;
  }
  if(frame.size() > link::kMaxPayload) {
    log_line("E", "tcp", "refusing oversized frame (%zu bytes)", frame.size());
    return false;
  }
  std::vector<std::uint8_t> packet(link::kHeaderSize + frame.size());
  link::encode_header(static_cast<std::uint16_t>(frame.size()), packet.data());
  if(!frame.empty()) {
    std::memcpy(packet.data() + link::kHeaderSize, frame.data(), frame.size());
  }
  if(!send_all(from_handle(peer_), packet.data(), packet.size())) {
    drop_peer();
    return false;
  }
  return true;
}

bool TcpServer::read_frames(std::vector<moto::ble::Bytes>* frames) {
  if(peer_ < 0) {
    return false;
  }

  const socket_t peer = from_handle(peer_);
  for(;;) {
    const int ready = wait_readable(peer, 0);
    if(ready < 0) {
      drop_peer();
      return false;
    }
    if(ready == 0) {
      break;
    }
    std::uint8_t buffer[4096];
    const int received =
        static_cast<int>(::recv(peer, reinterpret_cast<char*>(buffer),
                                static_cast<int>(sizeof(buffer)), 0));
    if(received == 0) {
      drop_peer();
      return false;
    }
    if(received < 0) {
      if(interrupted() || would_block()) {
        break;
      }
      drop_peer();
      return false;
    }
    rx_buffer_.insert(rx_buffer_.end(), buffer, buffer + received);
    if(rx_buffer_.size() > 64U * 1024U) {
      log_line("E", "tcp", "receive buffer overflow");
      drop_peer();
      return false;
    }
  }

  std::size_t offset = 0;
  while(rx_buffer_.size() - offset >= link::kHeaderSize) {
    std::uint16_t payload = 0;
    if(!link::decode_header(rx_buffer_.data() + offset, &payload)) {
      log_line("E", "tcp", "invalid link header");
      drop_peer();
      return false;
    }
    const std::size_t total = link::kHeaderSize + payload;
    if(rx_buffer_.size() - offset < total) {
      break;
    }
    if(payload > 0 && frames != nullptr) {
      frames->emplace_back(
          rx_buffer_.begin() + static_cast<std::ptrdiff_t>(offset + link::kHeaderSize),
          rx_buffer_.begin() + static_cast<std::ptrdiff_t>(offset + total));
    }
    offset += total;
  }
  if(offset > 0) {
    rx_buffer_.erase(rx_buffer_.begin(),
                     rx_buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return true;
}

TcpClient::~TcpClient() { close(); }

bool TcpClient::connect(const std::string& host, std::uint16_t port,
                        std::string* error) {
  ignore_sigpipe();
  if(!ensure_sockets(error)) {
    return false;
  }
  close();

  const socket_t client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(client == kInvalidSocket) {
    if(error != nullptr) *error = "socket() failed";
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if(inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    if(error != nullptr) *error = "invalid IPv4 address: " + host;
    close_socket(client);
    return false;
  }
  set_receive_timeout(client, 1'000);
  if(::connect(client, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
    if(error != nullptr) {
      *error = "connect(" + host + ":" + std::to_string(port) + ") failed";
    }
    close_socket(client);
    return false;
  }
  set_no_delay(client);
  socket_ = to_handle(client);
  return true;
}

void TcpClient::close() {
  if(socket_ >= 0) {
    close_socket(from_handle(socket_));
    socket_ = -1;
  }
}

bool TcpClient::connected() const { return socket_ >= 0; }

bool TcpClient::send_frame(const moto::ble::Bytes& frame) {
  if(socket_ < 0) {
    return false;
  }
  if(frame.size() > link::kMaxPayload) {
    return false;
  }
  std::vector<std::uint8_t> packet(link::kHeaderSize + frame.size());
  link::encode_header(static_cast<std::uint16_t>(frame.size()), packet.data());
  if(!frame.empty()) {
    std::memcpy(packet.data() + link::kHeaderSize, frame.data(), frame.size());
  }
  if(!send_all(from_handle(socket_), packet.data(), packet.size())) {
    close();
    return false;
  }
  return true;
}

int TcpClient::read_frame(moto::ble::Bytes* frame, int timeout_ms) {
  if(socket_ < 0) {
    return -1;
  }
  const int ready = wait_readable(from_handle(socket_), timeout_ms);
  if(ready == 0) {
    return 0;
  }
  if(ready < 0) {
    close();
    return -1;
  }

  std::uint8_t header[link::kHeaderSize];
  if(!receive_exact(from_handle(socket_), header, sizeof(header))) {
    close();
    return -1;
  }
  std::uint16_t payload = 0;
  if(!link::decode_header(header, &payload)) {
    close();
    return -1;
  }
  moto::ble::Bytes bytes(payload);
  if(payload > 0 &&
     !receive_exact(from_handle(socket_), bytes.data(), bytes.size())) {
    close();
    return -1;
  }
  if(frame != nullptr) {
    *frame = std::move(bytes);
  }
  return 1;
}

}  // namespace moto::sim
