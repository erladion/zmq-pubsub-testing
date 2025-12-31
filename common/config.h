#ifndef CONFIG_H
#define CONFIG_H

enum class ProtocolType { ZMQ, GRPC };

struct ConnectionConfig {
  std::string address;
  std::string clientId;
  ProtocolType protocol = ProtocolType::ZMQ;

  int keepAliveTime = 10000;
  int keepAliveTimeout = 5000;
  int compressionAlgorithm = 2;  // GZIP
};

#endif  // CONFIG_H
