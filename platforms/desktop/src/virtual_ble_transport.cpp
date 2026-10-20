#include "virtual_ble_transport.h"

#include <algorithm>

#include "esp_timer.h"
#include "sim_log.h"

namespace moto::sim {
namespace {

constexpr char kTag[] = "virtual_ble";
constexpr std::uint64_t kCommandAckTimeoutMs = 750;
constexpr std::uint8_t kMaxCommandAckRetries = 2;

std::uint64_t monotonic_ms() {
  return static_cast<std::uint64_t>(esp_timer_get_time()) / 1'000U;
}

bool is_session_message(moto::ble::MessageType type) {
  return type == moto::ble::MessageType::ConnectionStatus ||
         type == moto::ble::MessageType::Heartbeat ||
         type == moto::ble::MessageType::Ack;
}

}  // namespace

void VirtualBleTransport::set_callbacks(MessageCallback message,
                                        LinkCallback link,
                                        void* context) noexcept {
  message_ = message;
  link_ = link;
  context_ = context;
}

void VirtualBleTransport::on_peer_connected() {
  connected_ = true;
  protocol_ready_ = false;
  session_id_ = 0;
  peer_frame_size_ = 20;
  has_last_rx_ack_ = false;
  pending_ack_ = {};
  reassembler_.reset();
  watchdog_.reset();
  tx_sequence_.reset();
  last_tx_ms_ = 0;
  note_link(false);
  log_line("I", kTag, "peer connected");
}

void VirtualBleTransport::on_peer_disconnected() {
  if(!connected_) {
    return;
  }
  connected_ = false;
  protocol_ready_ = false;
  session_id_ = 0;
  pending_ack_ = {};
  has_last_rx_ack_ = false;
  reassembler_.reset();
  watchdog_.reset();
  note_link(false);
  log_line("I", kTag, "peer disconnected");
}

void VirtualBleTransport::note_link(bool active) {
  if(link_active_ == active) {
    return;
  }
  link_active_ = active;
  log_line("I", kTag, "link active=%d", active ? 1 : 0);
  if(link_ != nullptr) {
    link_(active, context_);
  }
}

std::size_t VirtualBleTransport::negotiated_frame_size() const noexcept {
  const std::size_t local = kLocalFrameSize;
  const std::size_t peer = peer_frame_size_;
  return std::clamp<std::size_t>(std::min(local, peer),
                                 moto::ble::kFrameOverhead + 1U,
                                 kLocalFrameSize);
}

bool VirtualBleTransport::send_frames(
    const std::vector<moto::ble::Bytes>& frames) {
  if(!connected_ || !send_) {
    return false;
  }
  for(const auto& frame : frames) {
    if(!send_(frame)) {
      return false;
    }
  }
  last_tx_ms_ = monotonic_ms();
  return true;
}

bool VirtualBleTransport::send_message(const moto::ble::Message& message,
                                       std::uint8_t frame_flags) {
  if(!connected_ || !send_) {
    return false;
  }
  if((frame_flags & ~moto::ble::kApplicationFrameFlags) != 0U) {
    return false;
  }
  if((frame_flags & moto::ble::AckRequested) != 0U && pending_ack_.active) {
    log_line("W", kTag, "one acknowledged command is already pending");
    return false;
  }

  const moto::ble::BytesResult encoded = moto::ble::encode_message(message);
  if(!encoded.ok()) {
    log_line("W", kTag, "outbound payload rejected: %s",
             moto::ble::to_string(encoded.error));
    return false;
  }

  const std::uint16_t sequence = tx_sequence_.next();
  moto::ble::FramesResult frames = moto::ble::fragment_message(
      moto::ble::message_type(message), sequence,
      moto::ble::ByteView(encoded.value), negotiated_frame_size(),
      frame_flags);
  if(!frames.ok()) {
    log_line("W", kTag, "outbound fragmentation failed: %s",
             moto::ble::to_string(frames.error));
    return false;
  }
  if(!send_frames(frames.value)) {
    return false;
  }

  if((frame_flags & moto::ble::AckRequested) != 0U) {
    pending_ack_ = {};
    pending_ack_.active = true;
    pending_ack_.sequence = sequence;
    if(const auto* command =
           std::get_if<moto::ble::DeviceCommand>(&message)) {
      pending_ack_.command_id = command->command_id;
    }
    pending_ack_.deadline_ms = monotonic_ms() + kCommandAckTimeoutMs;
    pending_ack_.frames = std::move(frames.value);
  }
  return true;
}

bool VirtualBleTransport::send_from_bridge(
    const moto::ble::Message& message, std::uint8_t frame_flags,
    void* context) {
  return static_cast<VirtualBleTransport*>(context)->send_message(message,
                                                                 frame_flags);
}

void VirtualBleTransport::send_ack(std::uint16_t sequence,
                                   moto::ble::AckStatus status,
                                   std::uint16_t command_id) {
  moto::ble::Ack ack;
  ack.acknowledged_sequence = sequence;
  ack.status = status;
  ack.command_id = command_id;
  (void)send_message(moto::ble::Message{ack});
}

void VirtualBleTransport::send_connection_status(
    moto::ble::ConnectionState state) {
  if(session_id_ == 0) {
    return;
  }
  moto::ble::ConnectionStatus status;
  status.role = moto::ble::EndpointRole::Device;
  status.state = state;
  status.minimum_version = moto::ble::kProtocolVersion;
  status.maximum_version = moto::ble::kProtocolVersion;
  status.capabilities = moto::ble::CapabilityNavigation |
                        moto::ble::CapabilityRouteGeometry |
                        moto::ble::CapabilityTraffic |
                        moto::ble::CapabilityMediaState |
                        moto::ble::CapabilityTouchCommands |
                        moto::ble::CapabilityMusicCommands |
                        moto::ble::CapabilityCommandAck |
                        moto::ble::CapabilityMapScene |
                        moto::ble::CapabilityUiLayout |
                        moto::ble::CapabilityWifiProvisioning;
  status.session_id = session_id_;
  status.max_frame_size =
      static_cast<std::uint16_t>(negotiated_frame_size());
  status.heartbeat_interval_ms = 1'000;
  (void)send_message(moto::ble::Message{status}, moto::ble::Urgent);
}

void VirtualBleTransport::accept_ack(const moto::ble::Ack& ack) {
  if(pending_ack_.active &&
     pending_ack_.sequence == ack.acknowledged_sequence &&
     pending_ack_.command_id == ack.command_id) {
    log_line("I", kTag, "command ACK sequence=%u command=%u status=%u",
             static_cast<unsigned>(ack.acknowledged_sequence),
             static_cast<unsigned>(ack.command_id),
             static_cast<unsigned>(ack.status));
    pending_ack_ = {};
  }
}

void VirtualBleTransport::service_pending_ack(std::uint64_t now_ms) {
  if(!pending_ack_.active || now_ms < pending_ack_.deadline_ms) {
    return;
  }
  if(pending_ack_.retries < kMaxCommandAckRetries && connected_) {
    log_line("W", kTag, "command ACK timeout; retry %u/2",
             static_cast<unsigned>(pending_ack_.retries + 1U));
    send_frames(pending_ack_.frames);
    ++pending_ack_.retries;
    pending_ack_.deadline_ms = now_ms + kCommandAckTimeoutMs;
    return;
  }
  log_line("E", kTag, "command ACK failed after 2 retries");
  pending_ack_ = {};
  send_connection_status(moto::ble::ConnectionState::Degraded);
}

void VirtualBleTransport::on_frame(const moto::ble::Bytes& frame,
                                   std::uint64_t now_ms) {
  if(!connected_) {
    return;
  }

  // A ConnectionStatus START always begins a new session and must not be
  // rejected against the previous session's sequence baseline.
  const moto::ble::FrameResult control = moto::ble::decode_frame(
      moto::ble::ByteView(frame.data(), frame.size()));
  if(control.ok() &&
     control.value.type == moto::ble::MessageType::ConnectionStatus &&
     (control.value.flags & moto::ble::FrameStart) != 0U) {
    reassembler_.reset();
  }

  const moto::ble::ReassemblyResult assembled = reassembler_.push(
      moto::ble::ByteView(frame.data(), frame.size()), now_ms);
  if(assembled.error != moto::ble::Error::None) {
    log_line("W", kTag, "frame rejected: %s at %u",
             moto::ble::to_string(assembled.error),
             static_cast<unsigned>(assembled.error_offset));
    return;
  }

  watchdog_.note_valid_frame(now_ms);
  note_link(true);

  if(assembled.state == moto::ble::ReassemblyState::DuplicateMessage) {
    if((assembled.message.flags & moto::ble::AckRequested) != 0U) {
      if(has_last_rx_ack_ &&
         last_rx_ack_sequence_ == assembled.message.sequence) {
        send_ack(last_rx_ack_sequence_, last_rx_ack_status_,
                 last_rx_ack_command_id_);
      } else {
        send_ack(assembled.message.sequence,
                 moto::ble::AckStatus::Duplicate);
      }
    }
    return;
  }
  if(!assembled.complete()) {
    return;
  }

  moto::ble::MessageResult decoded;
  decoded.error = moto::ble::Error::InvalidArgument;
  if(assembled.message.type == moto::ble::MessageType::ConnectionStatus ||
     assembled.message.type == moto::ble::MessageType::Ack ||
     assembled.message.type == moto::ble::MessageType::DeviceCommand) {
    decoded = moto::ble::decode_message(
        assembled.message.type,
        moto::ble::ByteView(assembled.message.payload));
  }

  const auto* phone_status =
      decoded.ok()
          ? std::get_if<moto::ble::ConnectionStatus>(&decoded.value)
          : nullptr;
  const auto* received_ack =
      decoded.ok() ? std::get_if<moto::ble::Ack>(&decoded.value) : nullptr;

  if(received_ack != nullptr) {
    accept_ack(*received_ack);
  }

  bool reset_for_new_session = false;
  if(phone_status != nullptr &&
     phone_status->role == moto::ble::EndpointRole::Phone) {
    const std::uint32_t previous_session = session_id_;
    reset_for_new_session =
        previous_session != 0 && previous_session != phone_status->session_id;
    if(reset_for_new_session) {
      note_link(false);
      note_link(true);
      protocol_ready_ = false;
      has_last_rx_ack_ = false;
      reassembler_.reset();
      pending_ack_ = {};
      tx_sequence_.reset();
    }
    session_id_ = phone_status->session_id;
    peer_frame_size_ = phone_status->max_frame_size;
    last_tx_ms_ = now_ms;
    const std::uint64_t timeout = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(phone_status->heartbeat_interval_ms) * 3U,
        1'500U, 15'000U);
    watchdog_.set_timeout(timeout);
  }

