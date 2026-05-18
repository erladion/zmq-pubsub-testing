#include "mainwindow.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QStringList>
#include <QVBoxLayout>

#include "hexutils.h"
#include "protoutils.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  // 1. Tell Qt about our custom struct for signals
  qRegisterMetaType<InspectorPacket>("SniffedPacket");

  setupUi();

  // 2. Spin up the ZeroMQ background thread
  m_worker = new InspectorWorker(this);

  // 3. Connect the threads safely
  connect(m_worker, &InspectorWorker::packetReceived, this, &MainWindow::onNewPacket, Qt::QueuedConnection);

  m_worker->start();
}

MainWindow::~MainWindow() {
  m_worker->stopWorker();
  m_worker->wait();
}

void MainWindow::onNewPacket(const InspectorPacket& packet) {
  m_packetHistory.push_back(packet);

  QScrollBar* scrollBar = m_packetTable->verticalScrollBar();
  bool isAtBottom = (scrollBar->value() == scrollBar->maximum());

  int row = m_packetTable->rowCount();
  m_packetTable->insertRow(row);
  m_packetTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(packet.timestamp)));
  m_packetTable->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(packet.senderId)));
  m_packetTable->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(packet.key)));
  m_packetTable->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(packet.topic)));

  // Optional: Auto-scroll
  if (isAtBottom) {
    m_packetTable->scrollToBottom();
  }
}

void MainWindow::onSelectionChanged() {
  QList<QTableWidgetItem*> selectedItems = m_packetTable->selectedItems();

  if (selectedItems.isEmpty()) {
    return;
  }

  int row = selectedItems.first()->row();

  if (row >= m_packetHistory.size()) {
    return;
  }

  const InspectorPacket& packet = m_packetHistory[row];

  m_hexDump->setPlainText(QString::fromStdString(HexUtils::generateHexDump(packet.rawMemory)));

  m_protoTree->clear();
  ProtoUtils::drawEnvelopeAndPayload(packet.parsedProto, m_protoTree);
}

void MainWindow::setupUi() {
  QSplitter* mainSplitter = new QSplitter(Qt::Vertical, this);

  m_packetTable = new QTableWidget(0, 4, this);
  m_packetTable->setHorizontalHeaderLabels({"Time", "Sender", "Key", "Topic"});
  m_packetTable->horizontalHeader()->setStretchLastSection(true);
  m_packetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_packetTable->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(m_packetTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
  m_protoTree = new QTreeWidget(this);
  m_protoTree->setHeaderLabels({"Field", "Value"});

  m_hexDump = new QTextEdit(this);
  m_hexDump->setFontFamily("Courier");  // Monospace for hex
  m_hexDump->setReadOnly(true);

  mainSplitter->addWidget(m_packetTable);
  mainSplitter->addWidget(m_protoTree);
  mainSplitter->addWidget(m_hexDump);

  setCentralWidget(mainSplitter);
  resize(1024, 768);
}