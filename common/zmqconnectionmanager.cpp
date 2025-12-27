#include "zmqconnectionmanager.h"

#include "logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

ZmqConnectionManager* ZmqConnectionManager::m_instance = nullptr;
std::mutex ZmqConnectionManager::m_initMutex;

std::vector<std::pair<std::string, MessageCallback>> ZmqConnectionManager::s_pendingMsgCallbacks;
std::vector<std::pair<std::string, FileCallback>> ZmqConnectionManager::s_pendingFileCallbacks;
std::vector<StatusCallback> ZmqConnectionManager::s_pendingStatusCallbacks;

static std::string generateUUID() {
  static const char hex_chars[] = "0123456789abcdef";
  thread_local std::random_device rd;
  thread_local std::mt19937 gen(rd());
  thread_local std::uniform_int_distribution<> dis(0, 15);
  thread_local std::uniform_int_distribution<> dis2(8, 11);

  std::string uuid(36, ' ');
  uuid[8] = '-';
  uuid[13] = '-';
  uuid[18] = '-';
  uuid[23] = '-';

  auto set_hex = [&](int index) { uuid[index] = hex_chars[dis(gen)]; };

  for (int i = 0; i < 8; ++i)
    set_hex(i);
  for (int i = 9; i < 13; ++i)
    set_hex(i);
  uuid[14] = '4';
  for (int i = 15; i < 18; ++i)
    set_hex(i);
  uuid[19] = hex_chars[dis2(gen)];
  for (int i = 20; i < 23; ++i)
    set_hex(i);
  for (int i = 24; i < 36; ++i)
    set_hex(i);

  return uuid;
}

void ZmqConnectionManager::init(const ConnectionConfig& config) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (!m_instance) {
    m_instance = new ZmqConnectionManager(config);

    // Flush pending callbacks
    for (auto& p : s_pendingMsgCallbacks) {
      m_instance->registerInternal(p.first, p.second);
    }
    s_pendingMsgCallbacks.clear();
    for (auto& p : s_pendingFileCallbacks) {
      m_instance->registerFileInternal(p.first, p.second);
    }
    s_pendingFileCallbacks.clear();

    {
      std::lock_guard<std::mutex> mapLock(m_instance->m_mapMutex);
      for (auto& cb : s_pendingStatusCallbacks)
        m_instance->m_statusHandlers.push_back(cb);
    }
    s_pendingStatusCallbacks.clear();
  }
}

void ZmqConnectionManager::shutdown() {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    delete m_instance;
    m_instance = nullptr;
  }
}

ZmqConnectionManager& ZmqConnectionManager::instance() {
  return *m_instance;
}

bool ZmqConnectionManager::sendMessage(const std::string& key, const std::string& message) {
  return instance().sendDataInternal(key, message);
}

bool ZmqConnectionManager::sendData(const std::string& key, const std::string_view& data) {
  return instance().sendDataInternal(key, data);
}

bool ZmqConnectionManager::sendDataRaw(const std::string& key, const char* data, int len) {
  return instance().sendDataInternal(key, std::string(data, len));
}

bool ZmqConnectionManager::sendFile(const std::string& key, const std::string& filepath) {
  return instance().sendFileInternal(key, filepath);
}

void ZmqConnectionManager::registerCallback(const std::string& key, MessageCallback callback) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance)
    instance().registerInternal(key, callback);
  else
    s_pendingMsgCallbacks.push_back({key, callback});
}

void ZmqConnectionManager::registerFileCallback(const std::string& key, FileCallback callback) {
  instance().registerFileInternal(key, callback);
}

void ZmqConnectionManager::registerStatusCallback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    std::lock_guard<std::mutex> mapLock(instance().m_mapMutex);
    instance().m_statusHandlers.push_back(callback);
  } else
    s_pendingStatusCallbacks.push_back(callback);
}

ZmqConnectionManager::ZmqConnectionManager(const ConnectionConfig& config) : m_clientId(config.clientId), m_running(true) {
  m_worker = new ZmqWorker(config, &m_queue, [this](bool connected) {
    std::lock_guard<std::mutex> lock(m_mapMutex);
    Logger::Log(Logger::INFO, std::string("Status: ") + (connected ? "ONLINE" : "OFFLINE"));

    for (auto& callback : m_statusHandlers) {
      callback(connected);
    }

    if (connected) {
      broker::BrokerPayload hello;
      hello.set_handler_key("__CONNECT__");
      hello.set_sender_id(m_clientId);
      hello.set_topic("");
      m_worker->writeMessage(hello);

      for (auto const& [topic, _] : m_msgHandlers) {
        broker::BrokerPayload sub;
        sub.set_handler_key("__SUBSCRIBE__");
        sub.set_sender_id(m_clientId);
        sub.set_topic(topic);
        m_worker->writeMessage(sub);
      }
      for (auto const& [topic, _] : m_fileHandlers) {
        broker::BrokerPayload sub;
        sub.set_handler_key("__SUBSCRIBE__");
        sub.set_sender_id(m_clientId);
        sub.set_topic(topic);
        m_worker->writeMessage(sub);
      }
    }
  });

  m_worker->start();
  m_processingThread = std::thread(&ZmqConnectionManager::processingLoop, this);
}

