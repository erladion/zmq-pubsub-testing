#include "zmq_client_manager.h"
#include "zmq_common.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

// --- HELPER FUNCTION: MANUAL MULTIPART RECEIVE ---
static bool receive_multipart_safe(zmq::socket_t& socket, std::vector<zmq::message_t>& msgs) {
  msgs.clear();
  while (true) {
    zmq::message_t msg;
    auto res = socket.recv(msg, zmq::recv_flags::none);
    if (!res)
      return false;
    msgs.emplace_back(std::move(msg));
    if (!socket.get(zmq::sockopt::rcvmore))
      break;
  }
  return true;
}

ZmqClientManager* g_instance = nullptr;

void ZmqClientManager::init(const std::string& address) {
  if (!g_instance)
    g_instance = new ZmqClientManager(address);
}

ZmqClientManager& ZmqClientManager::instance() {
  return *g_instance;
}

ZmqClientManager::ZmqClientManager(const std::string& address) : m_address(address), m_ctx(1), m_running(true) {
  // Start background poller
  m_thread = std::thread(&ZmqClientManager::workerLoop, this);

  m_cleanupTimer = new QTimer(this);
  connect(m_cleanupTimer, &QTimer::timeout, this, &ZmqClientManager::cleanupLoop);
  m_cleanupTimer->start(10000);
}

ZmqClientManager::~ZmqClientManager() {
  m_running = false;
  if (m_thread.joinable())
    m_thread.join();
}

// --- API ---

void ZmqClientManager::registerCallback(const QString& topic, MessageCallback cb) {
  QMutexLocker lock(&instance().m_mutex);
  instance().m_msgHandlers.insert(topic, cb);
  // Send subscription message
  instance().sendInternal(topic.toStdString(), ZmqProtocol::TYPE_SUB, "");
}

void ZmqClientManager::registerFileCallback(const QString& topic, FileCallback cb) {
  QMutexLocker lock(&instance().m_mutex);
  instance().m_fileHandlers.insert(topic, cb);
  instance().sendInternal(topic.toStdString(), ZmqProtocol::TYPE_SUB, "");
}

void ZmqClientManager::sendJson(const QString& topic, const QJsonObject& json) {
  QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Compact);
  instance().sendInternal(topic.toStdString(), ZmqProtocol::TYPE_MSG, data);
}

void ZmqClientManager::sendFile(const QString& topic, const QString& filePath) {
  // Run Async
  QtConcurrent::run([topic, filePath]() {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
      return;

    std::string transferId = QUuid::createUuid().toString().toStdString();
    std::string stdTopic = topic.toStdString();

    // 1. Send Metadata
    QJsonObject meta;
    meta["filename"] = QFileInfo(filePath).fileName();
    meta["transfer_id"] = QString::fromStdString(transferId);

    instance().sendInternal(stdTopic, ZmqProtocol::TYPE_FILE_META, QJsonDocument(meta).toJson(QJsonDocument::Compact));

    // 2. Stream Loop
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    const int CHUNK_SIZE = 64 * 1024;
    int seq = 0;

    while (!file.atEnd()) {
      QByteArray chunk = file.read(CHUNK_SIZE);
      hasher.addData(chunk);

      QByteArray chunkPayload;
      chunkPayload.append(transferId.c_str());
      chunkPayload.append('|');  // Delimiter
      chunkPayload.append(chunk);

      instance().sendInternal(stdTopic, ZmqProtocol::TYPE_CHUNK, chunkPayload);

      // Traffic shaping
      if (seq++ % 16 == 0)
        QThread::msleep(1);
    }

    // 3. Footer
    QJsonObject footer;
    footer["transfer_id"] = QString::fromStdString(transferId);
    footer["checksum"] = QString(hasher.result().toHex());

    instance().sendInternal(stdTopic, ZmqProtocol::TYPE_FILE_FOOTER, QJsonDocument(footer).toJson(QJsonDocument::Compact));
  });
}

void ZmqClientManager::sendInternal(const std::string& topic, const std::string& type, const QByteArray& payload) {
  QMutexLocker lock(&m_outboxMutex);
  m_outbox.push_back({topic, type, payload});
}

// --- WORKER THREAD ---

