#ifndef MESSAGEKEYS_H
#define MESSAGEKEYS_H

#include <string_view>

namespace Keys {
// System Handshakes
constexpr std::string_view CONNECT = "__CONNECT__";
constexpr std::string_view RESET = "__RESET__";
constexpr std::string_view HEARTBEAT = "__HEARTBEAT__";
constexpr std::string_view HEARTBEAT_ACK = "__HEARTBEAT_ACK__";
constexpr std::string_view SUBSCRIBE = "__SUBSCRIBE__";

// File Transfer
constexpr std::string_view FILE_META = "__FILE_META__";
constexpr std::string_view FILE_CHUNK = "__CHUNK__";
constexpr std::string_view FILE_FOOTER = "__FILE_FOOTER__";

constexpr bool isSystemPacket(std::string_view key) {
  return key == HEARTBEAT || key == HEARTBEAT_ACK || key == CONNECT || key == RESET;
}

constexpr bool isControlMessage(std::string_view key) {
  return key == HEARTBEAT || key == SUBSCRIBE || key == CONNECT || key == RESET;
}

constexpr bool isFilePacket(std::string_view key) {
  return key == FILE_META || key == FILE_CHUNK || key == FILE_FOOTER;
}

}  // namespace Keys

#endif  // MESSAGEKEYS_H
