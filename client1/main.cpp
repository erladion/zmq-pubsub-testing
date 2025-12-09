#include <QCoreApplication>

#include <QJsonDocument>
#include <QJsonObject>

#include "zmq_client_manager.h"

int main(int argc, char* argv[]) {
  QCoreApplication a(argc, argv);

  ZmqClientManager::init("ipc:///tmp/broker.sock");

  ZmqClientManager::registerCallback("test", [](const QByteArray& data) {
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject obj = doc.object();

    qDebug() << obj;

    QJsonObject payload;
    payload["id"] = "client1";
    payload["message"] = "Testing a return messag";

    ZmqClientManager::sendJson("MessageReceived", payload);
  });

  ZmqClientManager::registerFileCallback("file", [](const QString& filePath) {
    qDebug() << "Update downloaded to:" << filePath;

    // QFile::rename(filePath, "/var/updates/firmware.bin");
  });

  return a.exec();
}
