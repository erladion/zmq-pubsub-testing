#include "inspectorworker.h"

#include <chrono>
#include <iomanip>
#include <sstream>

#include "config.h"

std::string getCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);

  // Extract the millisecond fraction
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();

  return ss.str();
}

void InspectorWorker::run() {
  m_running = true;
  zmq::context_t ctx(1);
  zmq::socket_t inspector(ctx, ZMQ_SUB);
  inspector.set(zmq::sockopt::maxmsgsize, MAX_MESSAGE_SIZE_BYTES);
  inspector.connect("ipc:///tmp/broker_inspector.sock");
  inspector.set(zmq::sockopt::subscribe, "");

  while (m_running) {
    zmq::message_t msg;
    // Use dontwait so we can check m_running and exit cleanly
    if (inspector.recv(msg, zmq::recv_flags::dontwait)) {
      InspectorPacket p;
      p.rawMemory = std::string(static_cast<char*>(msg.data()), msg.size());

      if (p.parsedProto.ParseFromString(p.rawMemory)) {
        p.timestamp = getCurrentTimestamp();
        p.senderId = p.parsedProto.sender_id();
        p.key = p.parsedProto.handler_key();
        p.topic = p.parsedProto.topic();
        p.sizeBytes = msg.size();

        emit packetReceived(p);
      }
    } else {
      QThread::msleep(10);
    }
  }
}