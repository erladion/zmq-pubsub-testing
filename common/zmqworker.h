#ifndef ZMQWORKER_H
#define ZMQWORKER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <zmq.hpp>
#include "broker.pb.h"  // Your existing proto
#include "safequeue.h"

struct ConnectionConfig {
  std::string address;  // e.g., "tcp://127.0.0.1:5555"
  std::string clientId;
};

class ZmqWorker {
public:
  using MessageCallback = std::function<void(const broker::BrokerPayload&)>;
  using StatusCallback = std::function<void(bool)>;

  ZmqWorker(const ConnectionConfig& config, SafeQueue<broker::BrokerPayload>* inboundQueue, StatusCallback statusCb);
  ~ZmqWorker();

  void start();
  void stop();
  bool writeMessage(const broker::BrokerPayload& msg);
  bool writeControlMessage(const broker::BrokerPayload& msg);
  void setMessageCallback(MessageCallback callback);

private:
  void run();

  ConnectionConfig m_config;
  SafeQueue<broker::BrokerPayload>* m_inboundQueue;
  StatusCallback m_statusCallback;
  MessageCallback m_messageCallback;

  std::atomic<bool> m_running;
  std::thread m_workerThread;

  zmq::context_t m_context;

  SafeQueue<broker::BrokerPayload> m_controlQueue;
  SafeQueue<broker::BrokerPayload> m_outboundQueue;

  bool m_isOnline = false;
  std::chrono::steady_clock::time_point m_lastRxTime;
};

#endif  // ZMQWORKER_H
