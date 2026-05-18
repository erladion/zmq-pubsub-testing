#ifndef DATAMODEL_H
#define DATAMODEL_H

#include <cstdint>
#include <string>
#include "broker.pb.h"  // Your framework's envelope schema

struct InspectorPacket {
  std::string timestamp;
  std::string senderId;
  std::string key;
  std::string topic;
  size_t sizeBytes;

  // The exact bytes pulled off the wire
  std::string rawMemory;

  // The parsed envelope (contains the Any payload or raw bytes)
  broker::BrokerPayload parsedProto;
};

// Required so Qt can pass this custom struct through its Signal/Slot queue!
Q_DECLARE_METATYPE(InspectorPacket)

#endif