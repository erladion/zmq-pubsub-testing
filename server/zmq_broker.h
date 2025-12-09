#ifndef ZMQ_BROKER_H
#define ZMQ_BROKER_H

#include <zmq.hpp>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <vector>
#include <iostream>

class ZmqBroker {
public:
  static ZmqBroker& instance() {
    static ZmqBroker inst;
    return inst;
  }

  // Accepts ["tcp://0.0.0.0:5555", "ipc:///tmp/broker.sock"]
  void run(const std::vector<std::string>& addresses);
  void stop();

  // Connect to another Broker (Mesh)
  void connectToPeer(const std::string& address);

private:
  ZmqBroker();
  ~ZmqBroker();

  void brokerLoop();
  void processMessage(std::vector<zmq::message_t>& parts, bool fromPeer);

private:
  zmq::context_t m_ctx;
  std::unique_ptr<zmq::socket_t> m_router; // Local Clients

  // Mesh State
  std::mutex m_peerMutex;
  std::vector<std::unique_ptr<zmq::socket_t>> m_peers; // Remote Brokers

  std::atomic<bool> m_running;
  std::thread m_worker;

  // Pub/Sub State: Topic -> Set of Client Identities
  std::unordered_map<std::string, std::unordered_set<std::string>> m_subscriptions;

  // Deduplication History
  std::unordered_set<std::string> m_seenUuids;
  std::deque<std::string> m_uuidHistory;
};

#endif // ZMQ_BROKER_H
