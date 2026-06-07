#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <chrono>
#include <string>
#include <thread>

#include "broker.pb.h"
#include "messagekeys.h"
#include "safequeue.h"
#include "zmqworker.h"

namespace TestSupport {

// A high, unusual port so test runs don't collide with a broker the developer
// might already have running locally on the default 5555/5556.
inline const std::string& testBrokerAddress() {
  static const std::string addr = "tcp://127.0.0.1:25555";
  return addr;
}

// Blocks until `queue` yields a value or `timeout` elapses, polling rather than
// using SafeQueue::pop() so a hung broker fails the test instead of the run.
template <typename T>
bool popWithTimeout(SafeQueue<T>& queue, T& out, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (queue.try_pop(out)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

// A brand new client identity is always met with a RESET that swallows
// whatever message triggered it (see the "newClient" branch in
// ZmqBroker::processMessage). Real clients clear this automatically via
// ConnectionManager's CONNECT-on-connect handshake; the raw ZmqWorkers used in
// these tests have to do it explicitly before the broker will act on anything
// else they send.
inline void completeHandshake(ZmqWorker& worker, const std::string& clientId) {
  broker::BrokerPayload connect;
  connect.set_handler_key(Keys::CONNECT);
  connect.set_sender_id(clientId);
  connect.set_topic("");
  worker.writeControlMessage(connect);
}

inline void subscribe(ZmqWorker& worker, const std::string& clientId, const std::string& topic) {
  broker::BrokerPayload sub;
  sub.set_handler_key(Keys::SUBSCRIBE);
  sub.set_sender_id(clientId);
  sub.set_topic(topic);
  worker.writeControlMessage(sub);
}

}  // namespace TestSupport

#endif  // TEST_HELPERS_H
