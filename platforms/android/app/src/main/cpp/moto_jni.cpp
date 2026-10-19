// Android JNI adapter over the shared moto::ble protocol and moto::coordinates
// libraries. This is a new Android-specific shim; no iOS/ObjC++ code is reused.
//
// The golden-vector function below rebuilds the exact fixtures used by
// tests/native/ble_protocol_tests.cpp through the same C++ encode_message /
// fragment_message calls, so an on-device instrumented test can compare the
// bytes with shared/protocol/fixtures/ble-navigation-v1.golden.txt.
#include <jni.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "moto/ble_protocol/ble_protocol.hpp"
#include "moto_coordinates.hpp"
#include "nav_app/nav_app.hpp"

namespace {

using moto::ble::Ack;
using moto::ble::AckStatus;
using moto::ble::ByteView;
using moto::ble::Bytes;
using moto::ble::BytesResult;
using moto::ble::Capability;
using moto::ble::ConnectionState;
using moto::ble::ConnectionStatus;
using moto::ble::DeviceCommand;
using moto::ble::DeviceCommandKind;
using moto::ble::DisplayPage;
using moto::ble::EndpointRole;
using moto::ble::FramesResult;
using moto::ble::Heartbeat;
using moto::ble::Maneuver;
using moto::ble::Message;
using moto::ble::NavigationHasDestination;
using moto::ble::NavigationHasFix;
using moto::ble::NavigationHasNextManeuver;
using moto::ble::NavigationHasRouteView;
using moto::ble::NavigationSnapshot;
using moto::ble::NavigationState;
using moto::ble::NetworkState;
using moto::ble::RouteGeometry;
using moto::ble::TrafficLevel;
using moto::ble::UiLayout;
using moto::ble::UiLayoutComponent;
using moto::ble::UiLayoutState;

std::vector<jbyte> ToJBytes(const Bytes& bytes) {
  return std::vector<jbyte>(bytes.begin(), bytes.end());
}

jbyteArray MakeJByteArray(JNIEnv* env, const Bytes& bytes) {
  const jsize size = static_cast<jsize>(bytes.size());
  jbyteArray out = env->NewByteArray(size);
  if (size > 0) {
    env->SetByteArrayRegion(out, 0, size,
                            reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return out;
}

bool ReadBytes(JNIEnv* env, jbyteArray array, Bytes* out) {
  const jsize size = array == nullptr ? 0 : env->GetArrayLength(array);
  out->assign(static_cast<std::size_t>(size), 0);
  if (size > 0) {
    env->GetByteArrayRegion(array, 0, size,
                            reinterpret_cast<jbyte*>(out->data()));
  }
  return true;
}

void Throw(JNIEnv* env, const char* what, moto::ble::Error error) {
  std::string msg = what;
  msg += moto::ble::to_string(error);
  env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), msg.c_str());
}

// Little-endian hex, matching the golden fixture format.
std::string Hex(const Bytes& bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0F]);
  }
  return out;
}

BytesResult Encode(const Message& message) {
  return moto::ble::encode_message(message);
}

std::string FramesToGolden(const std::vector<Bytes>& frames) {
  std::string out;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (i != 0) out.push_back(';');
    out += Hex(frames[i]);
  }
  return out;
}

ConnectionStatus ConnectionFixture() {
  ConnectionStatus value;
  value.role = EndpointRole::Phone;
  value.state = ConnectionState::Ready;
  value.capabilities = Capability::CapabilityNavigation |
                       Capability::CapabilityRouteGeometry |
                       Capability::CapabilityTraffic |
                       Capability::CapabilityMediaState |
                       Capability::CapabilityTouchCommands |
                       Capability::CapabilityMusicCommands |
                       Capability::CapabilityCommandAck;
  value.session_id = 0x12345678U;
  value.max_frame_size = 185;
  value.heartbeat_interval_ms = 1000;
  return value;
}

NavigationSnapshot NavigationFixture() {
  NavigationSnapshot value;
  value.state = NavigationState::Navigating;
  value.network = NetworkState::Online;
  value.display_page = DisplayPage::Navigation;
  value.maneuver = Maneuver::Right;
  value.traffic = TrafficLevel::Slow;
  value.flags = NavigationHasDestination | NavigationHasFix |
                NavigationHasNextManeuver | NavigationHasRouteView;
  value.route_token = moto::ble::route_token("web-beijing-fixture");
  value.route_generation = 0x01020304U;
  value.maneuver_id = 42;
  value.distance_to_maneuver_m = 376;
  value.remaining_distance_m = 12345;
  value.remaining_duration_s = 1020;
  value.route_progress_m = 2100;
  value.total_distance_m = 14445;
  value.speed_deci_kph = 483;
  value.speed_limit_kph = 50;
  value.heading_cdeg = 9123;
  value.accuracy_dm = 38;
  value.cross_track_dm = 125;
  value.road_name = "东长安街";
  value.instruction = "前方路口右转";
  return value;
}

RouteGeometry GeometryFixture() {
  RouteGeometry value;
  value.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  value.route_token = moto::ble::route_token("web-beijing-fixture");
  value.route_generation = 0x01020304U;
  value.total_point_count = 4;
  value.view_origin = {39922596, 116416932};
  value.points = {
      {39922590, 116416920},
      {39922702, 116417285},
      {39922650, 116417200},
      {39923205, 116418160},
  };
  return value;
}

DeviceCommand CommandFixture() {
  DeviceCommand value;
  value.kind = DeviceCommandKind::MusicNext;
  value.command_id = 0x1234U;
  value.page = DisplayPage::Music;
  value.x = moto::ble::kUnknownTouchCoordinate;
  value.y = moto::ble::kUnknownTouchCoordinate;
  value.event_time_ms = 0x10203040U;
  return value;
}

UiLayout UiLayoutFixture() {
  UiLayout value;
  value.layout_revision = 12;
  value.flags = moto::ble::UiLayoutNorthUp |
                moto::ble::UiLayoutMusicPageEnabled |
                moto::ble::UiLayoutMapVisible;
  value.theme = 0;
  value.map_height_px = 232;
  value.map_opacity = 100;
  value.route_r = 255;
  value.route_g = 255;
  value.route_b = 255;
  value.route_width_px = 6;
  value.route_opacity = 100;
  value.street_r = 0x8E;
  value.street_g = 0x8E;
  value.street_b = 0x93;
  value.street_width_px = 3;
  value.street_opacity = 35;
  value.building_opacity = 25;
  value.layer_mask = moto::ble::UiLayerRoads |
                     moto::ble::UiLayerBuildings |
                     moto::ble::UiLayerRoute;
  value.zoom_percent = 100;
  value.components = {
      {1, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 100,
       185, 300, 0, 0},
      {2, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 100,
       300, 238, 0, 0},
      {3, 0, 1, 100, 24, 40, 0, 0},
      {4, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 100,
       29, 220, 0, 0},
      {5, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 90,
       150, 390, 0, 0},
      {6, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 90,
       250, 390, 0, 0},
      {7, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 80, 0,
       0, 0, 120},
      {8, static_cast<std::uint8_t>(moto::ble::UiComponentEnabled), 1, 80, 0,
       0, 2, 120},
  };
  return value;
}

