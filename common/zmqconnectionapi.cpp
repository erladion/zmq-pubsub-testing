#include "connectionapi.h"
#include "zmqconnectionmanager.h"

#include <cstring>

int initConnection(const Connection_Config* config) {
  if (!config || !config->address) {
    return ERROR_INVALID_ARGS;
  }

  ConnectionConfig cfg;
  cfg.address = config->address;  // e.g., "tcp://127.0.0.1:5555"
  cfg.clientId = config->client_id ? config->client_id : "ZmqClient";

  // ZMQ doesn't use these, but we keep the struct compatible
  // cfg.keepAliveTime = config->keepalive_time_ms;

  ZmqConnectionManager::init(cfg);
  return SUCCESS;
}

void shutdownConnection() {
  ZmqConnectionManager::shutdown();
}

int sendMessage(const char* topic, const char* text) {
  if (!topic || !text) {
    return ERROR_INVALID_ARGS;
  }
  return ZmqConnectionManager::sendMessage(topic, text) ? SUCCESS : ERROR_NO_CONNECTION;
}

int sendData(const char* topic, const char* data, int len) {
  if (!topic || !data) {
    return ERROR_INVALID_ARGS;
  }
  return ZmqConnectionManager::sendDataRaw(topic, data, len) ? SUCCESS : ERROR_NO_CONNECTION;
}

int sendFile(const char* topic, const char* filepath) {
  if (!topic || !filepath) {
    return ERROR_INVALID_ARGS;
  }
  return ZmqConnectionManager::sendFile(topic, filepath) ? SUCCESS : ERROR_NO_CONNECTION;
}

void registerCallback(const char* topic, Message_Callback callback, void* userData) {
  if (!topic || !callback) {
    return;
  }
  ZmqConnectionManager::registerCallback(topic, [callback, userData, t = std::string(topic)](const std::string& data) {
    callback(t.c_str(), data.c_str(), (int)data.size(), userData);
  });
}

void registerFileCallback(const char* topic, File_Callback callback, void* userData) {
  if (!topic || !callback) {
    return;
  }
  ZmqConnectionManager::registerFileCallback(
      topic, [callback, userData, t = std::string(topic)](const std::string& path) { callback(t.c_str(), path.c_str(), userData); });
}

void registerStatusCallback(Status_Callback callback, void* userData) {
  if (!callback) {
    return;
  }
  ZmqConnectionManager::registerStatusCallback(
      [callback, userData](bool connected) { callback(connected ? STATUS_CONNECTED : STATUS_DISCONNECTED, userData); });
}
