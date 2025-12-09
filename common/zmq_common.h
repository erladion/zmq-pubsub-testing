#ifndef ZMQ_COMMON_H
#define ZMQ_COMMON_H

#include <string>
#include <vector>
#include <zmq.hpp>

// WIRE FORMAT (Multipart):
// Frame 0: Identity (Added by Router, stripped by Dealer)
// Frame 1: Empty (Delimiter)
// Frame 2: Message UUID (For Mesh Deduplication)
// Frame 3: Topic (e.g., "chat", "file_upload")
// Frame 4: Message Type ("SUB", "MSG", "FILE_META", "CHUNK", "FILE_FOOTER")
// Frame 5: Payload (JSON or Raw Bytes)

namespace ZmqProtocol {
const std::string TYPE_SUB = "SUB";
const std::string TYPE_MSG = "MSG";
const std::string TYPE_FILE_META = "FILE_META";
const std::string TYPE_CHUNK = "CHUNK";
const std::string TYPE_FILE_FOOTER = "FILE_FOOTER";

// Helper to build a multipart message
static std::vector<zmq::message_t> buildMessage(const std::string& uuid,
                                                const std::string& topic,
                                                const std::string& type,
                                                const void* data,
                                                size_t size) {
  std::vector<zmq::message_t> msgs;
  msgs.emplace_back(uuid.data(), uuid.size());
  msgs.emplace_back(topic.data(), topic.size());
  msgs.emplace_back(type.data(), type.size());
  msgs.emplace_back(data, size);  // Copies data
  return msgs;
}
}  // namespace ZmqProtocol

#endif  // ZMQ_COMMON_H
