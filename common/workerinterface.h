#ifndef WORKERINTERFACE_H
#define WORKERINTERFACE_H

#include <functional>
#include <string>

#include "broker.pb.h"

using WorkerMessageCallback = std::function<void(const broker::BrokerPayload&)>;
using WorkerStatusCallback = std::function<void(bool)>;

class WorkerInterface {
public:
  virtual ~WorkerInterface() = default;

  virtual void start() = 0;
  virtual void stop() = 0;

  virtual bool writeMessage(const broker::BrokerPayload& msg) = 0;
  virtual bool writeControlMessage(const broker::BrokerPayload& msg) = 0;

  virtual void setMessageCallback(WorkerMessageCallback cb) = 0;
};

#endif  // WORKERINTERFACE_H
