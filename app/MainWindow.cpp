#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QtAlgorithms>
#include <QVBoxLayout>
#include <QWidget>

#include "../common/AVlabsCanBackend.h"

namespace cantrip {

namespace {

// Must match pcan2pcap.cpp's sanitizeId()/interfaceIdFor() exactly - this is
// how the app tells tshark/pcan2pcap which backend+channel to open. Every
// non-alphanumeric character collapses to a single underscore: the ID is
// used both as a tshark -i argument and as a preference NAME in
// `-o extcap.<id>.<key>:<value>`, and tshark's preference parser breaks on
// a raw space in that name (hit this for real with PEAK's "PCAN-USB FD").
std::string sanitizeId(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

QString interfaceIdFor(const ICanBackend& backend, const CanChannelInfo& channel) {
    return QString::fromStdString(backend.id() + "_" + sanitizeId(channel.name));
}

constexpr const char* kTestInterfaceId = "cantrip_test";

QString flagsString(const DecodedCanFrame& f) {
    QStringList flags;
    flags << (f.extended ? "EXT" : "STD");
    if (f.fd) flags << "FD";
    if (f.brs) flags << "BRS";
    if (f.esi) flags << "ESI";
    if (f.rtr) flags << "RTR";
    return flags.join(" ");
}

void setItemGrayed(QTreeWidgetItem* item, bool grayed) {
    const QBrush brush = grayed ? QBrush(QColor(Qt::gray)) : QBrush();
    for (int col = 0; col < item->columnCount(); ++col) {
        item->setForeground(col, brush);
    }
    for (int i = 0; i < item->childCount(); ++i) {
        for (int col = 0; col < item->columnCount(); ++col) {
            item->child(i)->setForeground(col, brush);
        }
    }
}

// QTreeWidgetItem's default operator< compares column text as plain
// strings, which sorts "10" before "2" - wrong for numeric columns like DLC.
// ID/DLC/Data get their real values stashed in Qt::UserRole (see
// populateDecodedChildren) precisely so this can compare them properly;
// every other column (Time, Flags, Message/Signal) falls back to the
// default text compare, which is already correct for those.
class FrameTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        switch (column) {
            case 1: // ID
            case 3: // DLC
                return data(column, Qt::UserRole).toUInt() < other.data(column, Qt::UserRole).toUInt();
            case 4: // Data
                return data(column, Qt::UserRole).toByteArray() < other.data(column, Qt::UserRole).toByteArray();
            default:
                return text(column) < other.text(column);
        }
    }
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("CANtrip");
    resize(1000, 600);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    auto* controlsLayout = new QHBoxLayout();
    channelCombo_ = new QComboBox(central);
    channelCombo_->setMinimumWidth(260);
    refreshButton_ = new QPushButton("Refresh", central);

    bitrateCombo_ = new QComboBox(central);
    bitrateCombo_->addItem("125 kbit/s", 125000);
    bitrateCombo_->addItem("250 kbit/s", 250000);
    bitrateCombo_->addItem("500 kbit/s", 500000);
    bitrateCombo_->setCurrentIndex(2);
    bitrateCombo_->addItem("1 Mbit/s", 1000000);

    fdCheckBox_ = new QCheckBox("CAN FD", central);

    dataBitrateCombo_ = new QComboBox(central);
    dataBitrateCombo_->addItem("2 Mbit/s", 2000000);
    dataBitrateCombo_->addItem("5 Mbit/s", 5000000);

    expertStringEdit_ = new QLineEdit(central);
    expertStringEdit_->setPlaceholderText("Expert FD init string (optional, backend-specific)");

    importDbcButton_ = new QPushButton("Import DBC...", central);
    dbcStatusLabel_ = new QLabel("No DBC loaded", central);

    displayModeCombo_ = new QComboBox(central);
    displayModeCombo_->addItem("Waterfall (newest first)", static_cast<int>(DisplayMode::Waterfall));
    displayModeCombo_->addItem("Periodic (per-ID)", static_cast<int>(DisplayMode::Periodic));

    startStopButton_ = new QPushButton("Start Capture", central);

    controlsLayout->addWidget(channelCombo_);
    controlsLayout->addWidget(refreshButton_);
    controlsLayout->addWidget(bitrateCombo_);
    controlsLayout->addWidget(fdCheckBox_);
    controlsLayout->addWidget(dataBitrateCombo_);
    controlsLayout->addWidget(expertStringEdit_, /*stretch=*/1);
    controlsLayout->addWidget(importDbcButton_);
    controlsLayout->addWidget(dbcStatusLabel_);
    controlsLayout->addWidget(displayModeCombo_);
    controlsLayout->addWidget(startStopButton_);