void ZmqClientManager::workerLoop() {
  m_socket = std::make_unique<zmq::socket_t>(m_ctx, zmq::socket_type::dealer);

  // Set Identity
  std::string id = "Client-" + QUuid::createUuid().toString().toStdString();
  m_socket->set(zmq::sockopt::routing_id, id);
  m_socket->connect(m_address);

  while (m_running) {
    // 1. Process Outbox (Sending)
    {
      QMutexLocker lock(&m_outboxMutex);
      for (const auto& item : m_outbox) {
        std::string uuid = QUuid::createUuid().toString().toStdString();
        auto parts = ZmqProtocol::buildMessage(uuid, item.topic, item.type, item.payload.data(), item.payload.size());

        // --- FIX: SEND EMPTY DELIMITER FIRST ---
        m_socket->send(zmq::message_t(), zmq::send_flags::sndmore);

        // Send multipart parts
        for (size_t i = 0; i < parts.size(); ++i) {
          m_socket->send(parts[i], (i < parts.size() - 1) ? zmq::send_flags::sndmore : zmq::send_flags::none);
        }
      }
      m_outbox.clear();
    }

    // 2. Poll Inbox (Receiving)
    zmq::pollitem_t items[] = {{*m_socket, 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, 1, std::chrono::milliseconds(10));

    if (items[0].revents & ZMQ_POLLIN) {
      std::vector<zmq::message_t> parts;
      bool res = receive_multipart_safe(*m_socket, parts);
      if (res) {
        // Dealer Recv: [Empty][UUID][Topic][Type][Payload]
        // We expect 5 frames minimum now because Broker sends [ID][Empty][...] and Dealer strips ID.
        if (parts.size() >= 5) {
          std::vector<std::string> strParts;
          // Start from Index 1 to skip Empty Delimiter
          for (size_t i = 1; i < parts.size(); ++i) {
            strParts.push_back(parts[i].to_string());
          }
          processIncoming(strParts);
        }
      }
    }
  }
}

void ZmqClientManager::processIncoming(const std::vector<std::string>& frames) {
  // Frames are now [UUID, Topic, Type, Payload] (aligned)
  if (frames.size() < 4)
    return;

  QString topic = QString::fromStdString(frames[1]);
  QString type = QString::fromStdString(frames[2]);
  QByteArray payload = QByteArray::fromStdString(frames[3]);

  QMutexLocker lock(&m_mutex);

  // --- FILE TRANSFER LOGIC ---
  if (type == QString::fromStdString(ZmqProtocol::TYPE_FILE_META)) {
    QJsonObject meta = QJsonDocument::fromJson(payload).object();
    QString tid = meta["transfer_id"].toString();

    IncomingFile inc;
    inc.filename = meta["filename"].toString();
    inc.tempPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/" + tid + ".dat";
    inc.tempFile = new QFile(inc.tempPath);
    if (!inc.tempFile->open(QIODevice::ReadWrite)) {
      qWarning() << "Failed to create temp file:" << inc.tempPath;
      delete inc.tempFile;
      return;
    }
    inc.hasher = new QCryptographicHash(QCryptographicHash::Sha256);
    inc.lastUpdate = QDateTime::currentMSecsSinceEpoch();

    m_incomingFiles.insert(tid, inc);
    return;
  } else if (type == QString::fromStdString(ZmqProtocol::TYPE_CHUNK)) {
    int split = payload.indexOf('|');
    if (split == -1)
      return;

    QString tid = payload.left(split);
    QByteArray chunk = payload.mid(split + 1);

    if (m_incomingFiles.contains(tid)) {
      IncomingFile& f = m_incomingFiles[tid];
      f.tempFile->write(chunk);
      f.hasher->addData(chunk);
      f.lastUpdate = QDateTime::currentMSecsSinceEpoch();
    }
    return;
  } else if (type == QString::fromStdString(ZmqProtocol::TYPE_FILE_FOOTER)) {
    QJsonObject foot = QJsonDocument::fromJson(payload).object();
    QString tid = foot["transfer_id"].toString();
    QByteArray expected = QByteArray::fromHex(foot["checksum"].toString().toUtf8());

    if (m_incomingFiles.contains(tid)) {
      IncomingFile f = m_incomingFiles.take(tid);  // Remove from map
      f.tempFile->close();

      if (f.hasher->result() == expected) {
        QString finalPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + f.filename;

        // Auto-rename if exists
        int c = 1;
        while (QFile::exists(finalPath)) {
          QFileInfo fi(f.filename);
          finalPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + fi.baseName() + "_" + QString::number(c++) + "." +
                      fi.completeSuffix();
        }

        QFile::remove(finalPath);
        if (QFile::rename(f.tempPath, finalPath)) {
          if (m_fileHandlers.contains(topic)) {
            m_fileHandlers[topic](finalPath);
          }
        } else {
          qWarning() << "Failed to move file to:" << finalPath;
        }
      } else {
        qWarning() << "Checksum Mismatch!";
        QFile::remove(f.tempPath);
      }
      delete f.tempFile;
      delete f.hasher;
    }
    return;
  }

  // --- MSG LOGIC ---
  if (m_msgHandlers.contains(topic)) {
    m_msgHandlers[topic](payload);
  }
}

void ZmqClientManager::cleanupLoop() {
  QMutexLocker lock(&m_mutex);
  qint64 now = QDateTime::currentMSecsSinceEpoch();
  auto it = m_incomingFiles.begin();
  while (it != m_incomingFiles.end()) {
    if (now - it.value().lastUpdate > 30000) {
      it.value().tempFile->close();
      QFile::remove(it.value().tempPath);
      delete it.value().tempFile;
      delete it.value().hasher;
      it = m_incomingFiles.erase(it);
    } else {
      ++it;
    }
  }
}
