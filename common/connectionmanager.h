#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <google/protobuf/any.pb.h>
#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
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

template <typename T, typename Enable = void>
struct NetworkSerializer {
  static constexpr bool is_specialized = false;

  // std::string serialize(const T& value);
  // T deserialize(const std::string& bytes);
};

template <typename T>
struct CallableTraits : CallableTraits<decltype(&T::operator())> {};

// Match const lambdas (default for captureless lambdas)
template <typename ClassType, typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (ClassType::*)(Arg) const> {
  using ArgType = Arg;
};

// Match mutable lambdas
template <typename ClassType, typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (ClassType::*)(Arg)> {
  using ArgType = Arg;
};

// Match standard function pointers
template <typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (*)(Arg)> {
  using ArgType = Arg;
};

// Match zero-argument lambdas (Forces SFINAE to fail gracefully)
template <typename ClassType, typename ReturnType>
struct CallableTraits<ReturnType (ClassType::*)() const> {
  // Intentionally left blank!
  // No 'ArgType' means SFINAE will safely ignore the 1-arg templates.
};

class ConnectionManager : public std::enable_shared_from_this<ConnectionManager> {
public:
  static void init(const ConnectionConfig& config);
  static void shutdown();
  static ConnectionManager& instance();

  static bool sendMessage(const std::string& key, const std::string& message);
  static bool sendData(const std::string& key, const std::string_view& data);
  static bool sendDataRaw(const std::string& key, const char* data, int len);
  static bool sendFile(const std::string& key, const std::string& filepath);

  template <typename T>
  static typename std::enable_if<NetworkSerializer<T>::is_specialized, bool>::type sendMessage(const std::string& key, const T& value) {
    std::string bytes = NetworkSerializer<T>::serialize(value);

    return instance().sendDataRaw(key, bytes.data(), static_cast<int>(bytes.size()));
  }

  template <typename T>
  static typename std::enable_if<std::is_base_of<google::protobuf::Message, T>::value, bool>::type sendMessage(const std::string& key,
                                                                                                               const T& protobufMessage) {
    return instance().sendMessageInternal(key, protobufMessage);
  }

