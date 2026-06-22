#ifndef QTCONNECTIONADAPTER_H
#define QTCONNECTIONADAPTER_H

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include "connectionmanager.h"

template <>
struct DataSerializer<QString> {
  static constexpr bool is_specialized = true;
  static std::string serialize(const QString& value) { return value.toStdString(); }
  static QString deserialize(const std::string& bytes) { return QString::fromStdString(bytes); }
};

template <>
struct DataSerializer<QByteArray> {
  static constexpr bool is_specialized = true;
  static std::string serialize(const QByteArray& value) { return std::string(value.constData(), value.size()); }
  static QByteArray deserialize(const std::string& bytes) { return QByteArray(bytes.data(), static_cast<int>(bytes.size())); }
};

template <>
struct DataSerializer<QJsonObject> {
  static constexpr bool is_specialized = true;
  static std::string serialize(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString(); }
  static QJsonObject deserialize(const std::string& bytes) {
    return QJsonDocument::fromJson(QByteArray(bytes.data(), static_cast<int>(bytes.size()))).object();
  }
};

// Qt-facing wrapper over ConnectionManager. Topics are QString at the API edge
// and converted to std::string internally, so callers stay in Qt types.
// Callbacks are marshaled onto the context QObject's thread, so handlers run
// where it is safe to touch widgets.
class QtConnectionAdapter {
public:
  explicit QtConnectionAdapter(const ConnectionConfig& config) { ConnectionManager::init(config); }
  ~QtConnectionAdapter() { ConnectionManager::shutdown(); }

  // Prevent accidental copying which would trigger double-shutdowns
  QtConnectionAdapter(const QtConnectionAdapter&) = delete;
  QtConnectionAdapter& operator=(const QtConnectionAdapter&) = delete;

  template <typename T>
  static bool sendMessage(const QString& key, const T& payload) {
    return ConnectionManager::sendMessage(key.toStdString(), payload);
  }

  template <typename T>
  static bool replyToSender(const T& payload) {
    return ConnectionManager::replyToSender(payload);
  }

  // Blocks the calling thread for up to timeoutMs waiting on the reply - never call from the UI thread.
  // QString assumes UTF-8 text; use the templated overload for binary/protobuf payloads.
  static bool sendRequest(const QString& topic, const QString& payload, QString& outResponse, int timeoutMs = 5000) {
    std::string response;
    if (!ConnectionManager::sendRequest(topic.toStdString(), payload.toStdString(), response, timeoutMs)) {
      return false;
    }
    outResponse = QString::fromStdString(response);
    return true;
  }

  template <typename ReqT, typename ResT>
  static bool sendRequest(const QString& topic, const ReqT& payload, ResT& outResponse, int timeoutMs = 5000) {
    return ConnectionManager::sendRequest(topic.toStdString(), payload, outResponse, timeoutMs);
  }

  static void unregisterCallback(const QString& key, QObject* context) { ConnectionManager::unregisterCallback(key.toStdString(), context); }

  // Lambdas
  template <typename Callable>
  static void registerCallback(const QString& key, QObject* context, Callable func) {
    using ArgType = typename CallableTraits<Callable>::ArgType;
    using BaseT = typename std::decay<ArgType>::type;

    ConnectionManager::registerCallback(
        key.toStdString(),
        [context, func](const BaseT& payload) {
          QMetaObject::invokeMethod(
              context, [func, payload]() { func(payload); }, Qt::QueuedConnection);
        },
        context);
  }

  // Class Member Functions (1 Argument)
  template <typename ClassType, typename ArgType>
  static void registerCallback(const QString& key, ClassType* context, void (ClassType::*method)(ArgType)) {
    static_assert(std::is_base_of<QObject, ClassType>::value, "Context must inherit from QObject!");

    using BaseT = typename std::decay<ArgType>::type;

    ConnectionManager::registerCallback(
        key.toStdString(),
        [context, method](const BaseT& payload) {
          QMetaObject::invokeMethod(
              context, [context, method, payload]() { (context->*method)(payload); }, Qt::QueuedConnection);
        },
        context);
  }

  // Class Member Functions (0 Arguments)
  template <typename ClassType>
  static void registerCallback(const QString& key, ClassType* context, void (ClassType::*method)()) {
    static_assert(std::is_base_of<QObject, ClassType>::value, "Context must inherit from QObject!");

    ConnectionManager::registerCallback(
        key.toStdString(),
        [context, method]() {
          QMetaObject::invokeMethod(
              context, [context, method]() { (context->*method)(); }, Qt::QueuedConnection);
        },
        context);
  }
};

#endif  // QTCONNECTIONADAPTER_H
