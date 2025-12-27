#include "zmqworker.h"

#include "logger.h"

#include <iostream>

ZmqWorker::ZmqWorker(const ConnectionConfig& config, SafeQueue<broker::BrokerPayload>* inboundQueue, StatusCallback statusCb)
    : m_config(config), m_inboundQueue(inboundQueue), m_statusCallback(statusCb), m_running(false), m_context(1) {}

ZmqWorker::~ZmqWorker() {
  stop();
}

void ZmqWorker::start() {
  m_running = true;
  m_workerThread = std::thread(&ZmqWorker::run, this);
}

void ZmqWorker::stop() {
  m_running = false;
  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }
}

bool ZmqWorker::writeMessage(const broker::BrokerPayload& msg) {
  m_outboundQueue.push(msg);
  return true;
}

bool ZmqWorker::writeControlMessage(const broker::BrokerPayload& msg) {
  m_controlQueue.push(msg);
  return true;
}

void ZmqWorker::setMessageCallback(MessageCallback callback) {
  m_messageCallback = callback;
}

void ZmqWorker::run() {
  zmq::socket_t socket(m_context, ZMQ_DEALER);
  socket.set(zmq::sockopt::linger, 0);
  socket.set(zmq::sockopt::routing_id, m_config.clientId);
  socket.connect(m_config.address);

  m_isOnline = false;
  m_lastRxTime = std::chrono::steady_clock::now();
  const auto SERVER_TIMEOUT = std::chrono::seconds(10);

  if (m_statusCallback) {
    m_statusCallback(true);
  }

  auto pollTimeout = std::chrono::milliseconds(20);
  auto lastHeartbeat = std::chrono::steady_clock::now();
  const auto HEARTBEAT_INTERVAL = std::chrono::seconds(3);

  while (m_running) {
    zmq::pollitem_t items[] = {{socket.handle(), 0, ZMQ_POLLIN, 0}};

    zmq::poll(items, 1, pollTimeout);

    if (items[0].revents & ZMQ_POLLIN) {
      zmq::message_t msg;
      if (socket.recv(msg, zmq::recv_flags::none)) {
        m_lastRxTime = std::chrono::steady_clock::now();

        if (!m_isOnline) {
          m_isOnline = true;
          if (m_statusCallback) {
            m_statusCallback(true);
          }
        }

        broker::BrokerPayload payload;
        if (payload.ParseFromArray(msg.data(), msg.size())) {
          if (payload.handler_key() == "__HEARTBEAT_ACK__") {
          } else if (m_inboundQueue) {
            m_inboundQueue->push(payload);
          } else if (m_messageCallback) {
            m_messageCallback(payload);
          }
        } else {
          Logger::Log(Logger::ERROR, "Failed to parse Protobuf message! Dropping packet.");
        }
      }
    }

    broker::BrokerPayload outbound;

    while (m_controlQueue.try_pop(outbound)) {
      std::string data = outbound.SerializeAsString();
      zmq::message_t zMsg(data.begin(), data.end());
      socket.send(zMsg, zmq::send_flags::none);
    }

    int messagesSent = 0;
    while (messagesSent < 100 && m_outboundQueue.try_pop(outbound)) {
      std::string data = outbound.SerializeAsString();
      zmq::message_t zMsg(data.begin(), data.end());
      socket.send(zMsg, zmq::send_flags::none);
      messagesSent++;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeat > HEARTBEAT_INTERVAL) {
      broker::BrokerPayload hb;
      hb.set_handler_key("__HEARTBEAT__");
      hb.set_sender_id(m_config.clientId);
      hb.set_topic("");  // No topic needed

      std::string data = hb.SerializeAsString();
      zmq::message_t zMsg(data.begin(), data.end());
      socket.send(zMsg, zmq::send_flags::none);

      lastHeartbeat = now;
    }

    if (m_isOnline && (now - m_lastRxTime > SERVER_TIMEOUT)) {
      Logger::Log(Logger::ERROR, "Server timed out! Switching to OFFLINE.");
      m_isOnline = false;
      if (m_statusCallback) {
        m_statusCallback(false);
      }
    }
  }

  socket.close();
}
