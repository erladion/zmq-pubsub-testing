#include <QCoreApplication>

#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

#include "connectionmanager.h"

int main(int argc, char* argv[]) {
  QCoreApplication a(argc, argv);

  ConnectionConfig config;
  // config.address = "ipc:///tmp/broker.sock";
  config.address = "tcp://127.0.0.1:5555";
  config.clientId = "client1_debug_" + std::to_string(QDateTime::currentMSecsSinceEpoch());

  ConnectionManager::init(config);

  ConnectionManager::registerCallback("test", [](const std::string& message) {
    std::cerr << message << std::endl;

    ConnectionManager::sendMessage("MessageReceived", "Send a response");
  });

  return a.exec();
}
