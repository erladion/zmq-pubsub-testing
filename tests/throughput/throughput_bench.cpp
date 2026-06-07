// Standalone load generator for the broker mesh: spins up a real in-process
// ZmqBroker plus a configurable number of publisher/subscriber ZmqWorkers,
// drives sustained pub/sub traffic over a fixed measurement window, and
// reports throughput (msgs/sec, MB/sec) and end-to-end latency percentiles.
//
// This is a benchmark, not a correctness check - it always exits 0 and prints
// a report. It is intentionally NOT registered with gtest_discover_tests, so
// it stays out of the normal `ctest` run. Invoke it directly, e.g.:
//
//   ./tests/throughput_bench --publishers 4 --subscribers 4 --duration-secs 10

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "broker.pb.h"
#include "config.h"
#include "safequeue.h"
#include "zmqbroker.h"
#include "zmqworker.h"

#include "support/test_helpers.h"

using namespace std::chrono_literals;
using TestSupport::completeHandshake;
using TestSupport::subscribe;
using TestSupport::testBrokerAddress;

namespace {

const std::string kTopic = "throughput-bench";
constexpr int kMinPayloadBytes = static_cast<int>(sizeof(int64_t));
constexpr auto kDrainGrace = 2s;

// Messages are tagged at send-time (via transfer_id) with which phase produced
// them. This is what lets publish- and delivery-side counts line up correctly:
// deciding whether to count a message based on the *subscriber's* clock at
// receive-time would be racy (a message generated a microsecond before the
// measurement window opens can easily arrive a microsecond after it does,
// inflating "received" beyond "sent" and corrupting the delivered-ratio and
// latency numbers - this is exactly what an earlier draft of this benchmark
// got wrong).
const std::string kWarmupTag = "warmup";
const std::string kMeasureTag = "measure";

struct BenchConfig {
  int publisherCount = 2;
  int subscriberCount = 2;
  int payloadBytes = 256;
  std::chrono::seconds warmup{1};
  std::chrono::seconds duration{5};
};

void printUsage() {
  std::cout << "Usage: throughput_bench [options]\n"
            << "  --publishers N      number of publisher clients   (default 2)\n"
            << "  --subscribers N     number of subscriber clients  (default 2)\n"
            << "  --payload-bytes N   message payload size in bytes (default 256, min " << kMinPayloadBytes << ")\n"
            << "  --warmup-secs N     unmeasured warmup duration    (default 1)\n"
            << "  --duration-secs N   measured run duration         (default 5)\n";
}

BenchConfig parseArgs(int argc, char** argv) {
  BenchConfig config;
  auto intArg = [&](int& i) {
    if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + argv[i]);
    return std::stoi(argv[++i]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--publishers") config.publisherCount = intArg(i);
    else if (flag == "--subscribers") config.subscriberCount = intArg(i);
    else if (flag == "--payload-bytes") config.payloadBytes = intArg(i);
    else if (flag == "--warmup-secs") config.warmup = std::chrono::seconds(intArg(i));
    else if (flag == "--duration-secs") config.duration = std::chrono::seconds(intArg(i));
    else if (flag == "--help" || flag == "-h") {
      printUsage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown flag: " + flag);
    }
  }

  if (config.publisherCount < 1 || config.subscriberCount < 1) {
    throw std::runtime_error("--publishers and --subscribers must be >= 1");
  }
  if (config.payloadBytes < kMinPayloadBytes) {
    throw std::runtime_error("--payload-bytes must be >= " + std::to_string(kMinPayloadBytes) + " (a send timestamp is embedded in the payload)");
  }
  return config;
}

// The benchmark runs entirely in-process, so steady_clock is consistent across
// publisher and subscriber threads - embed the send time directly in the
// (otherwise opaque) payload bytes and diff against it on receipt to measure
// end-to-end latency without any external clock-sync machinery.
std::string makeTimestampedPayload(int size) {
  std::string data(static_cast<std::size_t>(size), '\0');
  const int64_t sendNanos = std::chrono::steady_clock::now().time_since_epoch().count();
  std::memcpy(data.data(), &sendNanos, sizeof(sendNanos));
  return data;
}

std::chrono::nanoseconds latencySince(const broker::BrokerPayload& msg) {
  int64_t sendNanos = 0;
  std::memcpy(&sendNanos, msg.raw_data().data(), sizeof(sendNanos));
  const auto sendTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sendNanos));
  return std::chrono::steady_clock::now() - sendTime;
}

