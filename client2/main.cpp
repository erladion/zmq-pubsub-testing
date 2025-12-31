#include <QCoreApplication>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "connectionmanager.h"

int main(int argc, char* argv[]) {
  QCoreApplication a(argc, argv);

  ConnectionConfig config;
  config.address = "tcp://127.0.0.1:5555";
  config.clientId = "client2";

  ConnectionManager::init(config);

  ConnectionManager::registerCallback("MessageReceived", [](const std::string& data) { std::cout << data << std::endl; });

  QTimer t;
  QObject::connect(&t, &QTimer::timeout, []() {
    std::cout << "[Client2] Timer fired, sending message..." << std::endl;
    QJsonObject payload;
    payload["id"] = "client2";
    payload["message"] = "Sending a message";
    payload["timestamp"] = QDateTime::currentMSecsSinceEpoch();

    ConnectionManager::sendMessage("test", QString(QJsonDocument(payload).toJson()).toStdString());
  });

  t.start(2000);

  // QTimer::singleShot(5000, [&]() { ZmqClientManager::sendFile("file", "/mnt/c/Users/johan/Downloads/logo1.png"); });

  return a.exec();
}
