#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <google/protobuf/any.pb.h>
#include <google/protobuf/message.h>

#include <atomic>
#include <chrono>
#include <cstring>
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

struct CallbackEntry {
  void* instance;
  MessageCallback func;
};

template <typename T, typename Enable = void>
struct DataSerializer {
  static constexpr bool is_specialized = false;

  // std::string serialize(const T& value);
  // T deserialize(const std::string& bytes);
};

// Dependent-false for static_assert in discarded if-constexpr branches.
template <typename T>
inline constexpr bool always_false_v = false;

template <typename T>
struct CallableTraits : CallableTraits<decltype(&T::operator())> {};

template <typename ClassType, typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (ClassType::*)(Arg) const> {
  using ArgType = Arg;
};

template <typename ClassType, typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (ClassType::*)(Arg)> {
  using ArgType = Arg;
};

template <typename ReturnType, typename Arg>
struct CallableTraits<ReturnType (*)(Arg)> {
  using ArgType = Arg;
};

template <typename ClassType, typename ReturnType>
struct CallableTraits<ReturnType (ClassType::*)() const> {};

class ConnectionManager : public std::enable_shared_from_this<ConnectionManager> {
public:
  static void init(const ConnectionConfig& config);
  static void shutdown();
  static ConnectionManager& instance();

  static bool sendMessage(const std::string& key, const std::string& message);
  static bool sendData(const std::string& key, const std::string_view& data);
  static bool sendDataRaw(const std::string& key, const char* data, int len);