// Probes the broker with uniquely-tagged throwaway messages until every
// subscriber has seen one - re-issuing SUBSCRIBE periodically - then drains
// each inbound queue. This mirrors the synchronization in
// any_payload_roundtrip_test.cpp: a plain "did *a* probe arrive" check isn't
// enough, since a stale probe from an earlier retry can land late and get
// mistaken for real load traffic once the measurement window starts.
bool waitForSubscriptionsActive(ZmqWorker& probePublisher, const std::string& probeSenderId, std::vector<std::unique_ptr<ZmqWorker>>& subscribers,
                                std::vector<std::unique_ptr<SafeQueue<broker::BrokerPayload>>>& inboundQueues, const std::vector<std::string>& subscriberIds) {
  for (int attempt = 0; attempt < 60; ++attempt) {
    const std::string nonce = "ready-" + std::to_string(attempt);

    broker::BrokerPayload probe;
    probe.set_handler_key("PROBE");
    probe.set_sender_id(probeSenderId);
    probe.set_topic(kTopic);
    probe.set_raw_data(nonce);
    probePublisher.writeMessage(probe);

    std::vector<bool> seen(subscribers.size(), false);
    const auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < deadline) {
      for (std::size_t i = 0; i < inboundQueues.size(); ++i) {
        broker::BrokerPayload received;
        while (inboundQueues[i]->try_pop(received)) {
          if (received.handler_key() == "PROBE" && received.raw_data() == nonce) {
            seen[i] = true;
          }
        }
      }
      if (std::all_of(seen.begin(), seen.end(), [](bool b) { return b; })) {
        // Let any stragglers (late probes from earlier attempts, handshake
        // RESETs, ...) land, then wipe the slate clean before measuring.
        std::this_thread::sleep_for(250ms);
        for (auto& queue : inboundQueues) {
          broker::BrokerPayload discard;
          while (queue->try_pop(discard)) {
          }
        }
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }

    if (attempt % 5 == 4) {
      for (std::size_t i = 0; i < subscribers.size(); ++i) {
        subscribe(*subscribers[i], subscriberIds[i], kTopic);
      }
    }
  }
  return false;
}

struct PublisherResult {
  uint64_t messagesSent = 0;
  uint64_t enqueueFailures = 0;
};

// Publishes as fast as the worker's outbound queue allows - writeMessage's
// SafeQueue backpressure paces this naturally - for as long as `running` stays
// true, counting only what happens while `recording` is true so warmup/drain
// traffic doesn't skew the reported numbers.
PublisherResult runPublisher(ZmqWorker& worker, std::string senderId, int payloadBytes, const std::atomic<bool>& running, const std::atomic<bool>& recording) {
  PublisherResult result;
  int32_t sequence = 0;

  while (running.load(std::memory_order_relaxed)) {
    // Read once and reuse for both the tag and the counters, so a message is
    // never tagged "measure" without also being counted as sent (or vice versa).
    const bool measuring = recording.load(std::memory_order_relaxed);

    broker::BrokerPayload msg;
    msg.set_handler_key("LOAD");
    msg.set_sender_id(senderId);
    msg.set_topic(kTopic);
    msg.set_sequence_number(sequence++);
    msg.set_transfer_id(measuring ? kMeasureTag : kWarmupTag);
    msg.set_raw_data(makeTimestampedPayload(payloadBytes));

    const bool enqueued = worker.writeMessage(msg);
    if (measuring) {
      enqueued ? ++result.messagesSent : ++result.enqueueFailures;
    }
  }
  return result;
}

// One accepted "measure"-tagged delivery: when it landed (so the report can
// tell "arrived during the timed window" apart from "arrived during drain"),
// its end-to-end latency, and its serialized size.
struct ReceivedSample {
  int64_t receiveTimeNanos;
  int64_t latencyNanos;
  uint32_t bytes;
};

struct SubscriberResult {
  std::vector<ReceivedSample> samples;
};

