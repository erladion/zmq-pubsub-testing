#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

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

  // void toggleStatsView(); // TODO: Implement this when ready

private:
  void setupUi();
  void setupSysStatsView();

private:
  InspectorWorker* m_worker;
  std::vector<InspectorPacket> m_packetHistory;

  // The Wireshark Trinity
  QTableWidget* m_packetTable;  // Top Pane
  QTreeWidget* m_protoTree;     // Middle Pane
  QTextEdit* m_hexDump;         // Bottom Pane

  QDockWidget* m_statsDock;

  // Labels for the exact data your broker sends
  QLabel* m_brokerIdLabel;
  QLabel* m_uptimeLabel;
  QLabel* m_clientsLabel;
  QLabel* m_peersLabel;
  QLabel* m_msgsSecLabel;
  QLabel* m_kbSecLabel;
  QLabel* m_totalMsgsLabel;
};

#endif