ZmqConnectionManager::~ZmqConnectionManager() {
  m_running = false;
  m_queue.stop();
  if (m_processingThread.joinable()) {
    m_processingThread.join();
  }
  delete m_worker;
}

void ZmqConnectionManager::resubscribeAll() {
  std::lock_guard<std::mutex> lock(m_mapMutex);
  Logger::Log(Logger::INFO, "Server requested Reset. Re-sending all subscriptions...");

  // Resend Message Subscriptions
  for (auto const& [topic, _] : m_msgHandlers) {
    broker::BrokerPayload sub;
    sub.set_handler_key("__SUBSCRIBE__");
    sub.set_sender_id(m_clientId);
    sub.set_topic(topic);
    sendRawEnvelope(sub);
  }

  // Resend File Subscriptions
  for (auto const& [topic, _] : m_fileHandlers) {
    broker::BrokerPayload sub;
    sub.set_handler_key("__SUBSCRIBE__");
    sub.set_sender_id(m_clientId);
    sub.set_topic(topic);
    sendRawEnvelope(sub);
  }
}

bool ZmqConnectionManager::sendRawEnvelope(const broker::BrokerPayload& envelope) {
  std::string key = envelope.handler_key();

  if (key == "__HEARTBEAT__" || key == "__SUBSCRIBE__" || key == "__CONNECT__" || key == "__RESET__") {
    return m_worker->writeControlMessage(envelope);
  }

  return m_worker->writeMessage(envelope);
}

bool ZmqConnectionManager::sendDataInternal(const std::string& key, const std::string_view& data) {
  broker::BrokerPayload msg;
  msg.set_handler_key(key);
  msg.set_sender_id(m_clientId);
  msg.set_topic(key);
  msg.set_raw_data(data.data(), data.size());
  return sendRawEnvelope(msg);
}

void ZmqConnectionManager::registerInternal(const std::string& key, MessageCallback callback) {
  std::lock_guard<std::mutex> lock(m_mapMutex);
  m_msgHandlers[key].push_back(callback);

  broker::BrokerPayload sub;
  sub.set_handler_key("__SUBSCRIBE__");
  sub.set_sender_id(m_clientId);
  sub.set_topic(key);
  sendRawEnvelope(sub);
}

void ZmqConnectionManager::registerFileInternal(const std::string& key, FileCallback callback) {
  std::lock_guard<std::mutex> lock(m_mapMutex);
  m_fileHandlers[key].push_back(callback);

  broker::BrokerPayload sub;
  sub.set_handler_key("__SUBSCRIBE__");
  sub.set_sender_id(m_clientId);
  sub.set_topic(key);
  sendRawEnvelope(sub);
}

void ZmqConnectionManager::processingLoop() {
  broker::BrokerPayload msg;
  while (m_queue.pop(msg)) {
    if (!m_running) {
      break;
    }
    std::string key = msg.handler_key();

    if (key == "__RESET__") {
      resubscribeAll();
    } else if (key == "__CHUNK__" || key == "__FILE_META__" || key == "__FILE_FOOTER__") {
      handleFilePacket(msg);
    } else {
      handleMessage(msg);
    }
  }
}

void ZmqConnectionManager::handleMessage(const broker::BrokerPayload& msg) {
  std::string topic = msg.topic();
  std::string data = msg.has_payload() ? msg.payload().value() : msg.raw_data();

  std::vector<MessageCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(m_mapMutex);
    auto it = m_msgHandlers.find(topic);
    if (it != m_msgHandlers.end()) {
      callbacks = it->second;
    }
  }

  for (auto& callback : callbacks) {
    try {
      callback(data);
    } catch (const std::exception& e) {
      Logger::Log(Logger::ERROR, std::string("User Callback Exception: ") + e.what());
    } catch (...) {
      Logger::Log(Logger::ERROR, "Unknown User Exception");
    }
  }
}

