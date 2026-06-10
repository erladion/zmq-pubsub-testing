#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "safequeue.h"
#include "zmqbroker.h"
#include "zmqworker.h"

#include "support/test_helpers.h"

using namespace std::chrono_literals;
using TestSupport::completeHandshake;
using TestSupport::popWithTimeout;
using TestSupport::subscribe;
using TestSupport::testBrokerAddress;

namespace {
// Second endpoint for the peering test; testBrokerAddress() is the first one.
// Note both brokers race for the same ipc inspector socket - the loser just
// logs an error and runs without one, which is fine for these tests.
const std::string kPeerBrokerAddress = "tcp://127.0.0.1:25556";
}  // namespace

class WildcardSubscriptionTest : public ::testing::Test {
protected:
  void TearDown() override {
    for (auto& broker : m_brokers) {
      broker->stop();
    }
  }

  ZmqBroker& startBroker(const std::string& address) {
    m_brokers.push_back(std::make_unique<ZmqBroker>());
    m_brokers.back()->start({address});
    return *m_brokers.back();
  }

  std::vector<std::unique_ptr<ZmqBroker>> m_brokers;
};

// An empty-topic subscription is the broker's wildcard: the subscriber must
// receive messages published on topics it never explicitly subscribed to.
// This is the contract peer links are built on.
TEST_F(WildcardSubscriptionTest, EmptyTopicSubscriberReceivesEveryTopic) {
  startBroker(testBrokerAddress());

  SafeQueue<broker::BrokerPayload> inbound;
  ConnectionConfig subConfig;
  subConfig.address = testBrokerAddress();
  subConfig.clientId = "wildcard-subscriber";
  ZmqWorker subscriberWorker(subConfig, &inbound, nullptr);
  subscriberWorker.start();
  completeHandshake(subscriberWorker, subConfig.clientId);
  subscribe(subscriberWorker, subConfig.clientId, "");

  ConnectionConfig pubConfig;
  pubConfig.address = testBrokerAddress();
  pubConfig.clientId = "wildcard-publisher";
  ZmqWorker publisher(pubConfig, nullptr, nullptr);
  publisher.start();
  completeHandshake(publisher, pubConfig.clientId);

  // Re-publish until delivery: SUBSCRIBE is processed asynchronously by the
  // broker, so early publishes can race a not-yet-active subscription.
  broker::BrokerPayload received;
  bool got = false;
  for (int attempt = 0; attempt < 30 && !got; ++attempt) {
    broker::BrokerPayload msg;
    msg.set_handler_key("wildcard-data");
    msg.set_sender_id(pubConfig.clientId);
    msg.set_topic("some-arbitrary-topic");
    msg.set_raw_data("hello");
    publisher.writeMessage(msg);

    while (popWithTimeout(inbound, received, 300ms)) {
      if (received.topic() == "some-arbitrary-topic") {
        got = true;
        break;
      }
    }
  }

  ASSERT_TRUE(got) << "Wildcard (\"\") subscriber never received a message published on another topic";
  EXPECT_EQ(received.raw_data(), "hello");

  publisher.stop();
  subscriberWorker.stop();
}