  // Pointers and arrays are excluded so string literals and char* still pick
  // the plain std::string overload above. Dispatch order: DataSerializer
  // specializations win over the protobuf and raw-bytes fallbacks.
  template <typename T, typename std::enable_if<!std::is_pointer<T>::value && !std::is_array<T>::value, int>::type = 0>
  static bool sendMessage(const std::string& key, const T& value) {
    if constexpr (DataSerializer<T>::is_specialized) {
      std::string bytes = DataSerializer<T>::serialize(value);
      return instance().sendDataRaw(key, bytes.data(), static_cast<int>(bytes.size()));
    } else if constexpr (std::is_base_of<google::protobuf::Message, T>::value) {
      return instance().sendMessageInternal(key, value);
    } else if constexpr (std::is_trivially_copyable<T>::value && std::is_standard_layout<T>::value) {
      // Byte-copied structs assume both endpoints share the same ABI; layout
      // and padding are not part of the wire contract.
      return instance().sendDataRaw(key, reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(T)));
    } else {
      static_assert(always_false_v<T>,
                    "sendMessage: unsupported payload type. Expected a protobuf Message, a DataSerializer-specialized "
                    "type, or a trivially copyable standard-layout struct.");
    }
  }

  // Dispatches on the callback's argument type. The BaseT default argument
  // doubles as SFINAE: callables without a single-argument signature (e.g.
  // void() lambdas) fail substitution and fall through to the overloads below.
  // Dispatch order: std::string, then DataSerializer specializations, then
  // protobuf, then raw-bytes structs.
  template <typename Callable, typename BaseT = typename std::decay<typename CallableTraits<Callable>::ArgType>::type>
  static void registerCallback(const std::string& key, Callable func, void* instance = nullptr) {
    if constexpr (std::is_same<BaseT, std::string>::value) {
      registerInternal(
          key, [func](const std::string& raw) { func(raw); }, instance);
    } else if constexpr (DataSerializer<BaseT>::is_specialized) {
      registerInternal(
          key,
          [func, key](const std::string& raw) {
            try {
              BaseT value = DataSerializer<BaseT>::deserialize(raw);
              func(value);
            } catch (const std::exception& e) {
              Logger::Log(Logger::ERROR, std::string("Deserialization failed on: ") + key);
            }
          },
          instance);
    } else if constexpr (std::is_base_of<google::protobuf::Message, BaseT>::value) {
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
    } else if constexpr (std::is_trivially_copyable<BaseT>::value && std::is_standard_layout<BaseT>::value) {
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
    } else {
      static_assert(always_false_v<Callable>,
                    "registerCallback: unsupported callback argument type. Expected std::string, a protobuf Message, a "
                    "DataSerializer-specialized type, or a trivially copyable standard-layout struct.");
    }
  }

  template <typename ClassType, typename ArgType>
  static void registerCallback(const std::string& key, void (ClassType::*method)(ArgType), ClassType* instance) {
    registerCallback(
        key, [instance, method](ArgType arg) { (instance->*method)(std::forward<ArgType>(arg)); }, instance);
  }

  template <typename ClassType>
  static void registerCallback(const std::string& key, void (ClassType::*method)(), ClassType* instance) {
    registerCallback(
        key, [instance, method]() { (instance->*method)(); }, instance);
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

  template <typename T>
  static bool tryUnpack(const std::string& raw, T& outMsg) {
    // Payloads packed by sendMessage()/replyToSender() arrive as a serialized
    // Any. If the bytes are Any-shaped, commit to that interpretation: a type
    // mismatch is a hard failure, not a reason to re-parse the envelope bytes
    // as T (proto3 parsing is permissive enough that this would often
    // "succeed" and hand the callback a garbage-filled message).
    google::protobuf::Any any;
    if (any.ParseFromString(raw) && any.type_url().rfind("type.googleapis.com/", 0) == 0) {
      return any.Is<T>() && any.UnpackTo(&outMsg);
    }

    // Not an Any: treat as a bare serialized T (e.g. raw_data set directly).
    outMsg.Clear();
    return outMsg.ParseFromString(raw);
  }

  static void unregisterCallback(const std::string& key, void* instance);

  static bool sendRequest(const std::string& requestTopic, const std::string& payload, std::string& outResponse, int timeoutMs = 5000);

  template <typename ReqT, typename ResT>
  static bool sendRequest(const std::string& requestTopic, const ReqT& payload, ResT& outResponse, int timeoutMs = 5000) {
    std::string payloadData = payload.SerializeAsString();
    std::string rawResponse;

    if (sendRequest(requestTopic, payloadData, rawResponse, timeoutMs)) {
      return tryUnpack(rawResponse, outResponse);
    }
    return false;
  }

  static bool replyToSender(const std::string& data);

  // Same dispatch rules as sendMessage above.
  template <typename T, typename std::enable_if<!std::is_pointer<T>::value && !std::is_array<T>::value, int>::type = 0>
  static bool replyToSender(const T& value) {
    if constexpr (DataSerializer<T>::is_specialized) {
      return instance().replyToSenderInternal(DataSerializer<T>::serialize(value));
    } else if constexpr (std::is_base_of<google::protobuf::Message, T>::value) {
      return instance().replyToSenderInternal(value);
    } else if constexpr (std::is_trivially_copyable<T>::value && std::is_standard_layout<T>::value) {
      return instance().replyToSenderInternal(std::string(reinterpret_cast<const char*>(&value), sizeof(T)));
    } else {
      static_assert(always_false_v<T>,
                    "replyToSender: unsupported payload type. Expected a protobuf Message, a DataSerializer-specialized "
                    "type, or a trivially copyable standard-layout struct.");
    }
  }

private:
  ConnectionManager(const ConnectionConfig& config);
  ~ConnectionManager();

  void resubscribeAll();
  static void registerInternal(const std::string& key, MessageCallback callback, void* instance);

  // Snapshot of m_instance taken under m_initMutex. Callers keep the returned
  // shared_ptr alive for the duration of their work, so a concurrent
  // shutdown() can't destroy the instance out from under them.
  static std::shared_ptr<ConnectionManager> getInstance();

  bool sendDataInternal(const std::string& key, const std::string_view& data);
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

  bool replyToSenderInternal(const std::string& data);
  bool replyToSenderInternal(const google::protobuf::Message& protobufMessage);

  void processingLoop();
  void handleMessage(const broker::BrokerPayload& msg);

  void performRegistration(const std::string& key, MessageCallback callback, void* instance);
  void performUnregistration(const std::string& key, void* instance);
  broker::BrokerPayload createControlEnvelope(const std::string_view& controlKey, const std::string& topic);

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

  std::chrono::steady_clock::time_point m_lastConnectionTime;

  static std::vector<std::tuple<std::string, MessageCallback, void*>> s_pendingMsgCallbacks;
};

#endif  // CONNECTIONMANAGER_H
