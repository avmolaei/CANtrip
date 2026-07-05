#include "MainWindow.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "../common/AVlabsCanBackend.h"

namespace cantrip {

namespace {

std::string toLowerId(const std::string& name) {
    std::string out = name;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

// Must match pcan2pcap.cpp's interfaceIdFor() exactly - this is how the app
// tells tshark/pcan2pcap which backend+channel to open.
QString interfaceIdFor(const ICanBackend& backend, const CanChannelInfo& channel) {
    return QString::fromStdString(backend.id() + "_" + toLowerId(channel.name));
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

    startStopButton_ = new QPushButton("Start Capture", central);

    controlsLayout->addWidget(channelCombo_);
    controlsLayout->addWidget(refreshButton_);
    controlsLayout->addWidget(bitrateCombo_);
    controlsLayout->addWidget(fdCheckBox_);
    controlsLayout->addWidget(dataBitrateCombo_);
    controlsLayout->addWidget(expertStringEdit_, /*stretch=*/1);
    controlsLayout->addWidget(importDbcButton_);
    controlsLayout->addWidget(dbcStatusLabel_);
    controlsLayout->addWidget(startStopButton_);

    frameTree_ = new QTreeWidget(central);
    frameTree_->setColumnCount(7);
    frameTree_->setHeaderLabels({"Time", "ID", "Flags", "DLC", "Data", "Message/Signal", "Value"});
    frameTree_->setColumnWidth(0, 90);
    frameTree_->setColumnWidth(1, 80);
    frameTree_->setColumnWidth(2, 100);
    frameTree_->setColumnWidth(4, 220);
    frameTree_->setColumnWidth(5, 160);

    rootLayout->addLayout(controlsLayout);
    rootLayout->addWidget(frameTree_, /*stretch=*/1);
    setCentralWidget(central);
    statusBar();

    updateFdControlsEnabled();

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshChannels);
    connect(fdCheckBox_, &QCheckBox::toggled, this, &MainWindow::updateFdControlsEnabled);
    connect(importDbcButton_, &QPushButton::clicked, this, &MainWindow::importDbc);
    connect(startStopButton_, &QPushButton::clicked, this, &MainWindow::toggleCapture);
    connect(&capture_, &TsharkCapture::frameReceived, this, &MainWindow::onFrameReceived);
    connect(&capture_, &TsharkCapture::errorOccurred, this, &MainWindow::onCaptureError);
    connect(&capture_, &TsharkCapture::stopped, this, &MainWindow::onCaptureStopped);

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

    frameTree_->clear();
    frameCount_ = 0;
    statusBar()->showMessage("Starting capture on " + channelCombo_->currentText() + "...");
    capture_.start(config);
    startStopButton_->setText("Stop Capture");
    channelCombo_->setEnabled(false);
}

void MainWindow::onFrameReceived(const DecodedCanFrame& frame) {
    ++frameCount_;

    const QString idText = QString("0x%1").arg(frame.id, frame.extended ? 8 : 3, 16, QChar('0')).toUpper();
    const QString dataText = QString::fromLatin1(frame.data.toHex(' ').toUpper());

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

    auto* item = new QTreeWidgetItem(frameTree_);
    item->setText(0, frame.timestamp);
    item->setText(1, idText);
    item->setText(2, flagsString(frame));
    item->setText(3, QString::number(frame.dlc));
    item->setText(4, dataText);
    if (message) item->setText(5, QString::fromStdString(message->Name()));

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

    frameTree_->addTopLevelItem(item);
    statusBar()->showMessage(QString("Captured %1 frame(s)").arg(frameCount_));
}

void MainWindow::onCaptureError(const QString& message) {
    statusBar()->showMessage(message.trimmed(), 8000);
}

void MainWindow::onCaptureStopped() {
    startStopButton_->setText("Start Capture");
    channelCombo_->setEnabled(true);
    statusBar()->showMessage("Capture stopped", 4000);
}

} // namespace cantrip
