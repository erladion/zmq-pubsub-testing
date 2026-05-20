#ifndef DATAMODEL_H
#define DATAMODEL_H

#include <cstdint>
#include <string>

#include "broker.pb.h"

struct InspectorPacket {
  std::string timestamp;
  std::string senderId;
  std::string key;
  std::string topic;
  size_t sizeBytes;

  std::string rawMemory;

  broker::BrokerPayload parsedProto;
};

Q_DECLARE_METATYPE(InspectorPacket)

#endif