// Drains the inbound queue with a blocking pop() (woken by queue.stop() when
// the run ends) so idle polling doesn't add jitter to the latency numbers.
// Only "measure"-tagged LOAD messages are recorded - see the kWarmupTag /
// kMeasureTag comment for why that decision is made by the sender, not here.
SubscriberResult runSubscriber(SafeQueue<broker::BrokerPayload>& queue) {
  SubscriberResult result;
  broker::BrokerPayload msg;

  while (queue.pop(msg)) {
    if (msg.handler_key() != "LOAD" || msg.transfer_id() != kMeasureTag) {
      continue;
    }
    result.samples.push_back({std::chrono::steady_clock::now().time_since_epoch().count(), latencySince(msg).count(), static_cast<uint32_t>(msg.ByteSizeLong())});
  }
  return result;
}

double percentileMs(const std::vector<int64_t>& sortedNanos, double p) {
  if (sortedNanos.empty()) return 0.0;
  const auto idx = static_cast<std::size_t>(p * static_cast<double>(sortedNanos.size() - 1));
  return static_cast<double>(sortedNanos[idx]) / 1e6;
}

void printReport(const BenchConfig& config, std::chrono::steady_clock::time_point measureStart, std::chrono::steady_clock::time_point measureEnd,
                 const std::vector<PublisherResult>& publisherResults, const std::vector<SubscriberResult>& subscriberResults) {
  uint64_t totalSent = 0, totalEnqueueFailures = 0;
  for (const auto& r : publisherResults) {
    totalSent += r.messagesSent;
    totalEnqueueFailures += r.enqueueFailures;
  }

  // "In-window" = arrived while publishers were still actively sending, i.e.
  // directly comparable to the publish-side rate over the same wall-clock
  // span. "Eventually" additionally counts arrivals during the post-window
  // drain grace period, which is what the delivered-ratio below needs - a
  // message sent at T-1ms legitimately arrives a few ms after T.
  const int64_t windowStartNanos = measureStart.time_since_epoch().count();
  const int64_t windowEndNanos = measureEnd.time_since_epoch().count();

  uint64_t inWindowReceived = 0, inWindowBytes = 0;
  uint64_t eventuallyReceived = 0;
  std::vector<int64_t> latencyNanos;
  for (const auto& r : subscriberResults) {
    for (const auto& sample : r.samples) {
      ++eventuallyReceived;
      latencyNanos.push_back(sample.latencyNanos);
      if (sample.receiveTimeNanos >= windowStartNanos && sample.receiveTimeNanos <= windowEndNanos) {
        ++inWindowReceived;
        inWindowBytes += sample.bytes;
      }
    }
  }
  std::sort(latencyNanos.begin(), latencyNanos.end());

  const double seconds = std::chrono::duration<double>(measureEnd - measureStart).count();
  const uint64_t expectedReceived = totalSent * static_cast<uint64_t>(config.subscriberCount);
  const double deliveredRatio = expectedReceived == 0 ? 0.0 : 100.0 * static_cast<double>(eventuallyReceived) / static_cast<double>(expectedReceived);

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "\n===== Throughput benchmark report =====\n";
  std::cout << config.publisherCount << " publisher(s), " << config.subscriberCount << " subscriber(s), ~" << config.payloadBytes << " B payload, " << seconds
            << " s measured (after " << config.warmup.count() << " s warmup, plus a " << kDrainGrace.count() << " s drain grace before tallying delivery)\n\n";

  std::cout << "Publish side:\n";
  std::cout << "  enqueued:           " << totalSent << " messages (" << totalEnqueueFailures << " backpressure drops)\n";
  std::cout << "  throughput:         " << (seconds > 0 ? static_cast<double>(totalSent) / seconds : 0.0) << " msgs/sec\n\n";

  std::cout << "Delivery side (fan-out to " << config.subscriberCount << " subscriber(s)):\n";
  std::cout << "  in-window:          " << inWindowReceived << " messages, "
            << (seconds > 0 ? static_cast<double>(inWindowReceived) / seconds : 0.0) << " msgs/sec, "
            << (seconds > 0 ? (static_cast<double>(inWindowBytes) / seconds) / (1024.0 * 1024.0) : 0.0) << " MB/sec\n";
  std::cout << "                      (arrived while publishers were still sending - directly comparable to the publish-side rate above;\n"
            << "                       lower than it means the mesh is falling behind and queueing a backlog)\n";
  std::cout << "  delivered overall:  " << eventuallyReceived << " / " << expectedReceived << " expected (" << deliveredRatio
            << "%, including the post-window drain)\n";
  std::cout << "  latency (ms):       p50=" << percentileMs(latencyNanos, 0.50) << "  p95=" << percentileMs(latencyNanos, 0.95)
            << "  p99=" << percentileMs(latencyNanos, 0.99) << "  max=" << (latencyNanos.empty() ? 0.0 : static_cast<double>(latencyNanos.back()) / 1e6) << "\n";
  std::cout << "========================================\n";
}

}  // namespace

