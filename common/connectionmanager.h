#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <google/protobuf/any.pb.h>
#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "logger.h"
#include "safequeue.h"
#include "workerinterface.h"

using MessageCallback = std::function<void(const std::string&)>;
using FileCallback = std::function<void(const std::string&)>;
using StatusCallback = std::function<void(bool)>;

struct FileTransferState {
  std::ofstream fileHandle;
  std::string destFilename;
  std::string tempPath;
  std::string originalTopic;
  size_t totalSize;
  size_t receivedSize;
};

struct CallbackEntry {
  void* instance;
  MessageCallback func;
};

class ConnectionManager
    : public std::enable_shared_from_this<ConnectionManager> {
 public:
  static void init(const ConnectionConfig& config);
  static void shutdown();
  static ConnectionManager& instance();

  static bool sendMessage(const std::string& key, const std::string& message);
  static bool sendData(const std::string& key, const std::string_view& data);
  static bool sendDataRaw(const std::string& key, const char* data, int len);
  static bool sendFile(const std::string& key, const std::string& filepath);

  template <typename T>
  static typename std::enable_if<
      std::is_base_of<google::protobuf::Message, T>::value, bool>::type
  sendMessage(const std::string& key, const T& protobufMessage) {
    return instance().sendMessageInternal(key, protobufMessage);
  }

  template <typename ClassType>
  static void registerCallback(const std::string& key,
                               void (ClassType::*method)(const std::string&),
                               ClassType* instance) {
    registerInternal(
        key,
        [instance, method](const std::string& msg) {
          (instance->*method)(msg);
        },
        instance);
  }

  template <typename T, typename ClassType>
  static void registerCallback(const std::string& key,
                               void (ClassType::*method)(const T&),
                               ClassType* instance) {
    registerInternal(
        key,
        [instance, method](const std::string& raw) {
          T msg;
          if (tryUnpack(raw, msg)) {
            (instance->*method)(msg);
          } else {
            Logger::Log(Logger::ERROR,
                        "Failed to unpack message for key: " + key);
          }
        },
        instance);
  }

  static void registerCallback(const std::string& key, MessageCallback cb) {
    registerInternal(key, cb, nullptr);
  }

  static void registerFileCallback(const std::string& key,
                                   FileCallback callback);
  static void registerStatusCallback(StatusCallback callback);

  template <typename T>
  static bool tryUnpack(const std::string& raw, T& outMsg) {
    google::protobuf::Any any;
    if (any.ParseFromString(raw)) {
      if (any.Is<T>()) {
        return any.UnpackTo(&outMsg);
      }
    }
    outMsg.Clear();
    return outMsg.ParseFromString(raw);
  }

  static void unregisterCallback(const std::string& key, void* instance);

  static bool sendRequest(const std::string& requestTopic,
                          const std::string& replyTopic,
                          const std::string& payload, std::string& outResponse,
                          int timeoutMs = 5000);

  template <typename ReqT, typename ResT>
  static bool sendRequest(const std::string& requestTopic,
                          const std::string& replyTopic, const ReqT& payload,
                          ResT& outResponse, int timeoutMs = 5000) {
    std::string payloadData = payload.serializeAsString();
    std::string rawResponse;

    if (sendRequest(requestTopic, replyTopic, payloadData, rawResponse,
                    timeoutMs)) {
      return tryUnpack(rawResponse, outResponse);
    }

    return false;
  }

 private:
  ConnectionManager(const ConnectionConfig& config);
  ~ConnectionManager();

  void resubscribeAll();
  static void registerInternal(const std::string& key, MessageCallback callback,
                               void* instance);
  void registerFileInternal(const std::string& key, FileCallback callback);

  bool sendDataInternal(const std::string& key, const std::string_view& data);
  bool sendFileInternal(const std::string& key, const std::string& filePath);
  bool sendRawEnvelope(const broker::BrokerPayload& envelope);

  template <typename T>
  bool sendMessageInternal(const std::string& key, const T& protobufMessage) {
    broker::BrokerPayload envelope;
    envelope.set_handler_key(key);
    envelope.set_sender_id(m_clientId);
    envelope.set_topic(key);
    envelope.mutable_payload()->PackFrom(protobufMessage);
    return sendRawEnvelope(envelope);
  }

  void processingLoop();
  void handleMessage(const broker::BrokerPayload& msg);
  void handleFilePacket(const broker::BrokerPayload& msg);

 private:
  static std::shared_ptr<ConnectionManager> m_instance;
  static std::mutex m_initMutex;

  std::string m_clientId;

  std::unique_ptr<WorkerInterface> m_worker;

  SafeQueue<broker::BrokerPayload> m_queue;
  std::thread m_processingThread;
  std::atomic<bool> m_running;

  std::atomic<bool> m_connected;

  std::mutex m_mapMutex;
  std::map<std::string, std::vector<CallbackEntry>> m_msgHandlers;
  std::map<std::string, std::vector<FileCallback>> m_fileHandlers;
  std::vector<StatusCallback> m_statusHandlers;

  std::map<std::string, std::shared_ptr<FileTransferState>> m_transfers;

  std::chrono::steady_clock::time_point m_lastConnectionTime;

  static std::vector<std::tuple<std::string, MessageCallback, void*>>
      s_pendingMsgCallbacks;
  static std::vector<std::pair<std::string, FileCallback>>
      s_pendingFileCallbacks;
  static std::vector<StatusCallback> s_pendingStatusCallbacks;
};

#endif  // CONNECTIONMANAGER_H
