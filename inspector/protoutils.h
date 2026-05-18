#ifndef PROTOUTILS_H
#define PROTOUTILS_H

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <memory>
#include "broker.pb.h"

namespace ProtoUtils {

// =====================================================================
// 1. RECURSIVE TREE RENDERER (Extracts fields without knowing the type)
// =====================================================================
inline void populateProtobufTree(const google::protobuf::Message& msg, QTreeWidgetItem* parentNode) {
  const google::protobuf::Descriptor* descriptor = msg.GetDescriptor();
  const google::protobuf::Reflection* reflection = msg.GetReflection();

  for (int i = 0; i < descriptor->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);

    // Skip empty optional fields to keep the UI clean
    if (!field->is_repeated() && !reflection->HasField(msg, field))
      continue;

    QTreeWidgetItem* fieldNode = new QTreeWidgetItem(parentNode);
    fieldNode->setText(0, QString::fromStdString(std::string(field->name())));

    // Handle nested messages (and arrays of nested messages)
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        int count = reflection->FieldSize(msg, field);
        fieldNode->setText(1, QString("[%1 items]").arg(count));
        for (int j = 0; j < count; ++j) {
          QTreeWidgetItem* itemNode = new QTreeWidgetItem(fieldNode);
          itemNode->setText(0, QString("[%1]").arg(j));
          populateProtobufTree(reflection->GetRepeatedMessage(msg, field, j), itemNode);
        }
      } else {
        fieldNode->setText(1, "{...}");
        populateProtobufTree(reflection->GetMessage(msg, field), fieldNode);
      }
    }
    // Handle standard types (Ints, Strings, Bools, etc)
    else {
      std::string valueStr;
      google::protobuf::TextFormat::PrintFieldValueToString(msg, field, -1, &valueStr);

      // Clean up trailing newlines from TextFormat
      if (!valueStr.empty() && valueStr.back() == '\n')
        valueStr.pop_back();
      fieldNode->setText(1, QString::fromStdString(valueStr));
    }
  }
}

// =====================================================================
// 2. DYNAMIC UNPACKER (Converts Any -> Real Object)
// =====================================================================
inline std::unique_ptr<google::protobuf::Message> dynamicallyUnpack(const broker::BrokerPayload& payload) {
  if (!payload.has_payload())
    return nullptr;

  std::string typeUrl = payload.payload().type_url();
  std::string typeName = typeUrl.substr(typeUrl.find_last_of('/') + 1);

  const google::protobuf::Descriptor* descriptor = google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(typeName);
  if (!descriptor)
    return nullptr;

  const google::protobuf::Message* prototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(descriptor);
  if (!prototype)
    return nullptr;

  std::unique_ptr<google::protobuf::Message> dynamicMsg(prototype->New());
  if (payload.payload().UnpackTo(dynamicMsg.get())) {
    return dynamicMsg;
  }
  return nullptr;
}

// =====================================================================
// 3. MAIN PUBLIC API (Called by SnifferWindow)
// =====================================================================
inline void drawEnvelopeAndPayload(const broker::BrokerPayload& envelope, QTreeWidget* treeWidget) {
  // 1. Draw the Outer Envelope
  QTreeWidgetItem* envelopeRoot = new QTreeWidgetItem(treeWidget);
  envelopeRoot->setText(0, "Broker Envelope");
  populateProtobufTree(envelope, envelopeRoot);
  envelopeRoot->setExpanded(true);

  // 2. Draw the Inner Payload (If it's a Protobuf)
  if (envelope.has_payload()) {
    auto dynamicPayload = dynamicallyUnpack(envelope);
    if (dynamicPayload) {
      QTreeWidgetItem* payloadRoot = new QTreeWidgetItem(treeWidget);
      payloadRoot->setText(0, QString::fromStdString(std::string(dynamicPayload->GetTypeName())));
      populateProtobufTree(*dynamicPayload, payloadRoot);
      payloadRoot->setExpanded(true);
    } else {
      QTreeWidgetItem* errorRoot = new QTreeWidgetItem(treeWidget);
      errorRoot->setText(0, "[Unknown Protobuf Schema]");
      errorRoot->setText(1, "Link .pb.cc file to view");
    }
  }
  // 3. Draw the Inner Payload (If it's raw C++ struct bytes / string)
  else if (!envelope.raw_data().empty()) {
    QTreeWidgetItem* rawRoot = new QTreeWidgetItem(treeWidget);
    rawRoot->setText(0, "Raw Payload");
    rawRoot->setText(1, QString("[%1 bytes]").arg(envelope.raw_data().size()));
  }
}

}  // namespace ProtoUtils

#endif