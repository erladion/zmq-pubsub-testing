#include "mainwindow.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QStringList>
#include <QVBoxLayout>

#include "hexutils.h"
#include "protoutils.h"

#include "messagekeys.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  // 1. Tell Qt about our custom struct for signals
  qRegisterMetaType<InspectorPacket>("InspectorPacket");

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

  QTableWidgetItem* timeItem = new QTableWidgetItem(QString::fromStdString(packet.timestamp));
  QTableWidgetItem* senderItem = new QTableWidgetItem(QString::fromStdString(packet.senderId));
  QTableWidgetItem* keyItem = new QTableWidgetItem(QString::fromStdString(packet.key));
  QTableWidgetItem* topicItem = new QTableWidgetItem(QString::fromStdString(packet.topic));

  QColor rowColor;
  if (Keys::isControlMessage(packet.key)) {
    rowColor = QColor(40, 60, 255, 100);
  } else if (packet.key == Keys::SYS_STATS) {
    rowColor = QColor(128, 128, 0, 100);
  } else {
    // Normal data messages get the default color (or explicitly set one)
    // rowColor = QColor(50, 50, 50); // Optional: explicitly set normal row color
  }

  if (rowColor.isValid()) {
    QBrush brush(rowColor);
    timeItem->setBackground(brush);
    senderItem->setBackground(brush);
    keyItem->setBackground(brush);
    topicItem->setBackground(brush);
  }

  m_packetTable->setItem(row, 0, timeItem);
  m_packetTable->setItem(row, 1, senderItem);
  m_packetTable->setItem(row, 2, keyItem);
  m_packetTable->setItem(row, 3, topicItem);

  QString qTopic = QString::fromStdString(packet.topic);
  if (qTopic.isEmpty())
    qTopic = "[Empty]";  // Safety net for pure control messages

  // If we've never seen this topic before, add it to the UI!
  if (!m_knownTopics.contains(qTopic)) {
    m_knownTopics.insert(qTopic);

    QAction* action = new QAction(qTopic, this);
    action->setCheckable(true);
    action->setChecked(true);  // Default to checked (visible)

    m_topicMenu->addAction(action);

    // Re-run filters if the user toggles this new checkbox
    connect(action, &QAction::toggled, this, &MainWindow::applyFilters);
  }

  // ==========================================
  // APPLY FILTERS TO NEW ROW
  // ==========================================
  // Force the new row to immediately respect the current filters
  applyFilters();

  if (isAtBottom) {
    m_packetTable->scrollToBottom();
  }
  if (packet.topic == Keys::SYS_STATS) {
    broker::SystemStats statsMsg;

    if (packet.parsedProto.payload().UnpackTo(&statsMsg)) {
      // Update all labels
      m_brokerIdLabel->setText(QString("Broker ID: %1").arg(QString::fromStdString(statsMsg.broker_id())));
      m_uptimeLabel->setText(QString("Uptime: %1 s").arg(statsMsg.uptime_sec()));

      m_clientsLabel->setText(QString("Clients: %1").arg(statsMsg.clients_count()));
      m_peersLabel->setText(QString("Peers: %1").arg(statsMsg.peers_count()));

      m_msgsSecLabel->setText(QString("Msgs/sec: %1").arg(statsMsg.msgs_per_sec()));
      m_kbSecLabel->setText(QString("KB/sec: %1").arg(statsMsg.kb_per_sec(), 0, 'f', 2));  // 2 decimal places
      m_totalMsgsLabel->setText(QString("Total Msgs: %1").arg(statsMsg.total_msgs()));
    }
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
  QWidget* centralWidget = new QWidget(this);
  QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
  mainLayout->setContentsMargins(4, 4, 4, 4);

  // 1. Create the Top Bar Layout
  QHBoxLayout* topBarLayout = new QHBoxLayout();

  m_filterBar = new QLineEdit(this);
  m_filterBar->setPlaceholderText("Filter by Topic, Key, or Sender...");
  m_filterBar->setClearButtonEnabled(true);
  connect(m_filterBar, &QLineEdit::textChanged, this, &MainWindow::applyFilters);

  // 2. Create the Dropdown Button & Menu
  m_topicFilterButton = new QPushButton("Topic Filters", this);
  m_topicMenu = new QMenu(this);
  m_topicFilterButton->setMenu(m_topicMenu);  // Attach the menu to the button

  topBarLayout->addWidget(m_filterBar);
  topBarLayout->addWidget(m_topicFilterButton);

  // 3. Add to main layout
  mainLayout->addLayout(topBarLayout);

  // 3. Build the rest of your UI (Unchanged)
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
  m_hexDump->setFontFamily("Courier");
  m_hexDump->setReadOnly(true);

  mainSplitter->addWidget(m_packetTable);
  mainSplitter->addWidget(m_protoTree);
  mainSplitter->addWidget(m_hexDump);

  // 4. Stack them in the main layout
  mainLayout->addWidget(m_filterBar);
  mainLayout->addWidget(mainSplitter);

  // 5. Set the new master container
  setCentralWidget(centralWidget);
  resize(1024, 768);

  setupSysStatsView();
}

