#ifndef ZMQ_CLIENT_MANAGER_H
#define ZMQ_CLIENT_MANAGER_H

#include <QObject>
#include <QHash>
#include <QFile>
#include <QTimer>
#include <QMutex>
#include <QCryptographicHash>
#include <functional>
#include <thread>
#include <atomic>
#include <zmq.hpp>

// Callback types
using MessageCallback = std::function<void(const QByteArray&)>;
using FileCallback = std::function<void(const QString&)>;

struct IncomingFile {
  QString filename;
  QByteArray expectedHash;
  QFile* tempFile = nullptr;
  QString tempPath;
  QCryptographicHash* hasher = nullptr;
  qint64 lastUpdate = 0;
};

class ZmqClientManager : public QObject {
  Q_OBJECT
public:
  static void init(const std::string& address);
  static ZmqClientManager& instance();

  // API
  static void registerCallback(const QString& topic, MessageCallback cb);
  static void registerFileCallback(const QString& topic, FileCallback cb);
  static void sendJson(const QString& topic, const QJsonObject& json);
  static void sendFile(const QString& topic, const QString& filePath);

private:
  ZmqClientManager(const std::string& address);
  ~ZmqClientManager();

  void workerLoop();
  void cleanupLoop();

  void processIncoming(const std::vector<std::string>& frames);
  void sendInternal(const std::string& topic, const std::string& type, const QByteArray& payload);

private:
  std::string m_address;
  zmq::context_t m_ctx;
  std::unique_ptr<zmq::socket_t> m_socket; // DEALER

  std::atomic<bool> m_running;
  std::thread m_thread;
  QTimer* m_cleanupTimer;

  QHash<QString, MessageCallback> m_msgHandlers;
  QHash<QString, FileCallback> m_fileHandlers;
  QHash<QString, IncomingFile> m_incomingFiles; // Key: Transfer UUID

  QMutex m_mutex; // Protects maps

  // Outbox queue for thread safety
  struct OutboxItem { std::string topic; std::string type; QByteArray payload; };
  std::vector<OutboxItem> m_outbox;
  QMutex m_outboxMutex;
};

#endif // ZMQ_CLIENT_MANAGER_H
