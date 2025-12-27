#include "zmq_broker.h"

#include "logger.h"

#include <iostream>

ZmqBroker::ZmqBroker() : m_running(false), m_context(1) {}
ZmqBroker::~ZmqBroker() {
  stop();
}

void ZmqBroker::start(const std::vector<std::string>& bindAddresses) {
  m_running = true;
  m_brokerThread = std::thread(&ZmqBroker::run, this, bindAddresses);
}

void ZmqBroker::run(const std::vector<std::string>& addresses) {
  zmq::socket_t socket(m_context, ZMQ_ROUTER);
  socket.set(zmq::sockopt::linger, 0);
  socket.set(zmq::sockopt::router_mandatory, 1);

  for (const auto& addr : addresses) {
    try {
      socket.bind(addr);
      Logger::Log(Logger::INFO, "Bound to: " + addr);
    } catch (const zmq::error_t& e) {
      Logger::Log(Logger::ERROR, "Failed to bind to " + addr + " : " + e.what());
    }
  }

  auto lastCleanup = std::chrono::steady_clock::now();
  while (m_running) {
    zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, 1, std::chrono::milliseconds(20));

    if (items[0].revents & ZMQ_POLLIN) {
      zmq::message_t identity;
      socket.recv(identity, zmq::recv_flags::none);

      zmq::message_t payload;
      socket.recv(payload, zmq::recv_flags::none);

      while (socket.get(zmq::sockopt::rcvmore)) {
        zmq::message_t trash;
        socket.recv(trash, zmq::recv_flags::none);
      }

      std::string senderId = identity.to_string();

      broker::BrokerPayload msg;
      if (msg.ParseFromArray(payload.data(), payload.size())) {
        bool newClient = (m_clients.find(senderId) == m_clients.end());
        {
          std::lock_guard<std::mutex> lock(m_stateMutex);
          m_clients[senderId].identity = senderId;
          m_clients[senderId].lastSeen = std::chrono::steady_clock::now();
        }

        if (newClient) {
          Logger::Log(Logger::INFO, "New client: " + senderId + ". Requesting Subscription Reset");

          zmq::message_t outId(senderId.data(), senderId.size());

          broker::BrokerPayload resetMsg;
          resetMsg.set_handler_key("__RESET__");
          resetMsg.set_topic("");
          std::string resetData = resetMsg.SerializeAsString();
          zmq::message_t outData(resetData.begin(), resetData.end());

          try {
            socket.send(outId, zmq::send_flags::sndmore);
            socket.send(outData, zmq::send_flags::none);
          } catch (const zmq::error_t& e) {
            Logger::Log(Logger::ERROR, "⚠️ Failed to send RESET to " + senderId + " : " + e.what() + ". Retrying on next message");

            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_clients.erase(senderId);
          }
        }

        if (msg.handler_key() == "__CONNECT__") {
          std::lock_guard<std::mutex> lock(m_stateMutex);
          Logger::Log(Logger::INFO, "Client " + senderId + " Connected (Session Reset)");

          // Wipe old data to ensure a fresh start
          m_clients.erase(senderId);

          m_clients[senderId].identity = senderId;
          m_clients[senderId].lastSeen = std::chrono::steady_clock::now();
        } else if (msg.handler_key() == "__HEARTBEAT__") {
          zmq::message_t outId(senderId.data(), senderId.size());

          broker::BrokerPayload ack;
          ack.set_handler_key("__HEARTBEAT_ACK__");
          ack.set_topic("");
          std::string ackData = ack.SerializeAsString();
          zmq::message_t outData(ackData.begin(), ackData.end());

          try {
            socket.send(outId, zmq::send_flags::sndmore);
            socket.send(outData, zmq::send_flags::none);
          } catch (...) {
          }
        } else if (msg.handler_key() == "__SUBSCRIBE__") {
          std::lock_guard<std::mutex> lock(m_stateMutex);

          auto result = m_clients[senderId].subscriptions.insert(msg.topic());
          if (result.second) {
            Logger::Log(Logger::INFO, "Client " + senderId + " Subscribed to " + msg.topic());
          }
        } else {
          Logger::Log(Logger::INFO, "Broadcasting topic '" + msg.topic() + "' from " + senderId);
          std::lock_guard<std::mutex> lock(m_stateMutex);
          for (auto& [id, state] : m_clients) {
            if (id == senderId) {
              continue;
            }

            if (state.subscriptions.count(msg.topic())) {
              zmq::message_t outId(id.data(), id.size());
              zmq::message_t outData(payload.data(), payload.size());

              try {
                socket.send(outId, zmq::send_flags::sndmore);
                socket.send(outData, zmq::send_flags::none);
              } catch (const zmq::error_t& e) {
                state.lastSeen = std::chrono::steady_clock::time_point();
              }
            }
          }
        }
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - lastCleanup > std::chrono::seconds(2)) {  // Check every 2 seconds
      std::lock_guard<std::mutex> lock(m_stateMutex);

      for (auto it = m_clients.begin(); it != m_clients.end();) {
        auto elapsed = now - it->second.lastSeen;

        if (elapsed > CLIENT_TIMEOUT) {
          Logger::Log(Logger::INFO, "☠️ KILLED Zombie Client: " + it->first + " (Inactive for 10s)");
          it = m_clients.erase(it);
        } else {
          ++it;
        }
      }
      lastCleanup = now;
    }
  }
}

void ZmqBroker::stop() {
  m_running = false;
  if (m_brokerThread.joinable())
    m_brokerThread.join();
}
