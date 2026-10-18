// Android JNI adapter over the shared moto::ble protocol and moto::coordinates
// libraries. This is a new Android-specific shim; no iOS/ObjC++ code is reused.
//
// The golden-vector function below rebuilds the exact fixtures used by
// tests/native/ble_protocol_tests.cpp through the same C++ encode_message /
// fragment_message calls, so an on-device instrumented test can compare the
// bytes with shared/protocol/fixtures/ble-navigation-v1.golden.txt.
#include <jni.h>

#include <string>
#include <vector>

#include "moto/ble_protocol/ble_protocol.hpp"
#include "moto_coordinates.hpp"

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
  if (!connection_payload.ok() || !navigation_payload.ok() ||
      !geometry_payload.ok() || !command_payload.ok()) {
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
  if (!connection_frames.ok() || !command_frames.ok()) {
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

}  // extern "C"