    frameTree_ = new QTreeWidget(central);
    frameTree_->setColumnCount(7);
    frameTree_->setHeaderLabels({"Time", "ID", "Flags", "DLC", "Data", "Message/Signal", "Value"});
    frameTree_->setColumnWidth(0, 90);
    frameTree_->setColumnWidth(1, 80);
    frameTree_->setColumnWidth(2, 100);
    frameTree_->setColumnWidth(4, 220);
    frameTree_->setColumnWidth(5, 160);
    // Click any header to sort by that column; click again to flip
    // ascending/descending (built into QTreeWidget once enabled - the arrow
    // indicator in the header is drawn automatically). Default to newest
    // timestamp first, matching Waterfall mode's original always-on-top
    // behavior, until the user actually clicks a header.
    frameTree_->setSortingEnabled(true);
    frameTree_->sortByColumn(0, Qt::DescendingOrder);

    rootLayout->addLayout(controlsLayout);
    rootLayout->addWidget(frameTree_, /*stretch=*/1);
    setCentralWidget(central);

    // Plain addWidget (not addPermanentWidget) so both sit flush at the
    // bottom-left, in the order added - since nothing else ever calls
    // showMessage() anymore, there's no temporary-message overlay to worry
    // about hiding them.
    statusLed_ = new StatusLed(this);
    statusLed_->setCapturing(false);
    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLed_);
    statusBar()->addWidget(statusLabel_, /*stretch=*/1);

    updateFdControlsEnabled();

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshChannels);
    connect(fdCheckBox_, &QCheckBox::toggled, this, &MainWindow::updateFdControlsEnabled);
    connect(importDbcButton_, &QPushButton::clicked, this, &MainWindow::importDbc);
    connect(startStopButton_, &QPushButton::clicked, this, &MainWindow::toggleCapture);
    connect(displayModeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDisplayModeChanged);
    connect(&capture_, &TsharkCapture::frameReceived, this, &MainWindow::onFrameReceived);
    connect(&capture_, &TsharkCapture::errorOccurred, this, &MainWindow::onCaptureError);
    connect(&capture_, &TsharkCapture::stopped, this, &MainWindow::onCaptureStopped);

    connect(&staleTimer_, &QTimer::timeout, this, &MainWindow::checkStaleRows);
    staleTimer_.start(500);

    refreshChannels();
}

void MainWindow::updateFdControlsEnabled() {
    const bool fd = fdCheckBox_->isChecked();
    dataBitrateCombo_->setEnabled(fd);
    expertStringEdit_->setEnabled(fd);
}

void MainWindow::refreshChannels() {
    channelCombo_->clear();
    channels_.clear();

    for (auto& backend : probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            ChannelEntry entry;
            entry.interfaceId = interfaceIdFor(*backend, ch);
            entry.displayName = QString::fromStdString(backend->displayName()) + " " + QString::fromStdString(ch.name);
            channels_.push_back(entry);
        }
    }
    // Always available, even with zero vendor drivers installed - this is
    // how CANtrip's pipeline gets tested without any CAN hardware at hand.
    channels_.push_back({kTestInterfaceId, "CANtrip synthetic test source (no hardware needed)"});

    for (const ChannelEntry& entry : channels_) {
        channelCombo_->addItem(entry.displayName, entry.interfaceId);
    }
}

void MainWindow::importDbc() {
    const QString path = QFileDialog::getOpenFileName(this, "Import DBC", QString(), "DBC files (*.dbc)");
    if (path.isEmpty()) return;

    std::ifstream is(path.toStdString());
    if (!is) {
        QMessageBox::warning(this, "Import DBC", "Could not open file:\n" + path);
        return;
    }
    auto net = dbcppp::INetwork::LoadDBCFromIs(is);
    if (!net) {
        QMessageBox::warning(this, "Import DBC", "Failed to parse DBC file:\n" + path);
        return;
    }
    dbcNetwork_ = std::move(net);
    dbcStatusLabel_->setText(QFileInfo(path).fileName());
}

