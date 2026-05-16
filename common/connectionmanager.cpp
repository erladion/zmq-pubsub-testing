#include "connectionmanager.h"

#include <filesystem>
#include <random>
#include <sstream>

#include "logger.h"
#include "messagekeys.h"
#include "uuidhelper.h"
#include "zmqworker.h"
// #include "grpcworker.h"

using namespace std::string_literals;

std::shared_ptr<ConnectionManager> ConnectionManager::m_instance = nullptr;
std::mutex ConnectionManager::m_initMutex;

std::vector<std::tuple<std::string, MessageCallback, void*>> ConnectionManager::s_pendingMsgCallbacks;
std::vector<std::pair<std::string, FileCallback>> ConnectionManager::s_pendingFileCallbacks;
std::vector<StatusCallback> ConnectionManager::s_pendingStatusCallbacks;

void ConnectionManager::init(const ConnectionConfig& config) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (!m_instance) {
    m_instance = std::shared_ptr<ConnectionManager>(new ConnectionManager(config), [](ConnectionManager* ptr) { delete ptr; });

    // Flush pending callbacks
    for (auto& p : s_pendingMsgCallbacks) {
      m_instance->registerInternal(std::get<0>(p), std::get<1>(p), std::get<2>(p));
    }
    s_pendingMsgCallbacks.clear();

    for (auto& p : s_pendingFileCallbacks) {
      m_instance->registerFileInternal(p.first, p.second);
    }
    s_pendingFileCallbacks.clear();

    {
      std::lock_guard<std::mutex> mapLock(m_instance->m_mapMutex);
      for (auto& cb : s_pendingStatusCallbacks) {
        m_instance->m_statusHandlers.push_back(cb);
      }
    }
    s_pendingStatusCallbacks.clear();
  }
}

void ConnectionManager::shutdown() {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    m_instance->m_running = false;
    m_instance.reset();
  }
}

ConnectionManager& ConnectionManager::instance() {
  if (m_instance == nullptr) {
    Logger::Log(Logger::ERROR, "ConnectionManager::instance() called before init()!");
    throw std::runtime_error("ConnectionManager not initialized");
  }

  return *m_instance;
}

void ConnectionManager::unregisterCallback(const std::string& key, void* instance) {
  if (!m_instance) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_instance->m_mapMutex);

  if (m_instance->m_msgHandlers.count(key)) {
    auto& vec = m_instance->m_msgHandlers[key];

    auto newEnd = std::remove_if(vec.begin(), vec.end(), [instance](const CallbackEntry& entry) { return entry.instance == instance; });

    vec.erase(newEnd, vec.end());

    if (vec.empty()) {
      m_instance->m_msgHandlers.erase(key);
    }
  }
}

bool ConnectionManager::sendRequest(const std::string& requestTopic,
                                    const std::string& replyTopic,
                                    const std::string& payload,
                                    std::string& outResponse,
                                    int timeoutMs) {
  if (!m_instance) {
    return false;
  }

  auto promise = std::make_shared<std::promise<std::string>>();
  std::future<std::string> future = promise->get_future();

  void* tempInstanceKey = promise.get();

  registerInternal(
      replyTopic,
      [promise](const std::string& responseData) {
        try {
          promise->set_value(responseData);
        } catch (...) {
        }
      },
      tempInstanceKey);

  sendData(requestTopic, payload);

  bool success = false;
  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
    outResponse = future.get();
    success = true;
  } else {
    Logger::Log(Logger::WARNING, "Timeout waiting for reply on: " + replyTopic);
  }

  unregisterCallback(replyTopic, tempInstanceKey);

  return success;
}

ConnectionManager::ConnectionManager(const ConnectionConfig& config) : m_clientId(config.clientId), m_running(true), m_connected(false) {
  auto statusHandler = [this](bool connected) {
    std::lock_guard<std::mutex> lock(m_mapMutex);

    Logger::Log(Logger::INFO, std::string("Status: ") + (connected ? "ONLINE" : "OFFLINE"));

    m_connected = connected;

    if (connected) {
      m_lastConnectionTime = std::chrono::steady_clock::now();
    }

    for (auto& callback : m_statusHandlers) {
      callback(connected);
    }

    if (connected) {
      broker::BrokerPayload hello;
      hello.set_handler_key(std::string(Keys::CONNECT));
      hello.set_sender_id(m_clientId);
      hello.set_topic("");

      m_worker->writeControlMessage(hello);

      // Re-send subscriptions
      for (auto const& [topic, _] : m_msgHandlers) {
        broker::BrokerPayload sub;
        sub.set_handler_key(std::string(Keys::SUBSCRIBE));
        sub.set_sender_id(m_clientId);
        sub.set_topic(topic);
        m_worker->writeControlMessage(sub);
      }
      for (auto const& [topic, _] : m_fileHandlers) {
        broker::BrokerPayload sub;
        sub.set_handler_key(std::string(Keys::SUBSCRIBE));
        sub.set_sender_id(m_clientId);
        sub.set_topic(topic);
        m_worker->writeControlMessage(sub);
      }
    }
  };

  if (config.protocol == ProtocolType::ZMQ) {
    m_worker = std::make_unique<ZmqWorker>(config, &m_queue, statusHandler);
  } else if (config.protocol == ProtocolType::GRPC) {
    // m_worker = std::make_unique<GrpcWorker>(config, &m_queue, statusHandler);
    Logger::Log(Logger::ERROR, "gRPC Worker not implemented yet!");
  }

  if (m_worker) {
    m_worker->start();
  }

  m_processingThread = std::thread(&ConnectionManager::processingLoop, this);
}

