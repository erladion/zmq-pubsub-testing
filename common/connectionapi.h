#ifndef CONNECTIONAPI_H
#define CONNECTIONAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#ifdef BUILDING_DLL
#define CONN_API __declspec(dllexport)
#else
#define CONN_API __declspec(dllimport)
#endif
#else
#define CONN_API
#endif

typedef enum { PROTOCOL_ZMQ = 0, PROTOCOL_GRPC = 1 } Connection_Protocol;

typedef enum { SUCCESS = 0, ERROR_GENERIC = -1, ERROR_NO_CONNECTION = -2, ERROR_INVALID_ARGS = -3, ERROR_SEND_FAILED = -4 } Connection_Error_Code;

typedef enum { STATUS_DISCONNECTED = 0, STATUS_CONNECTING = 1, STATUS_CONNECTED = 2 } Connection_Status;

typedef enum { COMPRESS_NONE = 0, COMPRESS_DEFLATE = 1, COMPRESS_GZIP = 2 } Compression_Algorithm;

typedef struct {
  const char* address;    // e.g. "tcp://127.0.0.1:5555"
  const char* client_id;  // e.g. "Camera-1"

  Connection_Protocol protocol;

  int keepalive_time_ms;     // Default: 10000
  int keepalive_timeout_ms;  // Default: 5000
  Compression_Algorithm compression_algorithm;
} Connection_Config;

#define CONNECTION_CONFIG_DEFAULT \
  { NULL, NULL, PROTOCOL_ZMQ, 10000, 5000, COMPRESS_GZIP }

typedef void (*Message_Callback)(const char* topic, const char* data, int len, void* userData);
typedef void (*File_Callback)(const char* topic, const char* filepath, void* userData);
typedef void (*Status_Callback)(Connection_Status status, void* userData);

CONN_API int initConnection(const Connection_Config* config);
CONN_API void shutdownConnection();

CONN_API int sendData(const char* topic, const char* data, int len);
CONN_API int sendMessage(const char* topic, const char* text);
CONN_API int sendFile(const char* topic, const char* filepath);

CONN_API void registerCallback(const char* topic, Message_Callback callback, void* userData);
CONN_API void registerFileCallback(const char* topic, File_Callback callback, void* userData);
CONN_API void registerStatusCallback(Status_Callback callback, void* userData);

#ifdef __cplusplus
}
#endif

#endif  // CONNECTIONAPI_H
