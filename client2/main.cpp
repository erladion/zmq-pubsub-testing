#include <QCoreApplication>

#include <QJsonDocument>
#include <QJsonObject>

#include "zmq_client_manager.h"

int main(int argc, char* argv[]) {
  QCoreApplication a(argc, argv);

  ZmqClientManager::init("tcp://127.0.0.1:5555");

  ZmqClientManager::registerCallback("MessageReceived", [](const QByteArray& data) { qDebug() << data; });

  QTimer t;
  QObject::connect(&t, &QTimer::timeout, []() {
    QJsonObject payload;
    payload["id"] = "client2";
    payload["message"] = "Sending a message";
    payload["timestamp"] = QDateTime::currentMSecsSinceEpoch();

    ZmqClientManager::sendJson("test", payload);
  });

  t.start(2000);

  QTimer::singleShot(5000, [&]() { ZmqClientManager::sendFile("file", "/mnt/c/Users/johan/Downloads/logo1.png"); });

  return a.exec();
}
