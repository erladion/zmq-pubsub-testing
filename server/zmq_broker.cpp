#include "zmq_broker.h"
#include "zmq_common.h"

#ifdef _WIN32
#include <io.h>
#define unlink _unlink
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// --- HELPER FUNCTION: MANUAL MULTIPART RECEIVE ---
// Replaces zmq::recv_multipart to avoid dependency on zmq_addon.hpp
static bool receive_multipart_safe(zmq::socket_t& socket, std::vector<zmq::message_t>& msgs) {
  msgs.clear();
  while (true) {
    zmq::message_t msg;
    // Use auto to handle different return types (bool vs optional) across cppzmq versions
    auto res = socket.recv(msg, zmq::recv_flags::none);
    if (!res)
      return false;
    msgs.emplace_back(std::move(msg));

    // Check if there are more frames in this message
    if (!socket.get(zmq::sockopt::rcvmore))
      break;
  }
  return true;
}

ZmqBroker::ZmqBroker() : m_ctx(1), m_running(false) {}

ZmqBroker::~ZmqBroker() {
  stop();
}

void ZmqBroker::stop() {
  m_running = false;
  if (m_worker.joinable())
    m_worker.join();
}

void ZmqBroker::run(const std::vector<std::string>& addresses) {
  if (m_running)
    return;
  m_running = true;

  m_router = std::make_unique<zmq::socket_t>(m_ctx, zmq::socket_type::router);

  // Bind logic with Cleanup
  for (const auto& addr : addresses) {
    if (addr.find("ipc://") == 0) {
      std::string path = addr.substr(6);
      unlink(path.c_str());
    }
    m_router->bind(addr);
    std::cout << "[Broker] Bound to: " << addr << std::endl;

    // Fix permissions for UDS
    if (addr.find("ipc://") == 0) {
#ifndef _WIN32
      std::string path = addr.substr(6);
      chmod(path.c_str(), 0777);
#endif
    }
  }

  m_worker = std::thread(&ZmqBroker::brokerLoop, this);
}

void ZmqBroker::connectToPeer(const std::string& address) {
  std::lock_guard<std::mutex> lock(m_peerMutex);
  auto peer = std::make_unique<zmq::socket_t>(m_ctx, zmq::socket_type::dealer);

  // Set identity for the peer connection
  std::string id = "BrokerPeer-" + std::to_string((uintptr_t)peer.get());
  peer->set(zmq::sockopt::routing_id, id);

  peer->connect(address);
  m_peers.push_back(std::move(peer));
  std::cout << "[Broker] Connected to peer: " << address << std::endl;
}

void ZmqBroker::brokerLoop() {
  while (m_running) {
    // Poll both local Router and Peer sockets
    std::vector<zmq::pollitem_t> items;

    // Item 0: Local Router
    items.push_back({*m_router, 0, ZMQ_POLLIN, 0});

    // Items 1..N: Peers
    {
      std::lock_guard<std::mutex> lock(m_peerMutex);
      for (auto& p : m_peers) {
        items.push_back({*p, 0, ZMQ_POLLIN, 0});
      }
    }

    zmq::poll(items.data(), items.size(), std::chrono::milliseconds(100));

    // 1. Handle Local Clients
    if (items[0].revents & ZMQ_POLLIN) {
      std::vector<zmq::message_t> parts;
      // FIX: Use manual helper
      bool res = receive_multipart_safe(*m_router, parts);
      if (res)
        processMessage(parts, false);
    }

    // 2. Handle Peers
    std::lock_guard<std::mutex> lock(m_peerMutex);
    for (size_t i = 0; i < m_peers.size(); ++i) {
      if (items[i + 1].revents & ZMQ_POLLIN) {
        std::vector<zmq::message_t> parts;
        // FIX: Use manual helper
        bool res = receive_multipart_safe(*m_peers[i], parts);
        if (res)
          processMessage(parts, true);
      }
    }
  }
}

void ZmqBroker::processMessage(std::vector<zmq::message_t>& parts, bool fromPeer) {
  std::cout << __FUNCTION__ << std::endl;
  // Expected Format from Router: [Identity][Empty][UUID][Topic][Type][Payload]
  // Expected Format from Dealer(Peer): [UUID][Topic][Type][Payload]

  size_t offset = fromPeer ? 0 : 2;
  std::cout << parts.size() << std::endl;
  if (parts.size() < offset + 4)
    return;

  std::string identity = fromPeer ? "" : parts[0].to_string();
  std::string uuid = parts[offset + 0].to_string();
  std::string topic = parts[offset + 1].to_string();
  std::string type = parts[offset + 2].to_string();

  std::cout << topic << std::endl;

  // --- 1. DEDUPLICATION ---
  if (m_seenUuids.count(uuid))
    return;  // Loop detected
  m_seenUuids.insert(uuid);
  m_uuidHistory.push_back(uuid);
  if (m_uuidHistory.size() > 10000) {
    m_seenUuids.erase(m_uuidHistory.front());
    m_uuidHistory.pop_front();
  }

  // --- 2. HANDLE SUBSCRIPTION ---
  if (type == ZmqProtocol::TYPE_SUB && !fromPeer) {
    m_subscriptions[topic].insert(identity);
    std::cout << "[Broker] Client " << identity << " subscribed to " << topic << std::endl;
    return;  // Don't broadcast subscriptions
  }

  // --- 3. LOCAL DELIVERY ---
  if (m_subscriptions.count(topic)) {
    for (const auto& destId : m_subscriptions[topic]) {
      // Don't echo back to sender
      if (!fromPeer && destId == identity)
        continue;

      // Router Send: [DestID][Empty][UUID][Topic][Type][Payload]
      m_router->send(zmq::buffer(destId), zmq::send_flags::sndmore);
      m_router->send(zmq::message_t(), zmq::send_flags::sndmore);  // Delimiter

      // Re-send frames (Copy needed because ZMQ messages move ownership)
      for (size_t i = offset; i < parts.size(); ++i) {
        bool more = (i < parts.size() - 1);
        zmq::message_t copy;
        copy.copy(parts[i]);
        m_router->send(copy, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
      }
    }
  }

  // --- 4. BRIDGE FLOODING ---
  // Forward to all peers (Dealer sends: [UUID][Topic][Type][Payload])
  {
    std::lock_guard<std::mutex> lock(m_peerMutex);
    for (auto& peer : m_peers) {
      for (size_t i = offset; i < parts.size(); ++i) {
        bool more = (i < parts.size() - 1);
        zmq::message_t copy;
        copy.copy(parts[i]);
        peer->send(copy, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
      }
    }
  }
}
