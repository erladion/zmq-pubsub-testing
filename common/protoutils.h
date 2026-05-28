#ifndef PROTOUTILS_H
#define PROTOUTILS_H

static zmq::message_t createZmqMsg(const google::protobuf::Message& protoMsg) {
  size_t size = protoMsg.ByteSizeLong();
  if (size == 0) {
    return zmq::message_t();
  }

  zmq::message_t zMsg(size);
  protoMsg.SerializeToArray(zMsg.data(), size);

  return zMsg;
}

#endif  // PROTOUTILS_H
