#include <QCoreApplication>

#include "zmq_broker.h"

int main(int argc, char* argv[]) {
  QCoreApplication a(argc, argv);

  std::vector<std::string> bindings;
  bindings.push_back("tcp://*:5555");
  bindings.push_back("ipc:///tmp/broker.sock");

  ZmqBroker::instance().run(bindings);

  return a.exec();
}
