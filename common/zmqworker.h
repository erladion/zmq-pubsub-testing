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
  void setMessageCallback(MessageCallback callback);

private:
  void run();

  ConnectionConfig m_config;
  SafeQueue<broker::BrokerPayload>* m_inboundQueue;
  StatusCallback m_statusCallback;
  MessageCallback m_messageCallback;

  std::atomic<bool> m_running;
  std::thread m_workerThread;

  // ZMQ Context must be thread-safe (usually one per process, but here one per worker is fine for isolation)
  zmq::context_t m_context;
  // Socket is NOT thread safe, accessed only in run()
  // We use a thread-safe queue to pass outgoing messages to the worker thread
  SafeQueue<broker::BrokerPayload> m_outboundQueue;
};

#endif  // ZMQWORKER_H