void MainWindow::setupSysStatsView() {
  // 1. Create the Docking Window
  m_statsDock = new QDockWidget("Live System Stats", this);

  // Lock it to the left or right sides so it doesn't mess up your top/bottom layout
  m_statsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

  // 2. Create the container widget and layout
  QWidget* dockContent = new QWidget();
  QVBoxLayout* layout = new QVBoxLayout(dockContent);

  // 3. Initialize the labels
  m_brokerIdLabel = new QLabel("Broker ID: --");
  m_brokerIdLabel->setWordWrap(true);
  m_brokerIdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_uptimeLabel = new QLabel("Uptime: -- s");
  m_clientsLabel = new QLabel("Clients: 0");
  m_peersLabel = new QLabel("Peers: 0");
  m_msgsSecLabel = new QLabel("Msgs/sec: 0");
  m_kbSecLabel = new QLabel("KB/sec: 0.00");
  m_totalMsgsLabel = new QLabel("Total Msgs: 0");

  // Optional: Style the high-priority metrics so they stand out
  QFont boldFont("Courier", 10, QFont::Bold);
  m_brokerIdLabel->setFont(boldFont);

  m_msgsSecLabel->setStyleSheet("color: #2ecc71; font-weight: bold;");  // Green

  // 4. Add them to the layout with some spacing lines
  layout->addWidget(m_brokerIdLabel);
  layout->addWidget(m_uptimeLabel);

  QFrame* line1 = new QFrame();
  line1->setFrameShape(QFrame::HLine);
  layout->addWidget(line1);

  layout->addWidget(m_clientsLabel);
  layout->addWidget(m_peersLabel);

  QFrame* line2 = new QFrame();
  line2->setFrameShape(QFrame::HLine);
  layout->addWidget(line2);

  layout->addWidget(m_msgsSecLabel);
  layout->addWidget(m_kbSecLabel);
  layout->addWidget(m_totalMsgsLabel);

  // Push everything to the top
  layout->addStretch();

  // 5. Attach to Dock and add to Main Window
  m_statsDock->setWidget(dockContent);
  addDockWidget(Qt::RightDockWidgetArea, m_statsDock);

  // (Optional) If you have a QMenu or QToolBar, you can add the toggle action:
  // ui->menuView->addAction(m_statsDock->toggleViewAction());
}

void MainWindow::applyFilters() {
  QString lowerText = m_filterBar->text().toLower();

  // 1. Build a fast lookup set of all CURRENTLY CHECKED topics
  QSet<QString> allowedTopics;
  for (QAction* action : m_topicMenu->actions()) {
    if (action->isChecked()) {
      allowedTopics.insert(action->text());
    }
  }

  // 2. Loop through every row
  for (int i = 0; i < m_packetTable->rowCount(); ++i) {
    QString sender = m_packetTable->item(i, 1)->text().toLower();
    QString key = m_packetTable->item(i, 2)->text().toLower();
    QString topic = m_packetTable->item(i, 3)->text();  // Keep case for exact match

    // Check Text Match
    bool textMatch = lowerText.isEmpty() || sender.contains(lowerText) || key.contains(lowerText) || topic.toLower().contains(lowerText);

    // Check Dropdown Match
    bool topicMatch = allowedTopics.contains(topic);

    // Hide row if it fails EITHER filter
    m_packetTable->setRowHidden(i, !(textMatch && topicMatch));
  }
}