ConnectionManager::~ConnectionManager() {
  m_running = false;
  m_queue.stop();

  if (m_processingThread.joinable()) {
    m_processingThread.join();
  }

  if (m_worker) {
    m_worker->stop();
  }
}

bool ConnectionManager::sendMessage(const std::string& key, const std::string& message) {
  if (!m_instance) {
    return false;
  }
  return instance().sendDataInternal(key, message);
}
bool ConnectionManager::sendData(const std::string& key, const std::string_view& data) {
  if (!m_instance) {
    return false;
  }
  return instance().sendDataInternal(key, data);
}
bool ConnectionManager::sendDataRaw(const std::string& key, const char* data, int len) {
  if (!m_instance) {
    return false;
  }
  return instance().sendDataInternal(key, std::string(data, len));
}
bool ConnectionManager::sendFile(const std::string& key, const std::string& filepath) {
  if (!m_instance) {
    return false;
  }
  return instance().sendFileInternal(key, filepath);
}

void ConnectionManager::registerCallback(const std::string& key, MessageCallback callback) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    instance().registerInternal(key, callback, nullptr);
  } else {
    s_pendingMsgCallbacks.push_back({key, callback, nullptr});
  }
}

void ConnectionManager::registerFileCallback(const std::string& key, FileCallback callback) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    instance().registerFileInternal(key, callback);
  } else {
    s_pendingFileCallbacks.push_back({key, callback});
  }
}

void ConnectionManager::registerStatusCallback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_instance) {
    std::lock_guard<std::mutex> mapLock(instance().m_mapMutex);
    instance().m_statusHandlers.push_back(callback);
  } else {
    s_pendingStatusCallbacks.push_back(callback);
  }
}

void ConnectionManager::resubscribeAll() {
  std::lock_guard<std::mutex> lock(m_mapMutex);
  Logger::Log(Logger::INFO, "Server requested Reset. Re-sending all subscriptions...");

  for (auto const& [topic, _] : m_msgHandlers) {
    broker::BrokerPayload sub;
    sub.set_handler_key(std::string(Keys::SUBSCRIBE));
    sub.set_sender_id(m_clientId);
    sub.set_topic(topic);
    sendRawEnvelope(sub);
  }

  for (auto const& [topic, _] : m_fileHandlers) {
    broker::BrokerPayload sub;
    sub.set_handler_key(std::string(Keys::SUBSCRIBE));
    sub.set_sender_id(m_clientId);
    sub.set_topic(topic);
    sendRawEnvelope(sub);
  }
}

void ConnectionManager::registerInternal(const std::string& key, MessageCallback callback, void* instance) {
  if (!m_instance) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_instance->m_mapMutex);

  m_instance->m_msgHandlers[key].push_back({instance, callback});

  if (m_instance->m_connected) {
    broker::BrokerPayload sub;
    sub.set_handler_key(std::string(Keys::SUBSCRIBE));
    sub.set_sender_id(m_instance->m_clientId);
    sub.set_topic(key);
    m_instance->sendRawEnvelope(sub);
  }
}

bool ConnectionManager::sendRawEnvelope(const broker::BrokerPayload& envelope) {
  if (!m_worker) {
    return false;
  }

  std::string key = envelope.handler_key();

  if (Keys::isControlMessage(key)) {
    return m_worker->writeControlMessage(envelope);
  }

  return m_worker->writeMessage(envelope);
}

bool ConnectionManager::sendDataInternal(const std::string& key, const std::string_view& data) {
  broker::BrokerPayload msg;
  msg.set_handler_key(key);
  msg.set_sender_id(m_clientId);
  msg.set_topic(key);
  msg.set_raw_data(data.data(), data.size());
  return sendRawEnvelope(msg);
}

