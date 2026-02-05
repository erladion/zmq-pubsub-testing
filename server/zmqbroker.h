#ifndef ZMQBROKER_H
#define ZMQBROKER_H

#include <zmq.hpp>

#include <thread>
#include <unordered_map>
#include <set>
#include <mutex>
#include <atomic>
#include <deque>
#include <memory>

#include "zmqworker.h"
#include "safequeue.h"

#include "broker.pb.h"

struct ClientState {
  std::string identity; // ZMQ Routing ID
  std::set<std::string, std::less<>> subscriptions;
  std::chrono::steady_clock::time_point lastSeen;
};

class ZmqBroker {
  const std::chrono::seconds CLIENT_TIMEOUT{10};
  const size_t MAX_HISTORY_SIZE{10000};

public:
  ZmqBroker();
  ~ZmqBroker();

  void start(const std::vector<std::string> &bindAddresses);
  void stop();

  void connectToPeer(const std::string& peerAddress);

private:
  void run(const std::vector<std::string>& addresses);
  void processMessage(zmq::socket_t& socket, broker::BrokerPayload& msg, const std::string& senderId, bool isFromPeer);
  bool isDuplicate(const std::string& uuid);

  void broadcastStats(zmq::socket_t& socket);

private:
  std::atomic<bool> m_running;
  std::thread m_brokerThread;
  zmq::context_t m_context;

  std::string m_brokerId;

  // Track subscriptions
  std::mutex m_stateMutex;
  std::unordered_map<std::string, ClientState> m_clients;

  std::vector<std::unique_ptr<ZmqWorker>> m_peers;

  SafeQueue<broker::BrokerPayload> m_peerInboundQueue;

  std::mutex m_historyMutex;
  std::set<std::string> m_seenMessageIds;
  std::deque<std::string> m_messageIdOrder;

  // Stats
  std::chrono::steady_clock::time_point m_startTime;
  std::chrono::steady_clock::time_point m_lastStatsTime;

  uint64_t m_totalMessages = 0;
  uint64_t m_totalBytes = 0;

  // Interval counters (reset every second)
  uint64_t m_msgsInterval = 0;
  uint64_t m_bytesInterval = 0;
};

#endif