  moto::ble::AckStatus status = moto::ble::AckStatus::InvalidState;
  if((protocol_ready_ || is_session_message(assembled.message.type)) &&
     message_ != nullptr) {
    status = message_(assembled.message, context_);
  }

  std::uint16_t ack_command_id = 0;
  if(decoded.ok()) {
    if(const auto* command =
           std::get_if<moto::ble::DeviceCommand>(&decoded.value)) {
      ack_command_id = command->command_id;
    }
  }
  if((assembled.message.flags & moto::ble::AckRequested) != 0U &&
     assembled.message.type != moto::ble::MessageType::Ack) {
    send_ack(assembled.message.sequence, status, ack_command_id);
    has_last_rx_ack_ = true;
    last_rx_ack_sequence_ = assembled.message.sequence;
    last_rx_ack_status_ = status;
    last_rx_ack_command_id_ = ack_command_id;
  }

  if(phone_status != nullptr) {
    if(status == moto::ble::AckStatus::Unsupported) {
      send_connection_status(moto::ble::ConnectionState::Closing);
    } else if(status == moto::ble::AckStatus::Ok) {
      if(phone_status->state == moto::ble::ConnectionState::Starting) {
        send_connection_status(moto::ble::ConnectionState::Ready);
      } else if(phone_status->state == moto::ble::ConnectionState::Ready) {
        send_connection_status(moto::ble::ConnectionState::Ready);
        protocol_ready_ = true;
      }
    }
  }
}

void VirtualBleTransport::tick(std::uint64_t now_ms) {
  if(!connected_) {
    return;
  }
  service_pending_ack(now_ms);

  if(!pending_ack_.active && protocol_ready_ && session_id_ != 0 &&
     now_ms - last_tx_ms_ >= 1'000U) {
    moto::ble::Heartbeat heartbeat;
    heartbeat.session_id = session_id_;
    heartbeat.monotonic_ms = static_cast<std::uint32_t>(now_ms);
    heartbeat.status_flags = 0;
    (void)send_message(moto::ble::Message{heartbeat});
  }

  if(watchdog_.expired(now_ms)) {
    note_link(false);
  }
}

}  // namespace moto::sim