bool ZmqConnectionManager::sendFileInternal(const std::string& key, const std::string& filePath) {
  if (!std::filesystem::exists(filePath)) {
    return false;
  }

  std::thread([this, key, filePath]() {
    std::filesystem::path path(filePath);
    std::string filename = path.filename().string();
    size_t fileSize = std::filesystem::file_size(path);
    std::string transferId = generateUUID();

    std::ifstream inputFile(filePath, std::ios::binary);
    if (!inputFile.is_open()) {
      return false;
    }

    std::stringstream metaJson;
    metaJson << "{\"filename\":\"" << filename << "\",\"size\":" << fileSize << "}";

    broker::BrokerPayload metaMsg;
    metaMsg.set_handler_key("__FILE_META__");
    metaMsg.set_topic(key);
    metaMsg.set_sender_id(m_clientId);
    metaMsg.set_transfer_id(transferId);
    metaMsg.set_raw_data(metaJson.str());

    if (!m_worker->writeMessage(metaMsg)) {
      return false;
    }

    const size_t CHUNK_SIZE = 64 * 1024;
    std::vector<char> buffer(CHUNK_SIZE);

    while (inputFile.read(buffer.data(), CHUNK_SIZE) || inputFile.gcount() > 0) {
      broker::BrokerPayload chunkMsg;
      chunkMsg.set_handler_key("__CHUNK__");
      chunkMsg.set_topic(key);
      chunkMsg.set_sender_id(m_clientId);
      chunkMsg.set_transfer_id(transferId);
      chunkMsg.set_raw_data(buffer.data(), inputFile.gcount());
      m_worker->writeMessage(chunkMsg);
    }

    broker::BrokerPayload footerMsg;
    footerMsg.set_handler_key("__FILE_FOOTER__");
    footerMsg.set_topic(key);
    footerMsg.set_sender_id(m_clientId);
    footerMsg.set_transfer_id(transferId);
    m_worker->writeMessage(footerMsg);
  }).detach();

  return true;
}

void ZmqConnectionManager::handleFilePacket(const broker::BrokerPayload& msg) {
  std::string type = msg.handler_key();
  std::string id = msg.transfer_id();

  std::lock_guard<std::mutex> lock(m_mapMutex);

  if (type == "__FILE_META__") {
    auto state = std::make_shared<FileTransferState>();
    state->originalTopic = msg.topic();
    state->receivedSize = 0;

    std::string meta = msg.raw_data();

    size_t nameStart = meta.find("\"filename\":\"");
    if (nameStart != std::string::npos) {
      nameStart += 12;
      size_t nameEnd = meta.find("\"", nameStart);
      state->destFilename = meta.substr(nameStart, nameEnd - nameStart);
    } else {
      state->destFilename = "unknown_" + id + ".bin";
    }

    state->destFilename = std::filesystem::path(state->destFilename).filename().string();

    state->tempPath = (std::filesystem::temp_directory_path() / (id + ".part")).string();
    state->fileHandle.open(state->tempPath, std::ios::binary);

    if (!state->fileHandle.is_open()) {
      Logger::Log(Logger::ERROR, "Failed to create temp file: " + state->tempPath);
      return;
    }

    m_transfers[id] = state;

    Logger::Log(Logger::INFO, "Starting download: " + state->destFilename);
  }

  else if (type == "__CHUNK__") {
    if (m_transfers.find(id) == m_transfers.end()) {
      return;
    }

    auto& state = m_transfers[id];
    state->fileHandle.write(msg.raw_data().data(), msg.raw_data().size());
    state->receivedSize += msg.raw_data().size();
  }

  else if (type == "__FILE_FOOTER__") {
    if (m_transfers.find(id) == m_transfers.end()) {
      return;
    }

    auto state = m_transfers[id];
    state->fileHandle.close();

    std::filesystem::path downloadDir = std::filesystem::current_path() / "downloads";
    if (!std::filesystem::exists(downloadDir)) {
      std::filesystem::create_directory(downloadDir);
    }

    std::filesystem::path finalPath = downloadDir / state->destFilename;

    try {
      if (std::filesystem::exists(finalPath)) {
        std::filesystem::remove(finalPath);
      }
      std::filesystem::rename(state->tempPath, finalPath);

      Logger::Log(Logger::INFO, std::string("File saved to: ") + finalPath.string());

      if (m_fileHandlers.count(state->originalTopic)) {
        for (auto& callback : m_fileHandlers[state->originalTopic]) {
          callback(finalPath.string());
        }
      }

    } catch (const std::filesystem::filesystem_error& e) {
      Logger::Log(Logger::ERROR, std::string("File move failed: ") + e.what());
    }

    m_transfers.erase(id);
  }
}
