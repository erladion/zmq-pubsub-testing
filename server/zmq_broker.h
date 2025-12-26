#ifndef ZMQBROKER_H
#define ZMQBROKER_H

#include <zmq.hpp>
#include <thread>
#include <unordered_map>
#include <set>
#include <mutex>
#include "broker.pb.h"

struct ClientState {
  std::string identity; // ZMQ Routing ID
  std::set<std::string> subscriptions;
  std::chrono::steady_clock::time_point lastSeen;
};

class ZmqBroker {
  const std::chrono::seconds CLIENT_TIMEOUT{10};

public:
  ZmqBroker();
  ~ZmqBroker();

  void start(const std::vector<std::string> &bindAddresses);
  void stop();

private:
  void run(const std::vector<std::string>& addresses);
  void handleMessage(const std::string& senderId, const broker::BrokerPayload& msg);

  bool m_running;
  std::thread m_brokerThread;
  zmq::context_t m_context;

  // Track subscriptions
  std::mutex m_stateMutex;
  std::unordered_map<std::string, ClientState> m_clients;
};

#endif
