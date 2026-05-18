#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>

#include <vector>
#include "inspectorworker.h"

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  MainWindow(QWidget* parent = nullptr);
  ~MainWindow();

private slots:
  void onNewPacket(const InspectorPacket& packet);
  void onSelectionChanged();

private:
  void setupUi();

  InspectorWorker* m_worker;
  std::vector<InspectorPacket> m_packetHistory;

  // The Wireshark Trinity
  QTableWidget* m_packetTable;  // Top Pane
  QTreeWidget* m_protoTree;     // Middle Pane
  QTextEdit* m_hexDump;         // Bottom Pane
};

#endif