int main(int argc, char** argv) {
  BenchConfig config;
  try {
    config = parseArgs(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n\n";
    printUsage();
    return 1;
  }

  ZmqBroker broker;
  broker.start({testBrokerAddress()});
  std::this_thread::sleep_for(100ms);

  std::vector<std::unique_ptr<SafeQueue<broker::BrokerPayload>>> inboundQueues;
  std::vector<std::unique_ptr<ZmqWorker>> subscribers;
  std::vector<std::string> subscriberIds;
  for (int i = 0; i < config.subscriberCount; ++i) {
    const std::string clientId = "bench-subscriber-" + std::to_string(i);
    auto queue = std::make_unique<SafeQueue<broker::BrokerPayload>>();

    ConnectionConfig cfg;
    cfg.address = testBrokerAddress();
    cfg.clientId = clientId;
    auto worker = std::make_unique<ZmqWorker>(cfg, queue.get(), nullptr);
    worker->start();
    completeHandshake(*worker, clientId);
    subscribe(*worker, clientId, kTopic);

    subscriberIds.push_back(clientId);
    subscribers.push_back(std::move(worker));
    inboundQueues.push_back(std::move(queue));
  }

  std::vector<std::unique_ptr<ZmqWorker>> publishers;
  std::vector<std::string> publisherIds;
  for (int i = 0; i < config.publisherCount; ++i) {
    const std::string clientId = "bench-publisher-" + std::to_string(i);

    ConnectionConfig cfg;
    cfg.address = testBrokerAddress();
    cfg.clientId = clientId;
    auto worker = std::make_unique<ZmqWorker>(cfg, nullptr, nullptr);
    worker->start();
    completeHandshake(*worker, clientId);

    publisherIds.push_back(clientId);
    publishers.push_back(std::move(worker));
  }

  std::cout << "Waiting for the broker to start routing '" << kTopic << "' to " << config.subscriberCount << " subscriber(s)...\n";
  if (!waitForSubscriptionsActive(*publishers.front(), publisherIds.front(), subscribers, inboundQueues, subscriberIds)) {
    std::cerr << "Timed out waiting for subscriptions to become active - is the broker reachable on " << testBrokerAddress() << "?\n";
    return 1;
  }

  std::atomic<bool> running{true};
  std::atomic<bool> recording{false};

  std::vector<std::future<PublisherResult>> publisherFutures;
  for (int i = 0; i < config.publisherCount; ++i) {
    publisherFutures.push_back(
        std::async(std::launch::async, runPublisher, std::ref(*publishers[i]), publisherIds[i], config.payloadBytes, std::cref(running), std::cref(recording)));
  }

  std::vector<std::future<SubscriberResult>> subscriberFutures;
  for (int i = 0; i < config.subscriberCount; ++i) {
    subscriberFutures.push_back(std::async(std::launch::async, runSubscriber, std::ref(*inboundQueues[i])));
  }

  std::cout << "Warming up for " << config.warmup.count() << "s...\n";
  std::this_thread::sleep_for(config.warmup);

  std::cout << "Measuring for " << config.duration.count() << "s...\n";
  recording = true;
  const auto measureStart = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(config.duration);
  running = false;
  const auto measureEnd = std::chrono::steady_clock::now();

  // Publishers have stopped; give messages sent near the tail end of the
  // window time to actually arrive before we stop the subscriber threads, so
  // the delivered-ratio in the report isn't skewed by in-flight stragglers.
  std::this_thread::sleep_for(kDrainGrace);

  std::vector<PublisherResult> publisherResults;
  for (auto& f : publisherFutures) publisherResults.push_back(f.get());

  for (auto& queue : inboundQueues) queue->stop();
  std::vector<SubscriberResult> subscriberResults;
  for (auto& f : subscriberFutures) subscriberResults.push_back(f.get());

  printReport(config, measureStart, measureEnd, publisherResults, subscriberResults);

  for (auto& w : publishers) w->stop();
  for (auto& w : subscribers) w->stop();
  broker.stop();
  return 0;
}
