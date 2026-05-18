#ifndef INSPECTORWORKER_H
#define INSPECTORWORKER_H

#include <QThread>
#include <atomic>
#include <zmq.hpp>
#include "datamodel.h"

class InspectorWorker : public QThread {
  Q_OBJECT

public:
  InspectorWorker(QObject* parent = nullptr) : QThread(parent), m_running(false) {}

  void stopWorker() { m_running = false; }

signals:
  // This signal safely crosses the thread boundary!
  void packetReceived(const InspectorPacket& packet);

protected:
  void run() override;

private:
  std::atomic<bool> m_running;
};

#endif