UiLayoutState UiLayoutStateFixture() {
  UiLayoutState value;
  value.layout_revision = 12;
  value.source = static_cast<std::uint8_t>(moto::ble::UiLayoutSource::Phone);
  value.supported_mask = 0xFFU;
  value.flags = moto::ble::UiLayoutNvsSaved;
  value.reserved = 0;
  return value;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_dev_glimpse_motogps_NativeBridge_crc16CcittFalse(JNIEnv* env,
                                                      jclass /*clazz*/,
                                                      jbyteArray bytes) {
  Bytes buffer;
  ReadBytes(env, bytes, &buffer);
  return static_cast<jint>(
      moto::ble::crc16_ccitt_false(ByteView{buffer.data(), buffer.size()}));
}

// fragments a complete encoded payload into GATT-sized frames. Application
// flags may contain only AckRequested(4) and Urgent(8); START/END are assigned
// by the shared implementation.
JNIEXPORT jobjectArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_fragmentMessage(
    JNIEnv* env, jclass /*clazz*/, jint message_type, jint sequence,
    jint application_flags, jint max_frame_size, jbyteArray payload) {
  Bytes buffer;
  ReadBytes(env, payload, &buffer);
  const FramesResult result = moto::ble::fragment_message(
      static_cast<moto::ble::MessageType>(message_type),
      static_cast<std::uint16_t>(sequence),
      ByteView{buffer.data(), buffer.size()},
      static_cast<std::size_t>(max_frame_size),
      static_cast<std::uint8_t>(application_flags));
  if (!result.ok()) {
    Throw(env, "fragment failed: ", result.error);
    return nullptr;
  }
  jclass byte_array_class = env->FindClass("[B");
  jobjectArray out =
      env->NewObjectArray(static_cast<jsize>(result.value.size()),
                          byte_array_class, nullptr);
  for (std::size_t i = 0; i < result.value.size(); ++i) {
    env->SetObjectArrayElement(
        out, static_cast<jsize>(i), MakeJByteArray(env, result.value[i]));
  }
  return out;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_decodeFrame(JNIEnv* env,
                                                  jclass /*clazz*/,
                                                  jbyteArray frame_bytes) {
  Bytes buffer;
  ReadBytes(env, frame_bytes, &buffer);
  moto::ble::FrameResult result =
      moto::ble::decode_frame(ByteView{buffer.data(), buffer.size()});
  if (!result.ok()) {
    Throw(env, "decode failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value.payload);
}

// Rebuilds the upstream golden fixtures through the shared C++ encoder.
// Format matches ble-navigation-v1.golden.txt: one `key=hex` per line.
JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_goldenVectors(JNIEnv* env,
                                                    jclass /*clazz*/) {
  std::string report;

  const BytesResult connection_payload = Encode(Message{ConnectionFixture()});
  const BytesResult navigation_payload = Encode(Message{NavigationFixture()});
  const BytesResult geometry_payload = Encode(Message{GeometryFixture()});
  const BytesResult command_payload = Encode(Message{CommandFixture()});
  const BytesResult ui_layout_payload = Encode(Message{UiLayoutFixture()});
  const BytesResult ui_layout_state_payload =
      Encode(Message{UiLayoutStateFixture()});
  if (!connection_payload.ok() || !navigation_payload.ok() ||
      !geometry_payload.ok() || !command_payload.ok() ||
      !ui_layout_payload.ok() || !ui_layout_state_payload.ok()) {
    Throw(env, "golden encode failed: ",
          !connection_payload.ok() ? connection_payload.error
                                   : navigation_payload.error);
    return nullptr;
  }

  const FramesResult connection_frames = moto::ble::fragment_message(
      moto::ble::MessageType::ConnectionStatus, 1,
      ByteView{connection_payload.value.data(), connection_payload.value.size()},
      512);
  const FramesResult command_frames = moto::ble::fragment_message(
      moto::ble::MessageType::DeviceCommand, 0x1234,
      ByteView{command_payload.value.data(), command_payload.value.size()}, 20,
      moto::ble::AckRequested | moto::ble::Urgent);
  const FramesResult ui_layout_frames = moto::ble::fragment_message(
      moto::ble::MessageType::UiLayout, 0x3001,
      ByteView{ui_layout_payload.value.data(), ui_layout_payload.value.size()},
      185, moto::ble::AckRequested);
  const FramesResult ui_layout_frames20 = moto::ble::fragment_message(
      moto::ble::MessageType::UiLayout, 0x3002,
      ByteView{ui_layout_payload.value.data(), ui_layout_payload.value.size()},
      20, moto::ble::AckRequested);
  if (!connection_frames.ok() || !command_frames.ok() ||
      !ui_layout_frames.ok() || !ui_layout_frames20.ok()) {
    Throw(env, "golden fragment failed: ", connection_frames.ok()
                                               ? command_frames.error
                                               : connection_frames.error);
    return nullptr;
  }

  report += "connection.payload=" + Hex(connection_payload.value) + "\n";
  report += "connection.frame=" + FramesToGolden(connection_frames.value) + "\n";
  report += "navigation.payload=" + Hex(navigation_payload.value) + "\n";
  report += "geometry.payload=" + Hex(geometry_payload.value) + "\n";
  report += "command.payload=" + Hex(command_payload.value) + "\n";
  report += "command.frames20=" + FramesToGolden(command_frames.value) + "\n";
  report += "ui_layout.payload=" + Hex(ui_layout_payload.value) + "\n";
  report += "ui_layout.frame=" + FramesToGolden(ui_layout_frames.value) + "\n";
  report += "ui_layout.frames20=" + FramesToGolden(ui_layout_frames20.value) +
            "\n";
  report += "ui_layout_state.payload=" + Hex(ui_layout_state_payload.value) +
            "\n";
  return env->NewStringUTF(report.c_str());
}

JNIEXPORT jdoubleArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_wgs84ToGcj02(JNIEnv* env,
                                                   jclass /*clazz*/,
                                                   jdouble lon,
                                                   jdouble lat) {
  moto::coordinates::ConversionResult result = moto::coordinates::wgs84_to_gcj02(
      moto::coordinates::Coordinate{lon, lat});
  jdoubleArray out = env->NewDoubleArray(3);
  jdouble values[3] = {result.coordinate.longitude_deg,
                       result.coordinate.latitude_deg,
                       result.ok() ? 1.0 : 0.0};
  env->SetDoubleArrayRegion(out, 0, 3, values);
  return out;
}

// ---------------------------------------------------------------------------
// Message encoders used by the BLE session (handshake, heartbeat, ACK).
// ---------------------------------------------------------------------------

namespace {

std::string HexOf(const Bytes& bytes) { return Hex(bytes); }

}  // namespace

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeConnectionStatus(
    JNIEnv* env, jclass /*clazz*/, jint role, jint state, jint minimum_version,
    jint maximum_version, jlong capabilities, jlong session_id,
    jint max_frame_size, jint heartbeat_interval_ms) {
  ConnectionStatus status;
  status.role = static_cast<EndpointRole>(role);
  status.state = static_cast<ConnectionState>(state);
  status.minimum_version = static_cast<std::uint8_t>(minimum_version);
  status.maximum_version = static_cast<std::uint8_t>(maximum_version);
  status.capabilities = static_cast<std::uint32_t>(capabilities);
  status.session_id = static_cast<std::uint32_t>(session_id);
  status.max_frame_size = static_cast<std::uint16_t>(max_frame_size);
  status.heartbeat_interval_ms =
      static_cast<std::uint16_t>(heartbeat_interval_ms);
  const BytesResult result = Encode(Message{status});
  if (!result.ok()) {
    Throw(env, "encode connection status failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeHeartbeat(
    JNIEnv* env, jclass /*clazz*/, jlong session_id, jlong monotonic_ms,
    jint status_flags) {
  Heartbeat heartbeat;
  heartbeat.session_id = static_cast<std::uint32_t>(session_id);
  heartbeat.monotonic_ms = static_cast<std::uint32_t>(monotonic_ms);
  heartbeat.status_flags = static_cast<std::uint16_t>(status_flags);
  const BytesResult result = Encode(Message{heartbeat});
  if (!result.ok()) {
    Throw(env, "encode heartbeat failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeAck(JNIEnv* env, jclass /*clazz*/,
                                                jint acknowledged_sequence,
                                                jint status, jint command_id) {
  Ack ack;
  ack.acknowledged_sequence = static_cast<std::uint16_t>(acknowledged_sequence);
  ack.status = static_cast<AckStatus>(status);
  ack.command_id = static_cast<std::uint16_t>(command_id);
  const BytesResult result = Encode(Message{ack});
  if (!result.ok()) {
    Throw(env, "encode ack failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jlongArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_decodeConnectionStatus(
    JNIEnv* env, jclass /*clazz*/, jbyteArray payload) {
  Bytes buffer;
  ReadBytes(env, payload, &buffer);
  const moto::ble::MessageResult result = moto::ble::decode_message(
      moto::ble::MessageType::ConnectionStatus,
      ByteView{buffer.data(), buffer.size()});
  if (!result.ok()) {
    Throw(env, "decode connection status failed: ", result.error);
    return nullptr;
  }
  const auto& status = std::get<ConnectionStatus>(result.value);
  jlong values[9] = {moto::ble::kPayloadRevision,
                     static_cast<jlong>(status.role),
                     static_cast<jlong>(status.state),
                     static_cast<jlong>(status.minimum_version),
                     static_cast<jlong>(status.maximum_version),
                     static_cast<jlong>(status.capabilities),
                     static_cast<jlong>(status.session_id),
                     static_cast<jlong>(status.max_frame_size),
                     static_cast<jlong>(status.heartbeat_interval_ms)};
  jlongArray out = env->NewLongArray(9);
  env->SetLongArrayRegion(out, 0, 9, values);
  return out;
}

JNIEXPORT jintArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_decodeAck(JNIEnv* env, jclass /*clazz*/,
                                                jbyteArray payload) {
  Bytes buffer;
  ReadBytes(env, payload, &buffer);
  const moto::ble::MessageResult result = moto::ble::decode_message(
      moto::ble::MessageType::Ack, ByteView{buffer.data(), buffer.size()});
  if (!result.ok()) {
    Throw(env, "decode ack failed: ", result.error);
    return nullptr;
  }
  const auto& ack = std::get<Ack>(result.value);
  jint values[4] = {static_cast<jint>(moto::ble::kPayloadRevision),
                    static_cast<jint>(ack.acknowledged_sequence),
                    static_cast<jint>(ack.status),
                    static_cast<jint>(ack.command_id)};
  jintArray out = env->NewIntArray(4);
  env->SetIntArrayRegion(out, 0, 4, values);
  return out;
}

JNIEXPORT jlongArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_decodeDeviceCommand(
    JNIEnv* env, jclass /*clazz*/, jbyteArray payload) {
  Bytes buffer;
  ReadBytes(env, payload, &buffer);
  const moto::ble::MessageResult result = moto::ble::decode_message(
      moto::ble::MessageType::DeviceCommand,
      ByteView{buffer.data(), buffer.size()});
  if (!result.ok()) {
    Throw(env, "decode device command failed: ", result.error);
    return nullptr;
  }
  const auto& command = std::get<DeviceCommand>(result.value);
  jlong values[7] = {static_cast<jlong>(moto::ble::kPayloadRevision),
                     static_cast<jlong>(command.kind),
                     static_cast<jlong>(command.command_id),
                     static_cast<jlong>(command.page),
                     static_cast<jlong>(command.x),
                     static_cast<jlong>(command.y),
                     static_cast<jlong>(command.event_time_ms)};
  jlongArray out = env->NewLongArray(7);
  env->SetLongArrayRegion(out, 0, 7, values);
  return out;
}

// ---------------------------------------------------------------------------
// Reassembler handles. One handle per inbound direction (phone side).
// ---------------------------------------------------------------------------

namespace {

std::mutex g_handle_mutex;
std::unordered_map<jlong, std::unique_ptr<moto::ble::Reassembler>>
    g_reassemblers;
jlong g_next_handle = 1;

}  // namespace

JNIEXPORT jlong JNICALL
Java_dev_glimpse_motogps_NativeBridge_createReassembler(JNIEnv* /*env*/,
                                                        jclass /*clazz*/,
                                                        jlong timeout_ms) {
  moto::ble::ReassemblerConfig config;
  if (timeout_ms > 0) {
    config.fragment_timeout_ms = static_cast<std::uint64_t>(timeout_ms);
  }
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  const jlong handle = g_next_handle++;
  g_reassemblers.emplace(
      handle, std::make_unique<moto::ble::Reassembler>(config));
  return handle;
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_reassemblerPush(JNIEnv* env,
                                                      jclass /*clazz*/,
                                                      jlong handle,
                                                      jbyteArray frame,
                                                      jlong now_ms) {
  Bytes buffer;
  ReadBytes(env, frame, &buffer);
  moto::ble::ReassemblyResult result;
  {
    std::lock_guard<std::mutex> lock(g_handle_mutex);
    const auto it = g_reassemblers.find(handle);
    if (it == g_reassemblers.end()) {
      Throw(env, "unknown reassembler handle", moto::ble::Error::InvalidArgument);
      return nullptr;
    }
    result = it->second->push(ByteView{buffer.data(), buffer.size()},
                              static_cast<std::uint64_t>(now_ms));
  }
  std::string out = "state=";
  out += std::to_string(static_cast<int>(result.state));
  out += ",error=";
  out += std::to_string(static_cast<int>(result.error));
  out += ",dropped=";
  out += result.dropped_incomplete ? "1" : "0";
  out += ",type=";
  out += std::to_string(static_cast<int>(result.message.type));
  out += ",version=";
  out += std::to_string(static_cast<int>(result.message.version));
  out += ",flags=";
  out += std::to_string(static_cast<int>(result.message.flags));
  out += ",seq=";
  out += std::to_string(static_cast<int>(result.message.sequence));
  out += ",payload=";
  out += HexOf(result.message.payload);
  return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL
Java_dev_glimpse_motogps_NativeBridge_reassemblerReset(JNIEnv* /*env*/,
                                                       jclass /*clazz*/,
                                                       jlong handle) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  const auto it = g_reassemblers.find(handle);
  if (it != g_reassemblers.end()) {
    it->second->reset();
  }
}

JNIEXPORT void JNICALL
Java_dev_glimpse_motogps_NativeBridge_destroyReassembler(JNIEnv* /*env*/,
                                                         jclass /*clazz*/,
                                                         jlong handle) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  g_reassemblers.erase(handle);
}

// ---------------------------------------------------------------------------
// Shared navigation core (nav_app) bridge. The core stays authoritative; JNI
// only serialises its commands and snapshot as JSON for Kotlin.
// ---------------------------------------------------------------------------

namespace {

using moto::nav::NavApp;
using moto::nav::NavCommand;
using moto::nav::NavCommands;
using moto::nav::NavEvent;
using moto::nav::NavSnapshot;

std::mutex g_nav_mutex;
std::unordered_map<jlong, std::unique_ptr<NavApp>> g_nav_apps;
jlong g_next_nav_handle = 1;

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string SnapshotJson(const NavSnapshot& s) {
  std::string out = "{";
  out += "\"state\":" + std::to_string(static_cast<int>(s.state));
  out += ",\"network\":" + std::to_string(static_cast<int>(s.network));
  out += ",\"displayPage\":" + std::to_string(static_cast<int>(s.display_page));
  out += ",\"hasDestination\":";
  out += s.has_destination ? "true" : "false";
  out += ",\"hasUsableFix\":";
  out += s.has_usable_fix ? "true" : "false";
  out += ",\"gnssStale\":";
  out += s.gnss_stale ? "true" : "false";
  out += ",\"offRoute\":";
  out += s.off_route ? "true" : "false";
  out += ",\"routeRequestInFlight\":";
  out += s.route_request_in_flight ? "true" : "false";
  out += ",\"trafficRequestInFlight\":";
  out += s.traffic_request_in_flight ? "true" : "false";
  out += ",\"positionLon\":" + std::to_string(s.position.longitude_deg);
  out += ",\"positionLat\":" + std::to_string(s.position.latitude_deg);
  out += ",\"destinationLon\":" + std::to_string(s.destination.longitude_deg);
  out += ",\"destinationLat\":" + std::to_string(s.destination.latitude_deg);
  out += ",\"speedMps\":" + std::to_string(s.speed_mps);
  out += ",\"headingDeg\":" + std::to_string(s.heading_deg);
  out += ",\"accuracyM\":" + std::to_string(s.horizontal_accuracy_m);
  out += ",\"crossTrackM\":" + std::to_string(s.cross_track_distance_m);
  out += ",\"speedLimitKph\":" + std::to_string(s.speed_limit_kph);
  out += ",\"hasRouteView\":";
  out += s.has_route_view ? "true" : "false";
  out += ",\"routeViewOriginLon\":" +
         std::to_string(s.route_view_origin.longitude_deg);
  out += ",\"routeViewOriginLat\":" +
         std::to_string(s.route_view_origin.latitude_deg);
  out += ",\"routeViewPoints\":[";
  for (std::uint8_t i = 0; i < s.route_view_point_count; ++i) {
    if (i) out += ",";
    out += "[" + std::to_string(s.route_view_points[i].longitude_deg) + "," +
           std::to_string(s.route_view_points[i].latitude_deg) + "]";
  }
  out += "]";
  out += ",\"routeId\":\"" + JsonEscape(s.route_id) + "\"";
  out += ",\"routeProgressM\":" + std::to_string(s.route_progress_m);
  out += ",\"totalDistanceM\":" + std::to_string(s.total_distance_m);
  out += ",\"remainingDistanceM\":" + std::to_string(s.remaining_distance_m);
  out += ",\"remainingDurationS\":" + std::to_string(s.remaining_duration_s);
  out += ",\"hasNextManeuver\":";
  out += s.has_next_maneuver ? "true" : "false";
  out += ",\"maneuverType\":" +
         std::to_string(static_cast<int>(s.next_maneuver.type));
  out += ",\"maneuverId\":" + std::to_string(s.next_maneuver.id);
  out += ",\"distanceToManeuverM\":" +
         std::to_string(s.distance_to_next_maneuver_m);
  out += ",\"roadName\":\"" + JsonEscape(s.next_maneuver.road_name) + "\"";
  out += ",\"instruction\":\"" + JsonEscape(s.next_maneuver.instruction) + "\"";
  out += ",\"roundaboutExit\":" +
         std::to_string(static_cast<int>(s.next_maneuver.roundabout_exit));
  out += ",\"trafficAhead\":" + std::to_string(static_cast<int>(s.traffic_ahead));
  out += ",\"nowMs\":" + std::to_string(s.now_ms);
  out += ",\"routeGeneration\":" + std::to_string(s.route_generation);
  out += ",\"mapSceneRevision\":" + std::to_string(s.map_scene_revision);
  out += "}";
  return out;
}

std::string CommandsJson(const NavCommands& commands) {
  std::string out = "[";
  for (std::size_t i = 0; i < commands.size(); ++i) {
    if (i) out += ",";
    const NavCommand& command = commands[i];
    out += "{";
    out += "\"type\":" + std::to_string(static_cast<int>(command.type));
    out += ",\"requestId\":" + std::to_string(command.request_id);
    out += ",\"originLon\":" + std::to_string(command.route.origin.longitude_deg);
    out += ",\"originLat\":" + std::to_string(command.route.origin.latitude_deg);
    out += ",\"destinationLon\":" +
           std::to_string(command.route.destination.longitude_deg);
    out += ",\"destinationLat\":" +
           std::to_string(command.route.destination.latitude_deg);
    out += ",\"reroute\":";
    out += command.route.is_reroute ? "true" : "false";
    out += ",\"routeId\":\"" + JsonEscape(command.route_id) + "\"";
    out += "}";
  }
  out += "]";
  return out;
}

std::string HandleResult(NavApp* app, NavCommands commands) {
  std::string out = "{\"commands\":";
  out += CommandsJson(commands);
  out += ",\"snapshot\":";
  out += SnapshotJson(app->snapshot());
  out += "}";
  return out;
}

NavApp* LookupApp(jlong handle) {
  const auto it = g_nav_apps.find(handle);
  return it == g_nav_apps.end() ? nullptr : it->second.get();
}

jstring ResultToJString(JNIEnv* env, NavApp* app, NavCommands commands) {
  const std::string out = HandleResult(app, std::move(commands));
  return env->NewStringUTF(out.c_str());
}

NavCommands HandleNav(JNIEnv* /*env*/, jlong handle, NavEvent event, bool* ok) {
  std::lock_guard<std::mutex> lock(g_nav_mutex);
  NavApp* app = LookupApp(handle);
  if (app == nullptr) {
    *ok = false;
    return {};
  }
  *ok = true;
  return app->handle(std::move(event));
}

jstring NavEventResult(JNIEnv* env, jlong handle, NavEvent event) {
  std::lock_guard<std::mutex> lock(g_nav_mutex);
  NavApp* app = LookupApp(handle);
  if (app == nullptr) {
    Throw(env, "unknown nav handle", moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  return ResultToJString(env, app, app->handle(std::move(event)));
}

}  // namespace

JNIEXPORT jlong JNICALL
Java_dev_glimpse_motogps_NativeBridge_navCreate(JNIEnv* /*env*/,
                                                jclass /*clazz*/) {
  std::lock_guard<std::mutex> lock(g_nav_mutex);
  const jlong handle = g_next_nav_handle++;
  g_nav_apps.emplace(handle, std::make_unique<NavApp>(moto::nav::NavCoreConfig{}));
  return handle;
}

JNIEXPORT void JNICALL
Java_dev_glimpse_motogps_NativeBridge_navDestroy(JNIEnv* /*env*/,
                                                 jclass /*clazz*/,
                                                 jlong handle) {
  std::lock_guard<std::mutex> lock(g_nav_mutex);
  g_nav_apps.erase(handle);
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navReset(JNIEnv* env, jclass /*clazz*/,
                                               jlong handle) {
  return NavEventResult(env, handle, moto::nav::Reset{});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navBeginNavigation(JNIEnv* env,
                                                         jclass /*clazz*/,
                                                         jlong handle,
                                                         jdouble lon,
                                                         jdouble lat) {
  moto::nav::BeginNavigation event;
  event.destination = {lat, lon};
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navCancelNavigation(JNIEnv* env,
                                                          jclass /*clazz*/,
                                                          jlong handle) {
  return NavEventResult(env, handle, NavEvent{moto::nav::CancelNavigation{}});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navSetNetwork(JNIEnv* env,
                                                    jclass /*clazz*/,
                                                    jlong handle,
                                                    jint network) {
  moto::nav::NetworkChanged event;
  event.state = static_cast<moto::nav::NetworkState>(network);
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navSelectPage(JNIEnv* env,
                                                    jclass /*clazz*/,
                                                    jlong handle,
                                                    jint page) {
  moto::nav::DisplayPageSelected event;
  event.page = static_cast<moto::nav::DisplayPage>(page);
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navPushFix(JNIEnv* env, jclass /*clazz*/,
                                                 jlong handle, jdouble lon,
                                                 jdouble lat, jdouble accuracy_m,
                                                 jdouble speed_mps,
                                                 jdouble heading_deg,
                                                 jlong timestamp_ms) {
  moto::nav::GnssFixReceived event;
  event.fix.position = {lat, lon};
  event.fix.accuracy_m = static_cast<float>(accuracy_m);
  event.fix.speed_mps = static_cast<float>(speed_mps);
  event.fix.heading_deg = static_cast<float>(heading_deg);
  event.fix.timestamp_ms = static_cast<std::uint64_t>(timestamp_ms);
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navTick(JNIEnv* env, jclass /*clazz*/,
                                              jlong handle, jlong now_ms) {
  moto::nav::Tick event;
  event.timestamp_ms = static_cast<std::uint64_t>(now_ms);
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navAcceptRoute(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jint request_id,
    jstring route_id, jdouble total_distance_m, jdouble total_duration_s,
    jint speed_limit_kph, jlong generated_at_ms, jlong received_at_ms,
    jdoubleArray polyline_lon_lat, jintArray maneuver_numeric,
    jobjectArray maneuver_roads, jobjectArray maneuver_instructions,
    jdoubleArray traffic) {
  moto::nav::RouteBundle route;
  if (route_id != nullptr) {
    const char* text = env->GetStringUTFChars(route_id, nullptr);
    route.route_id = text != nullptr ? text : "";
    if (text != nullptr) env->ReleaseStringUTFChars(route_id, text);
  }
  route.total_distance_m = total_distance_m;
  route.total_duration_s = total_duration_s;
  route.speed_limit_kph = static_cast<std::uint16_t>(speed_limit_kph);
  route.generated_at_ms = static_cast<std::uint64_t>(generated_at_ms);

  if (polyline_lon_lat != nullptr) {
    const jsize count = env->GetArrayLength(polyline_lon_lat);
    std::vector<double> values(static_cast<std::size_t>(count));
    env->GetDoubleArrayRegion(polyline_lon_lat, 0, count, values.data());
    for (jsize i = 0; i + 1 < count; i += 2) {
      route.polyline.push_back({values[i + 1], values[i]});
    }
  }
  if (maneuver_numeric != nullptr && maneuver_roads != nullptr &&
      maneuver_instructions != nullptr) {
    const jsize count = env->GetArrayLength(maneuver_numeric);
    std::vector<int> values(static_cast<std::size_t>(count));
    env->GetIntArrayRegion(maneuver_numeric, 0, count, values.data());
    const jsize maneuvers = env->GetArrayLength(maneuver_roads);
    for (jsize i = 0; i + 4 < count && i / 5 < maneuvers; i += 5) {
      moto::nav::Maneuver maneuver;
      maneuver.id = static_cast<std::uint32_t>(values[i]);
      maneuver.type = static_cast<moto::nav::ManeuverType>(values[i + 1]);
      maneuver.route_offset_m = static_cast<double>(values[i + 2]);
      maneuver.roundabout_exit = static_cast<std::uint8_t>(values[i + 3]);
      auto read_string = [env](jobjectArray array, jsize index) {
        std::string value;
        auto item = static_cast<jstring>(env->GetObjectArrayElement(array, index));
        if (item != nullptr) {
          const char* text = env->GetStringUTFChars(item, nullptr);
          if (text != nullptr) {
            value = text;
            env->ReleaseStringUTFChars(item, text);
          }
          env->DeleteLocalRef(item);
        }
        return value;
      };
      maneuver.road_name = read_string(maneuver_roads, i / 5);
      maneuver.instruction = read_string(maneuver_instructions, i / 5);
      route.maneuvers.push_back(std::move(maneuver));
    }
  }
  if (traffic != nullptr) {
    const jsize count = env->GetArrayLength(traffic);
    std::vector<double> values(static_cast<std::size_t>(count));
    env->GetDoubleArrayRegion(traffic, 0, count, values.data());
    for (jsize i = 0; i + 2 < count; i += 3) {
      route.traffic.push_back(
          {values[i],
           values[i + 1],
           static_cast<moto::nav::TrafficLevel>(static_cast<int>(values[i + 2]))});
    }
  }
  moto::nav::RouteReady event;
  event.request_id = static_cast<std::uint32_t>(request_id);
  event.route = std::move(route);
  event.received_at_ms = static_cast<std::uint64_t>(received_at_ms);
  return NavEventResult(env, handle, NavEvent{std::move(event)});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navRejectRoute(JNIEnv* env,
                                                     jclass /*clazz*/,
                                                     jlong handle,
                                                     jint request_id,
                                                     jboolean retryable,
                                                     jlong received_at_ms) {
  moto::nav::RouteFailed event;
  event.request_id = static_cast<std::uint32_t>(request_id);
  event.retryable = retryable == JNI_TRUE;
  event.received_at_ms = static_cast<std::uint64_t>(received_at_ms);
  return NavEventResult(env, handle, NavEvent{event});
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navSnapshot(JNIEnv* env,
                                                  jclass /*clazz*/,
                                                  jlong handle) {
  std::lock_guard<std::mutex> lock(g_nav_mutex);
  NavApp* app = LookupApp(handle);
  if (app == nullptr) {
    Throw(env, "unknown nav handle", moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  const std::string out = SnapshotJson(app->snapshot());
  return env->NewStringUTF(out.c_str());
}

// --- BLE display encoders ---------------------------------------------------

JNIEXPORT jlong JNICALL
Java_dev_glimpse_motogps_NativeBridge_bleRouteToken(JNIEnv* env,
                                                    jclass /*clazz*/,
                                                    jstring route_id) {
  if (route_id == nullptr) return 0;
  const char* text = env->GetStringUTFChars(route_id, nullptr);
  if (text == nullptr) return 0;
  const std::uint32_t token = moto::ble::route_token(text);
  env->ReleaseStringUTFChars(route_id, text);
  return static_cast<jlong>(token);
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeNavigationSnapshot(
    JNIEnv* env, jclass /*clazz*/, jlongArray numeric, jstring road_name,
    jstring instruction) {
  const jsize count = numeric == nullptr ? 0 : env->GetArrayLength(numeric);
  if (count < 20) {
    Throw(env, "navigation snapshot needs 20 numeric fields",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  std::vector<jlong> v(static_cast<std::size_t>(count));
  env->GetLongArrayRegion(numeric, 0, count, v.data());

  NavigationSnapshot snapshot;
  snapshot.state = static_cast<NavigationState>(v[0]);
  snapshot.network = static_cast<NetworkState>(v[1]);
  snapshot.display_page = static_cast<DisplayPage>(v[2]);
  snapshot.maneuver = static_cast<Maneuver>(v[3]);
  snapshot.traffic = static_cast<TrafficLevel>(v[4]);
  snapshot.flags = static_cast<std::uint16_t>(v[5]);
  snapshot.route_token = static_cast<std::uint32_t>(v[6]);
  snapshot.route_generation = static_cast<std::uint32_t>(v[7]);
  snapshot.maneuver_id = static_cast<std::uint32_t>(v[8]);
  snapshot.distance_to_maneuver_m = static_cast<std::uint32_t>(v[9]);
  snapshot.remaining_distance_m = static_cast<std::uint32_t>(v[10]);
  snapshot.remaining_duration_s = static_cast<std::uint32_t>(v[11]);
  snapshot.route_progress_m = static_cast<std::uint32_t>(v[12]);
  snapshot.total_distance_m = static_cast<std::uint32_t>(v[13]);
  snapshot.speed_deci_kph = static_cast<std::uint16_t>(v[14]);
  snapshot.speed_limit_kph = static_cast<std::uint16_t>(v[15]);
  snapshot.heading_cdeg = static_cast<std::uint16_t>(v[16]);
  snapshot.accuracy_dm = static_cast<std::uint16_t>(v[17]);
  snapshot.cross_track_dm = static_cast<std::uint16_t>(v[18]);
  snapshot.roundabout_exit = static_cast<std::uint8_t>(v[19]);
  auto read = [env](jstring value) {
    std::string out;
    if (value == nullptr) return out;
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (text != nullptr) {
      out = text;
      env->ReleaseStringUTFChars(value, text);
    }
    return out;
  };
  snapshot.road_name = read(road_name);
  snapshot.instruction = read(instruction);

  const BytesResult result = Encode(Message{snapshot});
  if (!result.ok()) {
    Throw(env, "encode navigation snapshot failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeRouteGeometry(
    JNIEnv* env, jclass /*clazz*/, jlongArray meta, jdoubleArray points_lon_lat) {
  if (meta == nullptr || env->GetArrayLength(meta) < 9 || points_lon_lat == nullptr) {
    Throw(env, "route geometry arguments missing",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  jlong m[9];
  env->GetLongArrayRegion(meta, 0, 9, m);
  RouteGeometry geometry;
  geometry.coordinate_system = static_cast<moto::ble::CoordinateSystem>(m[0]);
  geometry.route_token = static_cast<std::uint32_t>(m[1]);
  geometry.route_generation = static_cast<std::uint32_t>(m[2]);
  geometry.chunk_index = static_cast<std::uint16_t>(m[3]);
  geometry.chunk_count = static_cast<std::uint16_t>(m[4]);
  geometry.first_point_index = static_cast<std::uint16_t>(m[5]);
  geometry.total_point_count = static_cast<std::uint16_t>(m[6]);
  geometry.view_origin = {static_cast<std::int32_t>(m[7]),
                          static_cast<std::int32_t>(m[8])};

  const jsize count = env->GetArrayLength(points_lon_lat);
  std::vector<double> values(static_cast<std::size_t>(count));
  env->GetDoubleArrayRegion(points_lon_lat, 0, count, values.data());
  for (jsize i = 0; i + 1 < count; i += 2) {
    geometry.points.push_back(
        {static_cast<std::int32_t>(std::llround(values[i + 1] * 1e6)),
         static_cast<std::int32_t>(std::llround(values[i] * 1e6))});
  }

  const BytesResult result = Encode(Message{geometry});
  if (!result.ok()) {
    Throw(env, "encode route geometry failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jstring JNICALL
Java_dev_glimpse_motogps_NativeBridge_navAcceptTraffic(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jint request_id,
    jstring route_id, jint remaining_duration_s, jlong observed_at_ms,
    jdoubleArray segments) {
  moto::nav::TrafficSnapshot traffic;
  if (route_id != nullptr) {
    const char* text = env->GetStringUTFChars(route_id, nullptr);
    traffic.route_id = text != nullptr ? text : "";
    if (text != nullptr) env->ReleaseStringUTFChars(route_id, text);
  }
  traffic.remaining_duration_s =
      static_cast<std::uint32_t>(remaining_duration_s);
  traffic.observed_at_ms = static_cast<std::uint64_t>(observed_at_ms);
  if (segments != nullptr) {
    const jsize count = env->GetArrayLength(segments);
    std::vector<double> values(static_cast<std::size_t>(count));
    env->GetDoubleArrayRegion(segments, 0, count, values.data());
    for (jsize i = 0; i + 2 < count; i += 3) {
      traffic.segments.push_back(
          {values[i], values[i + 1],
           static_cast<moto::nav::TrafficLevel>(static_cast<int>(values[i + 2]))});
    }
  }
  moto::nav::TrafficUpdated event;
  event.request_id = static_cast<std::uint32_t>(request_id);
  event.traffic = std::move(traffic);
  return NavEventResult(env, handle, NavEvent{std::move(event)});
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeMapScene(
    JNIEnv* env, jclass /*clazz*/, jlongArray meta, jintArray road_classes,
    jintArray road_ranges, jdoubleArray road_points, jintArray building_classes,
    jintArray building_ranges, jdoubleArray building_points) {
  if (meta == nullptr || env->GetArrayLength(meta) < 4) {
    Throw(env, "map scene meta missing", moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  jlong m[4];
  env->GetLongArrayRegion(meta, 0, 4, m);
  moto::ble::MapScene scene;
  scene.coordinate_system = moto::ble::CoordinateSystem::Gcj02;
  scene.scene_revision = static_cast<std::uint32_t>(m[0]);
  scene.view_origin = {static_cast<std::int32_t>(m[1]),
                       static_cast<std::int32_t>(m[2])};
  scene.radius_m = static_cast<std::uint16_t>(m[3]);

  auto read_points = [env](jdoubleArray array) {
    std::vector<std::pair<double, double>> out;
    if (array == nullptr) return out;
    const jsize count = env->GetArrayLength(array);
    std::vector<double> values(static_cast<std::size_t>(count));
    env->GetDoubleArrayRegion(array, 0, count, values.data());
    for (jsize i = 0; i + 1 < count; i += 2) {
      out.emplace_back(values[i + 1], values[i]);  // lat, lon
    }
    return out;
  };

  if (road_classes != nullptr && road_ranges != nullptr) {
    const jsize ranges = env->GetArrayLength(road_ranges);
    std::vector<int> range_values(static_cast<std::size_t>(ranges));
    env->GetIntArrayRegion(road_ranges, 0, ranges, range_values.data());
    const jsize classes = env->GetArrayLength(road_classes);
    std::vector<int> class_values(static_cast<std::size_t>(classes));
    env->GetIntArrayRegion(road_classes, 0, classes, class_values.data());
    const auto points = read_points(road_points);
    for (jsize i = 0; i + 1 < ranges && i / 2 < classes; i += 2) {
      const int start = range_values[i];
      const int count = range_values[i + 1];
      if (start < 0 || count < 2 ||
          start + count > static_cast<int>(points.size())) {
        continue;
      }
      moto::ble::MapRoadPolyline road;
      road.road_class = static_cast<moto::ble::MapRoadClass>(class_values[i / 2]);
      for (int p = 0; p < count; ++p) {
        const auto& point = points[static_cast<std::size_t>(start + p)];
        road.points.push_back(
            {static_cast<std::int32_t>(std::llround(point.first * 1e6)),
             static_cast<std::int32_t>(std::llround(point.second * 1e6))});
      }
      scene.roads.push_back(std::move(road));
    }
  }

  if (building_classes != nullptr && building_ranges != nullptr) {
    const jsize ranges = env->GetArrayLength(building_ranges);
    std::vector<int> range_values(static_cast<std::size_t>(ranges));
    env->GetIntArrayRegion(building_ranges, 0, ranges, range_values.data());
    const jsize classes = env->GetArrayLength(building_classes);
    std::vector<int> class_values(static_cast<std::size_t>(classes));
    env->GetIntArrayRegion(building_classes, 0, classes, class_values.data());
    const auto points = read_points(building_points);
    for (jsize i = 0; i + 1 < ranges && i / 2 < classes; i += 2) {
      const int start = range_values[i];
      const int count = range_values[i + 1];
      if (start < 0 || count < 3 ||
          start + count > static_cast<int>(points.size())) {
        continue;
      }
      moto::ble::MapBuildingFootprint building;
      building.building_class =
          static_cast<moto::ble::MapBuildingClass>(class_values[i / 2]);
      for (int p = 0; p < count; ++p) {
        const auto& point = points[static_cast<std::size_t>(start + p)];
        building.points.push_back(
            {static_cast<std::int32_t>(std::llround(point.first * 1e6)),
             static_cast<std::int32_t>(std::llround(point.second * 1e6))});
      }
      scene.buildings.push_back(std::move(building));
    }
  }

  const BytesResult result = Encode(Message{scene});
  if (!result.ok()) {
    Throw(env, "encode map scene failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeMediaState(
    JNIEnv* env, jclass /*clazz*/, jint flags, jlong track_token,
    jint position_s, jint duration_s, jstring source_name, jstring track_title,
    jstring artist_name) {
  moto::ble::MediaState state;
  state.flags = static_cast<std::uint8_t>(flags);
  state.track_token = static_cast<std::uint32_t>(track_token);
  state.position_s = static_cast<std::uint16_t>(position_s);
  state.duration_s = static_cast<std::uint16_t>(duration_s);
  auto read = [env](jstring value) {
    std::string out;
    if (value == nullptr) return out;
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (text != nullptr) {
      out = text;
      env->ReleaseStringUTFChars(value, text);
    }
    return out;
  };
  state.source_name = read(source_name);
  state.track_title = read(track_title);
  state.artist_name = read(artist_name);
  const BytesResult result = Encode(Message{state});
  if (!result.ok()) {
    Throw(env, "encode media state failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

// --- UI layout codec (docs/NAV_LAYOUT_EDITOR_PLAN.md §5.3/§5.4) ------------

// fixed[15] = [layoutRevision, flags, themeId, mapHeightPx, mapOpacity,
//              routeRgb, routeWidthPx, routeOpacity, streetRgb, streetWidthPx,
//              streetOpacity, buildingOpacity, layerMask, zoomPercent,
//              componentCount]
// components[componentCount * 8] = [id, cflags, size, opacity, x, y, anchor,
//                                   spanDeg] per component.
// routeRgb/streetRgb are packed 0xRRGGBB; an ARGB 0xFFRRGGBB value is accepted.
JNIEXPORT jbyteArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_encodeUiLayout(
    JNIEnv* env, jobject /*self*/, jintArray fixed, jintArray components) {
  if (fixed == nullptr || components == nullptr ||
      env->GetArrayLength(fixed) < 15) {
    Throw(env, "encode ui layout failed: ",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  const jsize fixed_size = env->GetArrayLength(fixed);
  std::vector<jint> fixed_values(static_cast<std::size_t>(fixed_size));
  env->GetIntArrayRegion(fixed, 0, fixed_size, fixed_values.data());

  const jint component_count = fixed_values[14];
  const jsize component_values = env->GetArrayLength(components);
  if (component_count < 0 ||
      component_count >
          static_cast<jint>(moto::ble::kMaxUiLayoutComponents) ||
      component_values != component_count * 8) {
    Throw(env, "encode ui layout component array mismatch: ",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  std::vector<jint> component_ints(
      static_cast<std::size_t>(component_values));
  if (component_values > 0) {
    env->GetIntArrayRegion(components, 0, component_values,
                           component_ints.data());
  }

  const auto as_byte = [env](jint value, const char* field,
                             std::uint8_t* out) {
    if (value < 0 || value > 0xFF) {
      Throw(env, field, moto::ble::Error::InvalidArgument);
      return false;
    }
    *out = static_cast<std::uint8_t>(value);
    return true;
  };
  const auto as_int16 = [env](jint value, const char* field,
                              std::int16_t* out) {
    if (value < -32768 || value > 32767) {
      Throw(env, field, moto::ble::Error::InvalidArgument);
      return false;
    }
    *out = static_cast<std::int16_t>(value);
    return true;
  };

  UiLayout layout;
  if (fixed_values[0] < 0 || fixed_values[0] > 0xFFFF) {
    Throw(env, "encode ui layout revision: ",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  layout.layout_revision = static_cast<std::uint16_t>(fixed_values[0]);
  // map_height_px is u16 (docs §5.3, u16 revision): 160..466, even.
  if (fixed_values[3] < 160 || fixed_values[3] > 466 ||
      (fixed_values[3] & 1) != 0) {
    Throw(env, "encode ui layout map height: ",
          moto::ble::Error::InvalidArgument);
    return nullptr;
  }
  layout.map_height_px = static_cast<std::uint16_t>(fixed_values[3]);
  const std::uint32_t route_rgb =
      static_cast<std::uint32_t>(fixed_values[5]);
  layout.route_r = static_cast<std::uint8_t>((route_rgb >> 16U) & 0xFFU);
  layout.route_g = static_cast<std::uint8_t>((route_rgb >> 8U) & 0xFFU);
  layout.route_b = static_cast<std::uint8_t>(route_rgb & 0xFFU);
  const std::uint32_t street_rgb =
      static_cast<std::uint32_t>(fixed_values[8]);
  layout.street_r = static_cast<std::uint8_t>((street_rgb >> 16U) & 0xFFU);
  layout.street_g = static_cast<std::uint8_t>((street_rgb >> 8U) & 0xFFU);
  layout.street_b = static_cast<std::uint8_t>(street_rgb & 0xFFU);
  if (!as_byte(fixed_values[1], "encode ui layout flags: ", &layout.flags) ||
      !as_byte(fixed_values[2], "encode ui layout theme: ", &layout.theme) ||
      !as_byte(fixed_values[4], "encode ui layout map opacity: ",
               &layout.map_opacity) ||
      !as_byte(fixed_values[6], "encode ui layout route width: ",
               &layout.route_width_px) ||
      !as_byte(fixed_values[7], "encode ui layout route opacity: ",
               &layout.route_opacity) ||
      !as_byte(fixed_values[9], "encode ui layout street width: ",
               &layout.street_width_px) ||
      !as_byte(fixed_values[10], "encode ui layout street opacity: ",
               &layout.street_opacity) ||
      !as_byte(fixed_values[11], "encode ui layout building opacity: ",
               &layout.building_opacity) ||
      !as_byte(fixed_values[12], "encode ui layout layer mask: ",
               &layout.layer_mask) ||
      !as_byte(fixed_values[13], "encode ui layout zoom: ",
               &layout.zoom_percent)) {
    return nullptr;
  }

  layout.components.reserve(static_cast<std::size_t>(component_count));
  for (jint i = 0; i < component_count; ++i) {
    const jint* values = component_ints.data() + i * 8;
    UiLayoutComponent component;
    if (!as_byte(values[0], "encode ui layout component id: ",
                 &component.id) ||
        !as_byte(values[1], "encode ui layout component flags: ",
                 &component.flags) ||
        !as_byte(values[2], "encode ui layout component size: ",
                 &component.size) ||
        !as_byte(values[3], "encode ui layout component opacity: ",
                 &component.opacity) ||
        !as_int16(values[4], "encode ui layout component x: ",
                  &component.x) ||
        !as_int16(values[5], "encode ui layout component y: ",
                  &component.y) ||
        !as_byte(values[6], "encode ui layout component anchor: ",
                 &component.anchor) ||
        !as_byte(values[7], "encode ui layout component span: ",
                 &component.span_deg)) {
      return nullptr;
    }
    layout.components.push_back(component);
  }

  const BytesResult result = Encode(Message{layout});
  if (!result.ok()) {
    Throw(env, "encode ui layout failed: ", result.error);
    return nullptr;
  }
  return MakeJByteArray(env, result.value);
}

// @return [revision, layoutRevision, source, supportedMask, flags, reserved]
JNIEXPORT jintArray JNICALL
Java_dev_glimpse_motogps_NativeBridge_decodeUiLayoutState(
    JNIEnv* env, jobject /*self*/, jbyteArray payload) {
  Bytes buffer;
  ReadBytes(env, payload, &buffer);
  const moto::ble::MessageResult result = moto::ble::decode_message(
      moto::ble::MessageType::UiLayoutState,
      ByteView{buffer.data(), buffer.size()});
  if (!result.ok()) {
    Throw(env, "decode ui layout state failed: ", result.error);
    return nullptr;
  }
  const auto& state = std::get<moto::ble::UiLayoutState>(result.value);
  jint values[6] = {static_cast<jint>(moto::ble::kPayloadRevision),
                    static_cast<jint>(state.layout_revision),
                    static_cast<jint>(state.source),
                    static_cast<jint>(state.supported_mask),
                    static_cast<jint>(state.flags),
                    static_cast<jint>(state.reserved)};
  jintArray out = env->NewIntArray(6);
  env->SetIntArrayRegion(out, 0, 6, values);
  return out;
}

}  // extern "C"
