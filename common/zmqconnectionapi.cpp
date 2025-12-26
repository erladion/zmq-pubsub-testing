#include "zmqconnectionapi.h"
#include <cstring>
#include "zmqconnectionmanager.h"

// Re-map the C struct to our internal ZMQ config
int initConnection(const GrpcConfig* config) {
  if (!config || !config->address)
    return GRPC_ERROR_INVALID_ARGS;

  ConnectionConfig cfg;
  cfg.address = config->address;  // e.g., "tcp://127.0.0.1:5555"
  cfg.clientId = config->client_id ? config->client_id : "ZmqClient";

  // ZMQ doesn't use these, but we keep the struct compatible
  // cfg.keepAliveTime = config->keepalive_time_ms;

  ZmqConnectionManager::init(cfg);
  return GRPC_SUCCESS;
}

void shutdownConnection() {
  ZmqConnectionManager::shutdown();
}

// These just delegate to the Manager
int sendText(const char* topic, const char* text) {
  if (!topic || !text)
    return GRPC_ERROR_INVALID_ARGS;
  return ZmqConnectionManager::sendMessage(topic, text) ? GRPC_SUCCESS : GRPC_ERROR_NO_CONNECTION;
}

int sendData(const char* topic, const char* data, int len) {
  if (!topic || !data)
    return GRPC_ERROR_INVALID_ARGS;
  return ZmqConnectionManager::sendDataRaw(topic, data, len) ? GRPC_SUCCESS : GRPC_ERROR_NO_CONNECTION;
}

int sendFile(const char* topic, const char* filepath) {
  if (!topic || !filepath)
    return GRPC_ERROR_INVALID_ARGS;
  return ZmqConnectionManager::sendFile(topic, filepath) ? GRPC_SUCCESS : GRPC_ERROR_NO_CONNECTION;
}

void registerCallback(const char* topic, GrpcMessageCallback callback, void* userData) {
  if (!topic || !callback)
    return;
  ZmqConnectionManager::registerCallback(topic, [callback, userData, t = std::string(topic)](const std::string& data) {
    callback(t.c_str(), data.c_str(), (int)data.size(), userData);
  });
}

void registerFileCallback(const char* topic, GrpcFileCallback callback, void* userData) {
  if (!topic || !callback)
    return;
  ZmqConnectionManager::registerFileCallback(
      topic, [callback, userData, t = std::string(topic)](const std::string& path) { callback(t.c_str(), path.c_str(), userData); });
}

void registerStatusCallback(GrpcStatusCallback callback, void* userData) {
  if (!callback)
    return;
  ZmqConnectionManager::registerStatusCallback(
      [callback, userData](bool connected) { callback(connected ? GRPC_STATUS_CONNECTED : GRPC_STATUS_DISCONNECTED, userData); });
}
