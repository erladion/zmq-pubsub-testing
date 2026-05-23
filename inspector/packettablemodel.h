#ifndef PACKETTABLEMODEL_H
#define PACKETTABLEMODEL_H

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

#include "datamodel.h"

class PacketTableModel : public QAbstractTableModel {
  Q_OBJECT
public:
  PacketTableModel(const std::vector<InspectorPacket>& history, QObject* parent = nullptr);
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void packetAdded();  // Call this when vector size increases
private:
  const std::vector<InspectorPacket>& m_history;
};

// 2. THE HIGH-SPEED FILTER
class PacketFilterProxyModel : public QSortFilterProxyModel {
  Q_OBJECT
public:
  PacketFilterProxyModel(QObject* parent = nullptr);

  QSet<QString> allowedTopics;
  QString searchText;

  void updateFilters(const QString& text, const QSet<QString>& topics);

protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
};
#endif  // PACKETTABLEMODEL_H