void ConnectionManager::registerFileInternal(const std::string& key, FileCallback callback) {
  std::lock_guard<std::mutex> lock(m_mapMutex);
  m_fileHandlers[key].push_back(callback);

  if (m_connected) {
    broker::BrokerPayload sub;
    sub.set_handler_key(std::string(Keys::SUBSCRIBE));
    sub.set_sender_id(m_clientId);
    sub.set_topic(key);
    sendRawEnvelope(sub);
  }
}

void ConnectionManager::processingLoop() {
  broker::BrokerPayload msg;
  while (m_queue.pop(msg)) {
    if (!m_running) {
      break;
    }

    std::string key = msg.handler_key();

    if (key == Keys::RESET) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastConnectionTime).count();

      if (elapsed > 2) {
        resubscribeAll();
      }
    } else if (Keys::isFilePacket(key)) {
      handleFilePacket(msg);
    } else {
      handleMessage(msg);
    }
  }
}

void ConnectionManager::handleMessage(const broker::BrokerPayload& msg) {
  std::string topic = msg.topic();
  std::string data = msg.has_payload() ? msg.payload().value() : msg.raw_data();

  std::vector<CallbackEntry> callbacks;
  {
    std::lock_guard<std::mutex> lock(m_mapMutex);
    auto it = m_msgHandlers.find(topic);
    if (it != m_msgHandlers.end()) {
      callbacks = it->second;
    }
  }

  for (auto& entry : callbacks) {
    try {
      if (entry.func) {
        entry.func(data);
      }
    } catch (const std::exception& e) {
      Logger::Log(Logger::ERROR, std::string("User Callback Exception: ") + e.what());
    } catch (...) {
      Logger::Log(Logger::ERROR, "Unknown User Exception");
    }
  }
}

bool ConnectionManager::sendFileInternal(const std::string& key, const std::string& filePath) {
  if (!std::filesystem::exists(filePath)) {
    return false;
  }

  std::thread([self = shared_from_this(), key, filePath]() {
    std::filesystem::path path(filePath);
    std::string filename = path.filename().string();
    size_t fileSize = std::filesystem::file_size(path);
    std::string transferId = generateUUID();

    std::ifstream inputFile(filePath, std::ios::binary);
    if (!inputFile.is_open()) {
      return;
    }

    std::stringstream metaJson;
    metaJson << "{\"filename\":\"" << filename << "\",\"size\":" << fileSize << "}";

    broker::BrokerPayload metaMsg;
    metaMsg.set_handler_key(std::string(Keys::FILE_META));
    metaMsg.set_topic(key);
    metaMsg.set_sender_id(self->m_clientId);
    metaMsg.set_transfer_id(transferId);
    metaMsg.set_raw_data(metaJson.str());

    if (!self->sendRawEnvelope(metaMsg)) {
      return;
    }

    const size_t CHUNK_SIZE = 64 * 1024;
    std::vector<char> buffer(CHUNK_SIZE);

    while (inputFile.read(buffer.data(), CHUNK_SIZE) || inputFile.gcount() > 0) {
      if (!self->m_running) {
        return;
      }

      broker::BrokerPayload chunkMsg;
      chunkMsg.set_handler_key(std::string(Keys::FILE_CHUNK));
      chunkMsg.set_topic(key);
      chunkMsg.set_sender_id(self->m_clientId);
      chunkMsg.set_transfer_id(transferId);
      chunkMsg.set_raw_data(buffer.data(), inputFile.gcount());
      self->sendRawEnvelope(chunkMsg);
    }

    broker::BrokerPayload footerMsg;
    footerMsg.set_handler_key(std::string(Keys::FILE_FOOTER));
    footerMsg.set_topic(key);
    footerMsg.set_sender_id(self->m_clientId);
    footerMsg.set_transfer_id(transferId);
    self->sendRawEnvelope(footerMsg);
  }).detach();

  return true;
}

void ConnectionManager::handleFilePacket(const broker::BrokerPayload& msg) {
  std::string type = msg.handler_key();
  std::string id = msg.transfer_id();
  std::lock_guard<std::mutex> lock(m_mapMutex);

  if (type == Keys::FILE_META) {
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
  } else if (type == Keys::FILE_CHUNK) {
    if (m_transfers.find(id) == m_transfers.end()) {
      return;
    }

    auto& state = m_transfers[id];
    state->fileHandle.write(msg.raw_data().data(), msg.raw_data().size());
    state->receivedSize += msg.raw_data().size();
  } else if (type == Keys::FILE_FOOTER) {
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