// A client holding both a wildcard and an exact subscription for the same
// topic must still receive each message exactly once.
TEST_F(WildcardSubscriptionTest, OverlappingExactAndWildcardSubscriptionsDeliverOneCopy) {
  startBroker(testBrokerAddress());

  const std::string topic = "dup-check-topic";

  SafeQueue<broker::BrokerPayload> inbound;
  ConnectionConfig subConfig;
  subConfig.address = testBrokerAddress();
  subConfig.clientId = "overlap-subscriber";
  ZmqWorker subscriberWorker(subConfig, &inbound, nullptr);
  subscriberWorker.start();
  completeHandshake(subscriberWorker, subConfig.clientId);
  subscribe(subscriberWorker, subConfig.clientId, "");
  subscribe(subscriberWorker, subConfig.clientId, topic);

  ConnectionConfig pubConfig;
  pubConfig.address = testBrokerAddress();
  pubConfig.clientId = "overlap-publisher";
  ZmqWorker publisher(pubConfig, nullptr, nullptr);
  publisher.start();
  completeHandshake(publisher, pubConfig.clientId);

  // Each attempt carries a unique payload so a duplicate delivery of the
  // received attempt is distinguishable from a late delivery of an earlier one.
  broker::BrokerPayload received;
  std::string gotPayload;
  for (int attempt = 0; attempt < 30 && gotPayload.empty(); ++attempt) {
    broker::BrokerPayload msg;
    msg.set_handler_key("dup-check-data");
    msg.set_sender_id(pubConfig.clientId);
    msg.set_topic(topic);
    msg.set_raw_data("copy-check-" + std::to_string(attempt));
    publisher.writeMessage(msg);

    while (popWithTimeout(inbound, received, 300ms)) {
      if (received.topic() == topic) {
        gotPayload = received.raw_data();
        break;
      }
    }
  }

  ASSERT_FALSE(gotPayload.empty()) << "Subscriber never received the published message at all";

  int duplicates = 0;
  broker::BrokerPayload extra;
  while (popWithTimeout(inbound, extra, 500ms)) {
    if (extra.topic() == topic && extra.raw_data() == gotPayload) {
      duplicates++;
    }
  }
  EXPECT_EQ(duplicates, 0) << "Message was delivered more than once to a client with overlapping subscriptions";

  publisher.stop();
  subscriberWorker.stop();
}

// End-to-end peering in the direction that depends on the wildcard: broker A
// links to broker B, so messages published on B must flow through A's peer
// link (RESET handshake -> wildcard SUBSCRIBE -> forwarding) to a subscriber
// connected to A.
TEST_F(WildcardSubscriptionTest, PeerLinkForwardsRemoteMessagesToLocalSubscribers) {
  startBroker(testBrokerAddress());                       // broker B (remote)
  ZmqBroker& brokerA = startBroker(kPeerBrokerAddress);   // broker A (local)
  brokerA.connectToPeer(testBrokerAddress());

  const std::string topic = "peer-topic";

  SafeQueue<broker::BrokerPayload> inbound;
  ConnectionConfig subConfig;
  subConfig.address = kPeerBrokerAddress;
  subConfig.clientId = "peer-subscriber";
  ZmqWorker subscriberWorker(subConfig, &inbound, nullptr);
  subscriberWorker.start();
  completeHandshake(subscriberWorker, subConfig.clientId);
  subscribe(subscriberWorker, subConfig.clientId, topic);

  ConnectionConfig pubConfig;
  pubConfig.address = testBrokerAddress();
  pubConfig.clientId = "peer-publisher";
  ZmqWorker publisher(pubConfig, nullptr, nullptr);
  publisher.start();
  completeHandshake(publisher, pubConfig.clientId);

  // The peer link needs its own handshake with broker B (heartbeat -> RESET ->
  // wildcard SUBSCRIBE) before anything flows, hence the generous retry loop.
  broker::BrokerPayload received;
  bool got = false;
  for (int attempt = 0; attempt < 30 && !got; ++attempt) {
    broker::BrokerPayload msg;
    msg.set_handler_key("peer-data");
    msg.set_sender_id(pubConfig.clientId);
    msg.set_topic(topic);
    msg.set_raw_data("across-the-mesh");
    publisher.writeMessage(msg);

    while (popWithTimeout(inbound, received, 300ms)) {
      if (received.topic() == topic) {
        got = true;
        break;
      }
    }
  }

  ASSERT_TRUE(got) << "Message published on the remote broker never reached a subscriber on the linked broker";
  EXPECT_EQ(received.raw_data(), "across-the-mesh");

  publisher.stop();
  subscriberWorker.stop();
}
