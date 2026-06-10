#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "zmqbroker.h"
#include "zmqworker.h"

#include "support/test_helpers.h"

using namespace std::chrono_literals;
using TestSupport::testBrokerAddress;

namespace {

// Runs stop() on a background thread and waits up to `deadline` for it to
// return, so a shutdown regression fails this test instead of hanging the
// whole suite. On failure the stuck thread is detached and the object leaked
// deliberately - destroying it would hang on the same join.
template <typename Stoppable>
bool stopWithin(std::unique_ptr<Stoppable>& target, std::chrono::milliseconds deadline) {
  std::atomic<bool> stopped{false};
  std::thread stopper([&target, &stopped] {
    target->stop();
    stopped = true;
  });

  const auto end = std::chrono::steady_clock::now() + deadline;
  while (!stopped && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(25ms);
  }

  if (!stopped) {
    stopper.detach();
    (void)target.release();
    return false;
  }

  stopper.join();
  return true;
}

}  // namespace

// A worker whose broker never answers fills its zmq send pipe up to the high
// water mark. Sends must be non-blocking (dontwait) beyond that point: a
// blocking send wedges the worker loop, and stop() then never joins the
// thread.
TEST(ShutdownTest, WorkerStopsPromptlyWithUnreachableBrokerAndFullSendPipe) {
  ConnectionConfig config;
  config.address = "tcp://127.0.0.1:25598";  // Nothing listening here
  config.clientId = "doomed-sender";

  auto worker = std::make_unique<ZmqWorker>(config, nullptr, nullptr);
  worker->start();

  // Far more messages than the default send HWM (1000) so the pipe is
  // guaranteed full while the connection can never be established.
  broker::BrokerPayload msg;
  msg.set_handler_key("flood");
  msg.set_sender_id(config.clientId);
  msg.set_topic("flood");
  msg.set_raw_data(std::string(512, 'x'));
  for (int i = 0; i < 3000; ++i) {
    worker->writeMessage(msg);
  }

  // Give the worker loop time to drain the queue into the capped zmq pipe.
  std::this_thread::sleep_for(300ms);

  EXPECT_TRUE(stopWithin(worker, 5s)) << "ZmqWorker::stop() hung - send pipe full and sends are blocking";
}

// Broker shutdown must stop the peer inbound queue before joining the peer
// workers; a worker blocked pushing into a full queue would otherwise hang
// the join forever. Filling the 5000-slot queue deterministically isn't
// practical here, so this pins the prompt-shutdown property for a broker
// with a live peer link plus the stop() ordering around the queue.
TEST(ShutdownTest, BrokerWithPeerLinkStopsPromptly) {
  auto remote = std::make_unique<ZmqBroker>();
  remote->start({testBrokerAddress()});

  auto local = std::make_unique<ZmqBroker>();
  local->start({"tcp://127.0.0.1:25597"});
  local->connectToPeer(testBrokerAddress());

  // Let the link finish its RESET/subscribe handshake before tearing down.
  std::this_thread::sleep_for(300ms);

  EXPECT_TRUE(stopWithin(local, 5s)) << "ZmqBroker::stop() hung with a live peer link";
  EXPECT_TRUE(stopWithin(remote, 5s)) << "ZmqBroker::stop() hung on the remote broker";
}
