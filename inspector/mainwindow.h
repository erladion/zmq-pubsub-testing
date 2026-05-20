#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <vector>
#include "inspectorworker.h"

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  MainWindow(QWidget* parent = nullptr);
  ~MainWindow();

private slots:

  void applyFilters();  // Replaces onFilterTextChanged
  void onSelectionChanged();
  void onNewPacket(const InspectorPacket& packet);

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

  QLineEdit* m_filterBar;

  QSet<QString> m_knownTopics;
  QPushButton* m_topicFilterButton;
  QMenu* m_topicMenu;
};

#endif