QString MainWindow::findTsharkExe() {
    QString found = QStandardPaths::findExecutable("tshark");
    if (!found.isEmpty()) return found;
    const QString defaultInstall = "C:/Program Files/Wireshark/tshark.exe";
    if (QFileInfo::exists(defaultInstall)) return defaultInstall;
    return "tshark"; // let QProcess try PATH and fail with a clear error if not found
}

void MainWindow::toggleCapture() {
    if (capture_.isRunning()) {
        capture_.stop();
        return;
    }

    if (channelCombo_->currentIndex() < 0) {
        QMessageBox::warning(this, "Start Capture", "No channel selected.");
        return;
    }

    TsharkCapture::Config config;
    config.tsharkPath = findTsharkExe();
    config.interfaceId = channelCombo_->currentData().toString();
    config.nominalBitrateBps = bitrateCombo_->currentData().toUInt();
    config.fd = fdCheckBox_->isChecked();
    config.dataBitrateBps = dataBitrateCombo_->currentData().toUInt();
    config.expertInitString = expertStringEdit_->text();

    resetDisplay();
    statusLabel_->setText("Starting capture on " + channelCombo_->currentText() + "...");
    statusLed_->setCapturing(true);
    capture_.start(config);
    startStopButton_->setText("Stop Capture");
    channelCombo_->setEnabled(false);
}

void MainWindow::onDisplayModeChanged() {
    displayMode_ = static_cast<DisplayMode>(displayModeCombo_->currentData().toInt());
    resetDisplay();
}

void MainWindow::resetDisplay() {
    frameTree_->clear();
    periodicRows_.clear();
    periodicErrorRows_.clear();
    frameCount_ = 0;
}

uint64_t MainWindow::frameKey(const DecodedCanFrame& frame) {
    return (static_cast<uint64_t>(frame.extended ? 1 : 0) << 32) | frame.id;
}

void MainWindow::populateDecodedChildren(QTreeWidgetItem* item, const DecodedCanFrame& frame) {
    const QString idText = QString("0x%1").arg(frame.id, frame.extended ? 8 : 3, 16, QChar('0')).toUpper();
    const QString dataText = QString::fromLatin1(frame.data.toHex(' ').toUpper());

    item->setText(1, idText);
    item->setText(2, flagsString(frame));
    item->setText(3, QString::number(frame.dlc));
    item->setText(4, dataText);
    // Real values for FrameTreeItem::operator< to sort on numerically/
    // byte-wise rather than as display text (see its definition above).
    item->setData(1, Qt::UserRole, frame.id);
    item->setData(3, Qt::UserRole, frame.dlc);
    item->setData(4, Qt::UserRole, frame.data);

    // dbcppp requires the DBC message ID convention where extended-frame IDs
    // have the 0x80000000 bit set (the format .dbc files themselves use for
    // BO_ entries) - Wireshark's can.id field, in contrast, is just the bare
    // 11/29-bit numeric ID with the extended-ness reported separately via
    // can_can_flags_xtd, so we OR the bit back in before matching.
    const uint32_t dbcId = frame.extended ? (frame.id | 0x80000000u) : frame.id;

    const dbcppp::IMessage* message = nullptr;
    if (dbcNetwork_) {
        for (const dbcppp::IMessage& msg : dbcNetwork_->Messages()) {
            if (msg.Id() == dbcId) { message = &msg; break; }
        }
    }

    qDeleteAll(item->takeChildren());
    item->setText(5, message ? QString::fromStdString(message->Name()) : QString());
    item->setText(6, QString());

    if (message) {
        // ISignal::Decode() requires at least 8 bytes and reads up to the
        // signal's own byte range - pad rather than pass frame.data
        // directly, since a short classic frame (dlc < 8) or a short FD
        // frame both give a buffer smaller than that minimum.
        uint8_t buf[64] = {};
        std::memcpy(buf, frame.data.constData(), static_cast<size_t>(std::min<qsizetype>(frame.data.size(), 64)));

        for (const dbcppp::ISignal& sig : message->Signals()) {
            const auto raw = sig.Decode(buf);
            const double phys = sig.RawToPhys(raw);
            auto* sigItem = new QTreeWidgetItem(item);
            sigItem->setText(5, QString::fromStdString(sig.Name()));
            sigItem->setText(6, QString::number(phys) + " " + QString::fromStdString(sig.Unit()));
        }
    }
}

