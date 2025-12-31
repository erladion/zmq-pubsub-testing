#ifndef ZMQWORKER_H
#define ZMQWORKER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <zmq.hpp>

#include "broker.pb.h"

#include "config.h"
#include "safequeue.h"
#include "workerinterface.h"

class ZmqWorker : public WorkerInterface {
public:
  ZmqWorker(const ConnectionConfig& config, SafeQueue<broker::BrokerPayload>* inboundQueue, WorkerStatusCallback statusCb);
  ~ZmqWorker();

  void start() override;
  void stop() override;
  bool writeMessage(const broker::BrokerPayload& msg) override;
  bool writeControlMessage(const broker::BrokerPayload& msg) override;
  void setMessageCallback(WorkerMessageCallback callback) override;

private:
  void run();
  void sendHeartbeat(zmq::socket_t& socket);

private:
  ConnectionConfig m_config;
  SafeQueue<broker::BrokerPayload>* m_inboundQueue;
  WorkerStatusCallback m_statusCallback;
  WorkerMessageCallback m_messageCallback;

  std::atomic<bool> m_running;
  std::thread m_workerThread;

  zmq::context_t m_context;

  SafeQueue<broker::BrokerPayload> m_controlQueue;
  SafeQueue<broker::BrokerPayload> m_outboundQueue;

  std::atomic<bool> m_isOnline;
  std::chrono::steady_clock::time_point m_lastRxTime;
};

#endif  // ZMQWORKER_H