  template <typename T>
  static typename std::enable_if<std::is_trivially_copyable<T>::value && !std::is_base_of<google::protobuf::Message, T>::value &&
                                     !NetworkSerializer<T>::is_specialized,
                                 bool>::type
  sendMessage(const std::string& key, const T& value) {
    return instance().sendDataRaw(key, reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(T)));
  }

  template <typename Callable>
  static typename std::enable_if<
      std::is_trivially_copyable<typename std::decay<typename CallableTraits<Callable>::ArgType>::type>::value &&
          !std::is_base_of<google::protobuf::Message, typename std::decay<typename CallableTraits<Callable>::ArgType>::type>::value &&
          !NetworkSerializer<typename std::decay<typename CallableTraits<Callable>::ArgType>::type>::is_specialized,
      void>::type
  registerCallback(const std::string& key, Callable func, void* instance = nullptr) {
    using ArgType = typename CallableTraits<Callable>::ArgType;
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [func](const std::string& raw) {
          if (raw.size() == sizeof(BaseT)) {
            BaseT value;
            std::memcpy(&value, raw.data(), sizeof(BaseT));
            func(value);
          }
        },
        instance);
  }

  template <typename Callable>
  static typename std::
      enable_if<std::is_base_of<google::protobuf::Message, typename std::decay<typename CallableTraits<Callable>::ArgType>::type>::value, void>::type
      registerCallback(const std::string& key, Callable func, void* instance = nullptr) {
    using ArgType = typename CallableTraits<Callable>::ArgType;
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [func, key](const std::string& raw) {
          BaseT msg;
          if (tryUnpack(raw, msg)) {
            func(msg);
          } else {
            Logger::Log(Logger::ERROR, "Failed to unpack message for key: " + key);
          }
        },
        instance);
  }

  template <typename Callable>
  static typename std::enable_if<NetworkSerializer<typename std::decay<typename CallableTraits<Callable>::ArgType>::type>::is_specialized, void>::type
  registerCallback(const std::string& key, Callable func, void* instance = nullptr) {
    using ArgType = typename CallableTraits<Callable>::ArgType;
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [func, key](const std::string& raw) {
          try {
            BaseT value = NetworkSerializer<BaseT>::deserialize(raw);
            func(value);
          } catch (const std::exception& e) {
            Logger::Log(Logger::ERROR, std::string("Deserialization failed on: ") + key);
          }
        },
        instance);
  }

  template <typename ClassType, typename ArgType>
  static typename std::enable_if<NetworkSerializer<typename std::decay<ArgType>::type>::is_specialized, void>::type
  registerCallback(const std::string& key, void (ClassType::*method)(ArgType), ClassType* instance) {
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [key, instance, method](const std::string& raw) {
          try {
            BaseT value = NetworkSerializer<BaseT>::deserialize(raw);
            (instance->*method)(value);
          } catch (const std::exception& e) {
            Logger::Log(Logger::ERROR, std::string("Deserialization failed on: ") + key);
          }
        },
        instance);
  }

  template <typename ClassType, typename ArgType>
  static typename std::enable_if<std::is_trivially_copyable<typename std::decay<ArgType>::type>::value &&
                                     !std::is_base_of<google::protobuf::Message, typename std::decay<ArgType>::type>::value,
                                 void>::type
  registerCallback(const std::string& key, void (ClassType::*method)(ArgType), ClassType* instance) {
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [key, instance, method](const std::string& raw) {
          if (raw.size() == sizeof(BaseT)) {
            BaseT value;
            std::memcpy(&value, raw.data(), sizeof(BaseT));

            (instance->*method)(value);
          }
        },
        instance);
  }

  template <typename ClassType>
  static void registerCallback(const std::string& key, void (ClassType::*method)(const std::string&), ClassType* instance) {
    registerInternal(
        key, [instance, method](const std::string& msg) { (instance->*method)(msg); }, instance);
  }

  template <typename ClassType, typename ArgType>
  static typename std::enable_if<std::is_base_of<google::protobuf::Message, typename std::decay<ArgType>::type>::value, void>::type
  registerCallback(const std::string& key, void (ClassType::*method)(ArgType), ClassType* instance) {
    using BaseT = typename std::decay<ArgType>::type;

    registerInternal(
        key,
        [instance, method, key](const std::string& raw) {
          BaseT msg;
          if (tryUnpack(raw, msg)) {
            (instance->*method)(msg);
          } else {
            Logger::Log(Logger::ERROR, "Failed to unpack message for key: " + key);
          }
        },
        instance);
  }

  template <typename ClassType>
  static void registerCallback(const std::string& key, void (ClassType::*method)(), ClassType* instance) {
    registerInternal(
        key, [instance, method](const std::string& /* ignored */) { (instance->*method)(); }, instance);
  }

  static void registerCallback(const std::string& key, void (*func)(), void* instance = nullptr) {
    registerInternal(
        key, [func](const std::string& /* ignored */) { func(); }, instance);
  }

  static void registerCallback(const std::string& key, std::function<void()> callback, void* instance = nullptr) {
    registerInternal(
        key,
        [callback](const std::string& /* ignored */) {
          if (callback)
            callback();
        },
        instance);
  }

  static void registerCallback(const std::string& key, MessageCallback cb);

  static void registerFileCallback(const std::string& key, FileCallback callback);
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
                          const std::string& payload,
                          std::string& outResponse,
                          int timeoutMs = 5000);

  template <typename ReqT, typename ResT>
  static bool sendRequest(const std::string& requestTopic,
                          const std::string& replyTopic,
                          const ReqT& payload,
                          ResT& outResponse,
                          int timeoutMs = 5000) {
    std::string payloadData = payload.SerializeAsString();
    std::string rawResponse;

    if (sendRequest(requestTopic, replyTopic, payloadData, rawResponse, timeoutMs)) {
      return tryUnpack(rawResponse, outResponse);
    }

    return false;
  }

private:
  ConnectionManager(const ConnectionConfig& config);
  ~ConnectionManager();

  void resubscribeAll();
  static void registerInternal(const std::string& key, MessageCallback callback, void* instance);
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

  static std::vector<std::tuple<std::string, MessageCallback, void*>> s_pendingMsgCallbacks;
  static std::vector<std::pair<std::string, FileCallback>> s_pendingFileCallbacks;
  static std::vector<StatusCallback> s_pendingStatusCallbacks;
};

#endif  // CONNECTIONMANAGER_H