void MainWindow::handleWaterfallFrame(const DecodedCanFrame& frame) {
    auto* item = new FrameTreeItem();
    item->setText(0, frame.timestamp);
    populateDecodedChildren(item, frame);
    // Newest-first is just the default sort (see the constructor's
    // sortByColumn call) - insertTopLevelItem's index here doesn't actually
    // matter once sorting is enabled, since Qt re-sorts on every insert.
    frameTree_->insertTopLevelItem(0, item);
}

void MainWindow::handlePeriodicFrame(const DecodedCanFrame& frame) {
    const uint64_t key = frameKey(frame);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto it = periodicRows_.find(key);
    if (it == periodicRows_.end()) {
        auto* item = new FrameTreeItem();
        item->setText(0, "--");
        frameTree_->addTopLevelItem(item);
        it = periodicRows_.emplace(key, PeriodicRowState{item, now, -1, false}).first;
    } else {
        const qint64 period = now - it->second.lastArrivalMs;
        it->second.periodMs = period;
        it->second.lastArrivalMs = now;
        it->second.item->setText(0, QString::number(period / 1000.0, 'f', 3) + " s");
    }

    if (it->second.stale) {
        it->second.stale = false;
        setItemGrayed(it->second.item, false);
    }

    populateDecodedChildren(it->second.item, frame);
}

void MainWindow::addErrorRow(const DecodedCanFrame& frame) {
    auto* item = new FrameTreeItem();
    item->setText(0, frame.timestamp);
    item->setText(5, "BUS ERROR: " + frame.errorDescription);
    for (int col = 0; col < item->columnCount(); ++col) {
        item->setForeground(col, QBrush(QColor(Qt::red)));
    }
    // Waterfall mode: every occurrence is its own row, newest first, exactly
    // like a data frame would be treated in this mode.
    frameTree_->insertTopLevelItem(0, item);
}

void MainWindow::handlePeriodicErrorFrame(const DecodedCanFrame& frame) {
    // Periodic mode: one row per distinct error type (there's no CAN ID to
    // key on the way data frames do), updated in place instead of spawning
    // a new row per occurrence - a chattering bus fault would otherwise
    // flood the exact same way the old plain waterfall log did.
    const QString key = frame.errorDescription;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto it = periodicErrorRows_.find(key);
    if (it == periodicErrorRows_.end()) {
        auto* item = new FrameTreeItem();
        item->setText(0, "--");
        item->setText(5, "BUS ERROR: " + frame.errorDescription);
        frameTree_->addTopLevelItem(item);
        it = periodicErrorRows_.insert(key, PeriodicRowState{item, now, -1, false});
    } else {
        const qint64 period = now - it->lastArrivalMs;
        it->periodMs = period;
        it->lastArrivalMs = now;
        it->item->setText(0, QString::number(period / 1000.0, 'f', 3) + " s");
    }

    it->stale = false;
    for (int col = 0; col < it->item->columnCount(); ++col) {
        it->item->setForeground(col, QBrush(QColor(Qt::red)));
    }
}

void MainWindow::onFrameReceived(const DecodedCanFrame& frame) {
    ++frameCount_;

    if (frame.error) {
        if (displayMode_ == DisplayMode::Waterfall) {
            addErrorRow(frame);
        } else {
            handlePeriodicErrorFrame(frame);
        }
    } else if (displayMode_ == DisplayMode::Waterfall) {
        handleWaterfallFrame(frame);
    } else {
        handlePeriodicFrame(frame);
    }

    statusLabel_->setText(QString("Captured %1 frame(s)").arg(frameCount_));
}

void MainWindow::checkStaleRows() {
    if (displayMode_ != DisplayMode::Periodic) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto& [key, state] : periodicRows_) {
        if (!state.stale && (now - state.lastArrivalMs) > kStaleTimeoutMs) {
            state.stale = true;
            setItemGrayed(state.item, true);
        }
    }
    for (auto it = periodicErrorRows_.begin(); it != periodicErrorRows_.end(); ++it) {
        if (!it->stale && (now - it->lastArrivalMs) > kStaleTimeoutMs) {
            it->stale = true;
            setItemGrayed(it->item, true);
        }
    }
}

void MainWindow::onCaptureError(const QString& message) {
    statusLabel_->setText(message.trimmed());
}

void MainWindow::onCaptureStopped() {
    startStopButton_->setText("Start Capture");
    channelCombo_->setEnabled(true);
    statusLed_->setCapturing(false);
    statusLabel_->setText("Capture stopped");
}

} // namespace cantrip
