#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPixmap>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QtAlgorithms>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "AscLogWriter.h"
#include "CanControllerDialog.h"
#include "CsvLogWriter.h"
#include "LogFilenameTemplate.h"
#include "Version.h"

namespace cantrip {

namespace {

// Must match can2pcap.cpp's sanitizeId()/interfaceIdFor() exactly - this is
// how the app tells tshark/can2pcap which backend+channel to open. Every
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

QString interfaceIdFor(const IAvlabsCanBackend& backend, const CanChannelInfo& channel) {
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

    ribbon_ = new QTabWidget(this);
    ribbon_->setMaximumHeight(110);
    ribbon_->addTab(buildHomeTab(), "Home");
    ribbon_->addTab(buildHardwareTab(), "Hardware");
    ribbon_->addTab(buildAnalysisTab(), "Analysis && Measurement");
    ribbon_->addTab(buildLoggingTab(), "Logging");
    ribbon_->addTab(buildStimulationTab(), "Stimulation");
    ribbon_->addTab(buildAboutTab(), "About...");

    frameTree_ = new QTreeWidget(this);
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

    graphWindows_ = new GraphWindowContainer(&signalHistory_, this);

    contentStack_ = new QStackedWidget(this);
    contentStack_->addWidget(frameTree_);
    contentStack_->addWidget(graphWindows_);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->addWidget(ribbon_);
    rootLayout->addWidget(contentStack_, /*stretch=*/1);
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

    connect(&capture_, &TsharkCapture::frameReceived, this, &MainWindow::onFrameReceived);
    connect(&capture_, &TsharkCapture::errorOccurred, this, &MainWindow::onCaptureError);
    connect(&capture_, &TsharkCapture::stopped, this, &MainWindow::onCaptureStopped);

    // A replay feeds the exact same frame handler a live capture does -
    // Trace/Graph views can't tell the difference, by design.
    connect(&replaySource_, &LogReplaySource::frameReceived, this, &MainWindow::onFrameReceived);
    connect(&replaySource_, &LogReplaySource::errorOccurred, this, &MainWindow::onCaptureError);
    connect(&replaySource_, &LogReplaySource::stopped, this, &MainWindow::onReplayStopped);

    // "Clear Graph" only clears plotted data/history, not axis layout -
    // GraphView doesn't own SignalHistoryStore, so it just requests this
    // rather than clearing both itself. Every window shares one
    // SignalHistoryStore, so a clear requested by any single window has to
    // apply to all of them - otherwise the others would keep showing stale
    // points against reset history.
    connect(graphWindows_, &GraphWindowContainer::clearRequested, this, [this]() {
        signalHistory_.reset();
        graphWindows_->clearAllData();
    });

    connect(&staleTimer_, &QTimer::timeout, this, &MainWindow::checkStaleRows);
    staleTimer_.start(500);

    connect(&displayFlushTimer_, &QTimer::timeout, this, &MainWindow::flushPendingDisplay);
    displayIntervalMs_ = displayRateCombo_->currentData().toInt();
    if (displayIntervalMs_ > 0) displayFlushTimer_.start(displayIntervalMs_);

    refreshChannels();
}

QWidget* MainWindow::buildHomeTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* captureGroup = new QGroupBox("Capture", page);
    auto* captureLayout = new QHBoxLayout(captureGroup);
    startButton_ = new QPushButton("Start", captureGroup);
    stopButton_ = new QPushButton("Stop", captureGroup);
    stopButton_->setEnabled(false);
    captureLayout->addWidget(startButton_);
    captureLayout->addWidget(stopButton_);

    auto* displayGroup = new QGroupBox("Display", page);
    auto* displayLayout = new QVBoxLayout(displayGroup);
    waterfallRadio_ = new QRadioButton("Waterfall", displayGroup);
    periodicRadio_ = new QRadioButton("Periodic", displayGroup);
    waterfallRadio_->setChecked(true);
    displayLayout->addWidget(waterfallRadio_);
    displayLayout->addWidget(periodicRadio_);

    auto* rateGroup = new QGroupBox("Display Rate", page);
    auto* rateLayout = new QVBoxLayout(rateGroup);
    displayRateCombo_ = new QComboBox(rateGroup);
    // A busy real bus can produce far more frames/sec than CANtrip can fully
    // decode and paint into the Trace/Graph views in real time - reproduced
    // for real (freeze + runaway memory) via ProcDump + cdb. This throttles
    // how often the UI actually gets touched; full decode still happens for
    // whichever frame is shown, just not every single one on a fast bus.
    // Defaults to 30 Hz rather than Unlimited since the freeze is a real,
    // confirmed risk, not a hypothetical one.
    displayRateCombo_->addItem("Unlimited (may freeze on a busy bus)", 0);
    displayRateCombo_->addItem("30 Hz", 33);
    displayRateCombo_->addItem("10 Hz", 100);
    displayRateCombo_->addItem("5 Hz", 200);
    displayRateCombo_->setCurrentIndex(1);
    rateLayout->addWidget(displayRateCombo_);

    auto* configGroup = new QGroupBox("Configuration", page);
    auto* configLayout = new QHBoxLayout(configGroup);
    saveRuneButton_ = new QPushButton("Save Rune...", configGroup);
    loadRuneButton_ = new QPushButton("Load Rune...", configGroup);
    configLayout->addWidget(saveRuneButton_);
    configLayout->addWidget(loadRuneButton_);

    layout->addWidget(captureGroup);
    layout->addWidget(displayGroup);
    layout->addWidget(rateGroup);
    layout->addWidget(configGroup);
    layout->addStretch(1);

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopCapture);
    connect(waterfallRadio_, &QRadioButton::toggled, this, &MainWindow::onDisplayModeChanged);
    connect(periodicRadio_, &QRadioButton::toggled, this, &MainWindow::onDisplayModeChanged);
    connect(displayRateCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDisplayRateChanged);
    connect(saveRuneButton_, &QPushButton::clicked, this, &MainWindow::saveRune);
    connect(loadRuneButton_, &QPushButton::clicked, this, &MainWindow::loadRune);

    return page;
}

QWidget* MainWindow::buildHardwareTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* hwGroup = new QGroupBox("Network Hardware", page);
    auto* hwLayout = new QHBoxLayout(hwGroup);
    channelCombo_ = new QComboBox(hwGroup);
    channelCombo_->setMinimumWidth(260);
    refreshButton_ = new QPushButton("Refresh", hwGroup);
    hwLayout->addWidget(channelCombo_);
    hwLayout->addWidget(refreshButton_);

    auto* controllerGroup = new QGroupBox("Bus Configuration", page);
    auto* controllerLayout = new QHBoxLayout(controllerGroup);
    canControllerButton_ = new QPushButton("CAN Controller...", controllerGroup);
    controllerLayout->addWidget(canControllerButton_);

    auto* autoDetectGroup = new QGroupBox("Autodetect Bus Config", page);
    auto* autoDetectLayout = new QHBoxLayout(autoDetectGroup);
    autoDetectButton_ = new QPushButton("Click to check", autoDetectGroup);
    autoDetectLayout->addWidget(autoDetectButton_);

    layout->addWidget(hwGroup);
    layout->addWidget(controllerGroup);
    layout->addWidget(autoDetectGroup);
    layout->addStretch(1);

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshChannels);
    connect(canControllerButton_, &QPushButton::clicked, this, &MainWindow::openCanController);
    connect(autoDetectButton_, &QPushButton::clicked, this, &MainWindow::autoDetectBusConfig);

    return page;
}

QWidget* MainWindow::buildAnalysisTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* dbGroup = new QGroupBox("CAN Database", page);
    auto* dbLayout = new QHBoxLayout(dbGroup);
    importDbcButton_ = new QPushButton("Import DBC...", dbGroup);
    dbLayout->addWidget(importDbcButton_);

    dbcStatusLabel_ = new QLabel("No DBC loaded...", page);
    dbcStatusLabel_->setStyleSheet("color: red;");

    auto* viewGroup = new QGroupBox("Views", page);
    auto* viewLayout = new QHBoxLayout(viewGroup);
    auto* traceButton = new QPushButton("CAN Trace", viewGroup);
    auto* graphicsButton = new QPushButton("Graphics", viewGroup);
    viewLayout->addWidget(traceButton);
    viewLayout->addWidget(graphicsButton);

    layout->addWidget(dbGroup);
    layout->addWidget(dbcStatusLabel_);
    layout->addWidget(viewGroup);
    layout->addStretch(1);

    connect(importDbcButton_, &QPushButton::clicked, this, &MainWindow::importDbc);
    connect(traceButton, &QPushButton::clicked, this, [this]() { contentStack_->setCurrentWidget(frameTree_); });
    connect(graphicsButton, &QPushButton::clicked, this, [this]() { contentStack_->setCurrentWidget(graphWindows_); });

    return page;
}

QWidget* MainWindow::buildStimulationTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QVBoxLayout(page);
    auto* label = new QLabel("coming soon :3", page);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    return page;
}

QWidget* MainWindow::buildLoggingTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* loggingGroup = new QGroupBox("Logging", page);
    auto* loggingLayout = new QHBoxLayout(loggingGroup);
    startLoggingButton_ = new QPushButton("Start", loggingGroup);
    stopLoggingButton_ = new QPushButton("Stop", loggingGroup);
    stopLoggingButton_->setEnabled(false);
    loggingLayout->addWidget(startLoggingButton_);
    loggingLayout->addWidget(stopLoggingButton_);

    auto* outputGroup = new QGroupBox("Output File", page);
    auto* outputLayout = new QVBoxLayout(outputGroup);
    outputFileButton_ = new QPushButton("Output file...", outputGroup);
    outputFileLabel_ = new QLabel("(default location)", outputGroup);
    // Elided to a single line rather than word-wrapped: a wrapped label
    // grows taller with a long filename, and since every ribbon group in
    // this row shares the tab's fixed max height, that extra height came
    // out of its siblings - squeezing the Format radio buttons down to
    // near-unreadable. A fixed-width single line keeps this group's height
    // constant regardless of filename length.
    outputFileLabel_->setFixedWidth(170);
    outputLayout->setContentsMargins(6, 4, 6, 4);
    outputLayout->setSpacing(2);
    outputLayout->addWidget(outputFileButton_);
    outputLayout->addWidget(outputFileLabel_);

    auto* formatGroup = new QGroupBox("Format", page);
    auto* formatLayout = new QVBoxLayout(formatGroup);
    ascFormatRadio_ = new QRadioButton(".asc", formatGroup);
    csvFormatRadio_ = new QRadioButton(".csv", formatGroup);
    mf4FormatRadio_ = new QRadioButton(".mf4", formatGroup);
    ascFormatRadio_->setChecked(true);
    mf4FormatRadio_->setEnabled(false);
    mf4FormatRadio_->setToolTip("Coming soon");
    // Default QVBoxLayout margins/spacing push 3 stacked radios + the
    // groupbox title just past the ribbon's fixed max height (see
    // MainWindow's constructor - ribbon_->setMaximumHeight(110)), which
    // forced Qt to compress this entire row, including the sibling Output
    // File group. Tightened rather than raising the ribbon height globally,
    // since every other tab already fits comfortably at 110px.
    formatLayout->setContentsMargins(6, 4, 6, 4);
    formatLayout->setSpacing(2);
    formatLayout->addWidget(ascFormatRadio_);
    formatLayout->addWidget(csvFormatRadio_);
    formatLayout->addWidget(mf4FormatRadio_);

    auto* optionsGroup = new QGroupBox("Other Options", page);
    auto* optionsLayout = new QHBoxLayout(optionsGroup);
    loggingOptionsButton_ = new QPushButton("Other options...", optionsGroup);
    optionsLayout->addWidget(loggingOptionsButton_);

    auto* replayGroup = new QGroupBox("Log Replay", page);
    auto* replayLayout = new QHBoxLayout(replayGroup);
    startReplayButton_ = new QPushButton("Start Replay...", replayGroup);
    stopReplayButton_ = new QPushButton("Stop Replay", replayGroup);
    stopReplayButton_->setEnabled(false);
    replayLayout->addWidget(startReplayButton_);
    replayLayout->addWidget(stopReplayButton_);

    layout->addWidget(loggingGroup);
    layout->addWidget(outputGroup);
    layout->addWidget(formatGroup);
    layout->addWidget(optionsGroup);
    layout->addWidget(replayGroup);
    layout->addStretch(1);

    connect(startLoggingButton_, &QPushButton::clicked, this, &MainWindow::startLogging);
    connect(stopLoggingButton_, &QPushButton::clicked, this, &MainWindow::stopLogging);
    connect(outputFileButton_, &QPushButton::clicked, this, &MainWindow::chooseLogOutputFile);
    connect(loggingOptionsButton_, &QPushButton::clicked, this, &MainWindow::openLoggingOptions);
    connect(startReplayButton_, &QPushButton::clicked, this, &MainWindow::startReplay);
    connect(stopReplayButton_, &QPushButton::clicked, this, &MainWindow::stopReplay);

    return page;
}

QString MainWindow::resolveMessageName(const DecodedCanFrame& frame) const {
    // Same DBC id convention as populateDecodedChildren() above - kept as
    // its own small lookup rather than sharing code with that function,
    // since populateDecodedChildren also needs the full IMessage* to walk
    // ISignal::Decode() over, not just the name.
    if (!dbcNetwork_) return QString();
    const uint32_t dbcId = frame.extended ? (frame.id | 0x80000000u) : frame.id;
    auto it = messageById_.find(dbcId);
    return it != messageById_.end() ? QString::fromStdString(it->second->Name()) : QString();
}

QString MainWindow::currentLogExtension() const {
    if (csvFormatRadio_->isChecked()) return "csv";
    return "asc"; // default/fallback - mf4 is disabled, so this is always asc or csv in practice
}

QString MainWindow::currentBusNameForFilename() const {
    if (channelCombo_->currentIndex() < 0) return "bus";
    // The synthetic source's real display name ("CANtrip synthetic test
    // source (no hardware needed)") is deliberately verbose for the channel
    // picker, but that's the wrong thing to put in a filename - use the
    // AVlabs CAN backend's own short identifier instead, matching how a
    // real vendor channel's [bus] token is short too (e.g. "PEAK_System_...").
    if (channelCombo_->currentData().toString() == kTestInterfaceId) return "AVLBS_CAN";
    return channelCombo_->currentText();
}

std::unique_ptr<ILogWriter> MainWindow::makeLogWriter() const {
    auto resolver = [this](const DecodedCanFrame& frame) { return resolveMessageName(frame); };
    if (csvFormatRadio_->isChecked()) {
        return std::make_unique<CsvLogWriter>(resolver);
    }
    return std::make_unique<AscLogWriter>(resolver);
}

void MainWindow::chooseLogOutputFile() {
    const QString capturesDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/CANtrip/captures";
    QDir().mkpath(capturesDir);

    const QString busName = currentBusNameForFilename();
    const QString defaultName = expandLogFilenameTemplate(logFilenameTemplate_, busName) + "." + currentLogExtension();

    const QString path = QFileDialog::getSaveFileName(this, "Output File", capturesDir + "/" + defaultName,
        "Log files (*.asc *.csv)");
    if (path.isEmpty()) return;

    logOutputPath_ = path;
    logOutputPathExplicit_ = true;
    setOutputFileLabel(path);
}

void MainWindow::openLoggingOptions() {
    LoggingOptionsDialog dialog(this);
    dialog.setFilenameTemplate(logFilenameTemplate_);
    dialog.setPreviewBusName(currentBusNameForFilename());
    dialog.setMaxFileSizeMb(logMaxFileSizeMb_);
    dialog.setExistingFilePolicy(logExistingFilePolicy_);
    dialog.setAutoStartWithCapture(logAutoStartWithCapture_);

    if (dialog.exec() != QDialog::Accepted) return;

    logFilenameTemplate_ = dialog.filenameTemplate();
    logMaxFileSizeMb_ = dialog.maxFileSizeMb();
    logExistingFilePolicy_ = dialog.existingFilePolicy();
    logAutoStartWithCapture_ = dialog.autoStartWithCapture();
}

void MainWindow::setLoggingUiRunning(bool running) {
    startLoggingButton_->setEnabled(!running);
    stopLoggingButton_->setEnabled(running);
    outputFileButton_->setEnabled(!running);
    ascFormatRadio_->setEnabled(!running);
    csvFormatRadio_->setEnabled(!running);
}

void MainWindow::setOutputFileLabel(const QString& path) {
    const QString fileName = QFileInfo(path).fileName();
    outputFileLabel_->setText(outputFileLabel_->fontMetrics().elidedText(fileName, Qt::ElideMiddle, outputFileLabel_->width()));
    outputFileLabel_->setToolTip(path); // full path/name always available on hover, even when elided
}

void MainWindow::startLogging() {
    if (logging_) return;

    // Only reuse logOutputPath_ if the user explicitly chose it via "Output
    // file..." - otherwise regenerate fresh from the template every time
    // (new timestamp, current format radio), rather than silently reusing
    // whatever path happened to be left over from the previous session.
    QString path = logOutputPathExplicit_ ? logOutputPath_ : QString();
    if (path.isEmpty()) {
        const QString capturesDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/CANtrip/captures";
        QDir().mkpath(capturesDir);
        const QString busName = currentBusNameForFilename();
        path = capturesDir + "/" + expandLogFilenameTemplate(logFilenameTemplate_, busName) + "." + currentLogExtension();
    }

    if (QFileInfo::exists(path)) {
        switch (logExistingFilePolicy_) {
            case LoggingOptionsDialog::ExistingFilePolicy::Prompt: {
                const auto choice = QMessageBox::question(this, "Output File Exists",
                    path + " already exists. Overwrite it?", QMessageBox::Yes | QMessageBox::No);
                if (choice != QMessageBox::Yes) return;
                break;
            }
            case LoggingOptionsDialog::ExistingFilePolicy::AutoIncrement: {
                const QFileInfo info(path);
                int suffix = 1;
                QString candidate;
                do {
                    candidate = info.absolutePath() + "/" + info.completeBaseName() + QString("_%1.").arg(suffix++) + info.suffix();
                } while (QFileInfo::exists(candidate));
                path = candidate;
                break;
            }
            case LoggingOptionsDialog::ExistingFilePolicy::Overwrite:
                break;
        }
    }

    auto writer = makeLogWriter();
    QString error;
    if (!writer->open(path, &error)) {
        QMessageBox::warning(this, "Start Logging", error);
        return;
    }

    logWriter_ = std::move(writer);
    logOutputPath_ = path;
    logSplitSequence_ = 0;
    logging_ = true;
    setOutputFileLabel(path);
    setLoggingUiRunning(true);
}

void MainWindow::stopLogging() {
    if (!logging_) return;
    logWriter_->close();
    logWriter_.reset();
    logging_ = false;
    loggingAutoStarted_ = false;
    setLoggingUiRunning(false);
}

void MainWindow::rotateLogFileIfNeeded() {
    if (!logging_ || logMaxFileSizeMb_ <= 0) return;
    if (logWriter_->bytesWritten() < static_cast<qint64>(logMaxFileSizeMb_) * 1024 * 1024) return;

    logWriter_->close();
    ++logSplitSequence_;
    const QFileInfo info(logOutputPath_);
    const QString nextPath = info.absolutePath() + "/" + info.completeBaseName() + QString("_part%1.").arg(logSplitSequence_ + 1) + info.suffix();

    auto writer = makeLogWriter();
    QString error;
    if (!writer->open(nextPath, &error)) {
        // Nothing more we can do here without losing frames outright - stop
        // logging cleanly rather than silently drop data into a broken writer.
        logging_ = false;
        setLoggingUiRunning(false);
        QMessageBox::warning(this, "Logging", "Could not start a new log file after splitting: " + error);
        return;
    }
    logWriter_ = std::move(writer);
    logOutputPath_ = nextPath;
}

void MainWindow::setCaptureControlsEnabledForReplay(bool replaying) {
    startButton_->setEnabled(!replaying);
    channelCombo_->setEnabled(!replaying);
    startReplayButton_->setEnabled(!replaying);
    stopReplayButton_->setEnabled(replaying);
}

void MainWindow::startReplay() {
    if (replaySource_.isRunning() || capture_.isRunning()) return;

    const QString path = QFileDialog::getOpenFileName(this, "Log Replay", QString(), "Log files (*.asc *.csv)");
    if (path.isEmpty()) return;

    resetDisplay();
    QString error;
    if (!replaySource_.start(path, &error)) {
        QMessageBox::warning(this, "Log Replay", error);
        return;
    }

    statusLabel_->setText("Replaying " + QFileInfo(path).fileName() + "...");
    statusLed_->setCapturing(true);
    setCaptureControlsEnabledForReplay(true);
}

void MainWindow::stopReplay() {
    if (!replaySource_.isRunning()) return;
    replaySource_.stop();
    onReplayStopped();
}

void MainWindow::onReplayStopped() {
    setCaptureControlsEnabledForReplay(false);
    statusLed_->setCapturing(false);
    statusLabel_->setText("Replay finished");
}

QWidget* MainWindow::buildAboutTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* githubButton = new QPushButton("GitHub", page);
    auto* aboutButton = new QPushButton("About CANtrip...", page);
    layout->addWidget(githubButton);
    layout->addWidget(aboutButton);
    layout->addStretch(1);

    connect(githubButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/avmolaei/CANtrip"));
    });
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::showAboutDialog);

    return page;
}

void MainWindow::showAboutDialog() {
    QMessageBox box(this);
    box.setWindowTitle(QString("CANtrip - v%1 \"%2\"").arg(kVersion, kCodename));
    box.setIconPixmap(QPixmap(":/cantrip_source.png").scaledToWidth(96, Qt::SmoothTransformation));
    box.setTextFormat(Qt::RichText);
    box.setText(
        "<b>CANtrip</b><br>"
        "An open-source, free alternative to Vector CANalyzer for viewing "
        "and decoding CAN / CAN-FD bus traffic on Windows.<br><br>"
        "Created by Avesta Molaei.<br>"
        "https://github.com/avmolaei/CANtrip<br><br>"
        "Licensed under the GNU General Public License v3.0 (GPL-3.0). "
        "See the LICENSE file in the repository for the full text.<br><br>"
        "<small>Vector, CANalyzer, and CANoe are trademarks of Vector "
        "Informatik GmbH. CANtrip is an independent, unaffiliated "
        "open-source project and is not endorsed by or affiliated with "
        "Vector Informatik.</small>");
    box.exec();
}

void MainWindow::openCanController() {
    CanControllerDialog dialog(this);
    dialog.setConfig(busConfig_);
    if (dialog.exec() == QDialog::Accepted) {
        busConfig_ = dialog.config();
    }
}

void MainWindow::autoDetectBusConfig() {
    if (capture_.isRunning()) {
        QMessageBox::warning(this, "Autodetect Bus Config", "Stop the current capture first.");
        return;
    }
    if (channelCombo_->currentIndex() < 0) {
        QMessageBox::warning(this, "Autodetect Bus Config", "No channel selected.");
        return;
    }

    const QString can2pcapPath = findCan2pcapExe();
    if (can2pcapPath.isEmpty()) {
        QMessageBox::warning(this, "Autodetect Bus Config",
            "can2pcap.exe isn't installed in Wireshark's extcap folder - see the README.");
        return;
    }

    autoDetectButton_->setText("Checking...");
    autoDetectButton_->setEnabled(false);
    // BusAutoDetector::detectClassicBitrate() blocks synchronously for
    // roughly a second - repaint explicitly now, since the event loop won't
    // get a turn to show "Checking..." until this handler returns otherwise.
    autoDetectButton_->repaint();

    const QString interfaceId = channelCombo_->currentData().toString();
    const std::optional<uint32_t> bitrate = BusAutoDetector::detectClassicBitrate(can2pcapPath, interfaceId);

    if (bitrate) {
        busConfig_.fd = false;
        busConfig_.nominalBitrateBps = *bitrate;
        autoDetectButton_->setText(QString("%1 kbit/s").arg(*bitrate / 1000));
        // Reuse the same Save Rune flow the Home tab button uses, so a
        // successful detection isn't lost if nothing was loaded/saved yet.
        saveRune();
    } else {
        autoDetectButton_->setText("No config found");
    }
    autoDetectButton_->setEnabled(true);
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

bool MainWindow::loadDbcFile(const QString& path, QString* error) {
    std::ifstream is(path.toStdString());
    if (!is) {
        if (error) *error = "Could not open file:\n" + path;
        return false;
    }
    // dbcppp's boost::spirit x3 grammar writes its only diagnostic (which
    // line/token it choked on) to std::cerr, with no way to retrieve it
    // through LoadDBCFromIs's return value - redirect cerr for the duration
    // of the call so a parse failure can show the user *why*, not just that
    // it failed.
    std::ostringstream parseLog;
    std::streambuf* prevCerr = std::cerr.rdbuf(parseLog.rdbuf());
    auto net = dbcppp::INetwork::LoadDBCFromIs(is);
    std::cerr.rdbuf(prevCerr);
    if (!net) {
        if (error) {
            QString detail = QString::fromStdString(parseLog.str()).trimmed();
            *error = "Failed to parse DBC file:\n" + path;
            if (!detail.isEmpty()) *error += "\n\n" + detail;
        }
        return false;
    }
    dbcNetwork_ = std::move(net);
    dbcPath_ = path;

    // Built once here rather than linearly scanning dbcNetwork_->Messages()
    // in populateDecodedChildren() on every single received frame.
    messageById_.clear();
    for (const dbcppp::IMessage& msg : dbcNetwork_->Messages()) {
        messageById_[static_cast<uint32_t>(msg.Id())] = &msg;
    }

    dbcStatusLabel_->setText(QFileInfo(path).fileName() + " loaded!");
    dbcStatusLabel_->setStyleSheet("color: green;");
    return true;
}

void MainWindow::importDbc() {
    const QString path = QFileDialog::getOpenFileName(this, "Import DBC", QString(), "DBC files (*.dbc)");
    if (path.isEmpty()) return;

    QString error;
    if (!loadDbcFile(path, &error)) {
        QMessageBox::warning(this, "Import DBC", error);
    }
}

void MainWindow::saveRune() {
    const QString path = QFileDialog::getSaveFileName(this, "Save Rune", QString(), "Rune files (*.rune)");
    if (path.isEmpty()) return;

    RuneConfig config;
    config.channelDisplayName = channelCombo_->currentIndex() >= 0 ? channelCombo_->currentText() : QString();
    config.busConfig = busConfig_;
    config.displayMode = displayMode_ == DisplayMode::Periodic ? RuneDisplayMode::Periodic : RuneDisplayMode::Waterfall;
    config.displayRateMs = displayIntervalMs_;
    config.dbcPath = dbcPath_;
    config.graphWindows = graphWindows_->exportLayout();

    QString error;
    if (!saveRuneFile(path, config, &error)) {
        QMessageBox::warning(this, "Save Rune", error);
    }
}

void MainWindow::loadRune() {
    const QString path = QFileDialog::getOpenFileName(this, "Load Rune", QString(), "Rune files (*.rune)");
    if (path.isEmpty()) return;

    QString error;
    const std::optional<RuneConfig> config = loadRuneFile(path, &error);
    if (!config) {
        QMessageBox::warning(this, "Load Rune", error);
        return;
    }

    const int channelIndex = channelCombo_->findText(config->channelDisplayName);
    if (channelIndex < 0) {
        QMessageBox::warning(this, "Load Rune",
            "Channel not found among currently available hardware:\n" + config->channelDisplayName +
            "\n\nLeaving the current channel selection as-is.");
    } else {
        channelCombo_->setCurrentIndex(channelIndex);
    }

    busConfig_ = config->busConfig;

    displayMode_ = config->displayMode == RuneDisplayMode::Periodic ? DisplayMode::Periodic : DisplayMode::Waterfall;
    periodicRadio_->setChecked(displayMode_ == DisplayMode::Periodic);
    waterfallRadio_->setChecked(displayMode_ == DisplayMode::Waterfall);

    const int rateIndex = displayRateCombo_->findData(config->displayRateMs);
    if (rateIndex >= 0) displayRateCombo_->setCurrentIndex(rateIndex);

    if (!config->dbcPath.isEmpty()) {
        QString dbcError;
        if (!loadDbcFile(config->dbcPath, &dbcError)) {
            QMessageBox::warning(this, "Load Rune", "Could not reload the DBC referenced by this rune:\n" + dbcError);
        }
    }

    graphWindows_->reset();
    graphWindows_->importLayout(config->graphWindows);
}

QString MainWindow::findTsharkExe() {
    QString found = QStandardPaths::findExecutable("tshark");
    if (!found.isEmpty()) return found;
    const QString defaultInstall = "C:/Program Files/Wireshark/tshark.exe";
    if (QFileInfo::exists(defaultInstall)) return defaultInstall;
    return "tshark"; // let QProcess try PATH and fail with a clear error if not found
}

QString MainWindow::findCan2pcapExe() {
    // can2pcap.exe must already be installed in Wireshark's personal extcap
    // folder for CANtrip to capture at all (see README) - reusing that exact
    // path here (rather than looking next to cantrip.exe, which only holds
    // true in a packaged release, not the raw build tree) means autodetect
    // fails the same way capturing itself would if that step was skipped,
    // instead of needing its own separate "can't find it" case.
    const QString extcapPath = qEnvironmentVariable("APPDATA") + "/Wireshark/extcap/can2pcap.exe";
    return QFileInfo::exists(extcapPath) ? extcapPath : QString();
}

void MainWindow::startCapture() {
    if (capture_.isRunning()) return;

    if (channelCombo_->currentIndex() < 0) {
        QMessageBox::warning(this, "Start Capture", "No channel selected.");
        return;
    }

    TsharkCapture::Config config;
    config.tsharkPath = findTsharkExe();
    config.interfaceId = channelCombo_->currentData().toString();
    config.nominalBitrateBps = busConfig_.nominalBitrateBps;
    config.fd = busConfig_.fd;
    config.dataBitrateBps = busConfig_.dataBitrateBps;
    config.nomBrp = busConfig_.nominalTiming.brp;
    config.nomTseg1 = busConfig_.nominalTiming.tseg1;
    config.nomTseg2 = busConfig_.nominalTiming.tseg2;
    config.nomSjw = busConfig_.nominalTiming.sjw;
    config.dataBrp = busConfig_.dataTiming.brp;
    config.dataTseg1 = busConfig_.dataTiming.tseg1;
    config.dataTseg2 = busConfig_.dataTiming.tseg2;
    config.dataSjw = busConfig_.dataTiming.sjw;

    resetDisplay();
    statusLabel_->setText("Starting capture on " + channelCombo_->currentText() + "...");
    statusLed_->setCapturing(true);
    capture_.start(config);
    startButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    channelCombo_->setEnabled(false);
    // QFileDialog (used by importDbc()) runs its own modal, nested Windows
    // message loop - which still dispatches TsharkCapture's queued
    // readyRead-driven frame processing underneath it. On a busy real bus,
    // that reenters MainWindow::onFrameReceived potentially thousands of
    // times while the dialog just sits there waiting for a click, since
    // nothing paces it against the dialog's own loop - reproduced for real:
    // opening Import DBC mid-capture on a busy CAN bus froze the UI and
    // grew memory into the hundreds of MB. Simplest fix: don't allow that
    // combination at all rather than try to make frame processing safe
    // under an arbitrary nested loop.
    importDbcButton_->setEnabled(false);
    // Same nested-message-loop hazard as Import DBC above - Save/Load Rune
    // and Output File all use QFileDialog too.
    saveRuneButton_->setEnabled(false);
    loadRuneButton_->setEnabled(false);
    outputFileButton_->setEnabled(false);
    // Live capture and replay both ultimately drive the same
    // onFrameReceived() path - mutually exclusive, not simultaneous.
    startReplayButton_->setEnabled(false);

    if (logAutoStartWithCapture_ && !logging_) {
        startLogging();
        loggingAutoStarted_ = logging_; // startLogging() may have failed (e.g. bad path) - only true on real success
    }
}

void MainWindow::stopCapture() {
    if (!capture_.isRunning()) return;
    capture_.stop();
}

void MainWindow::onDisplayModeChanged() {
    displayMode_ = periodicRadio_->isChecked() ? DisplayMode::Periodic : DisplayMode::Waterfall;
    resetDisplay();
}

void MainWindow::resetDisplay() {
    frameTree_->clear();
    periodicRows_.clear();
    periodicErrorRows_.clear();
    pendingPeriodicFrames_.clear();
    pendingWaterfallFrames_.clear();
    frameCount_ = 0;
    signalHistory_.reset();
    // Non-destructive: clears plotted data but keeps axis/signal layout, so
    // setting up a graph once and then Start -> Stop -> Start again keeps
    // plotting onto the same axes instead of losing them every run. Use
    // graphWindows_->reset() instead (tears down axes entirely) for a
    // genuinely fresh setup, e.g. after importing a different DBC.
    graphWindows_->clearAllData();
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
        auto it = messageById_.find(dbcId);
        if (it != messageById_.end()) message = it->second;
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
            const QString signalName = QString::fromStdString(sig.Name());
            const QString unit = QString::fromStdString(sig.Unit());
            sigItem->setText(5, signalName);
            sigItem->setText(6, QString::number(phys) + " " + unit);

            // Feed the Graph view's data model - same decode work, just also
            // retained over time instead of thrown away once the row updates.
            const QString qualifiedName = QString::fromStdString(message->Name()) + "." + signalName;
            signalHistory_.recordSample(qualifiedName, unit, frame.timestamp.toDouble(), phys);
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

    // Logging always sees every frame, regardless of the display-rate
    // throttle below - that throttle only bounds how often the UI repaints,
    // it was never meant to drop data from the log (bus errors get the same
    // "never throttled" treatment for the same reason, a few lines down).
    // Channel hardcoded to 1 until multi-bus exists - see LogWriter.h.
    if (logging_) {
        logWriter_->writeFrame(1, frame);
        rotateLogFileIfNeeded();
    }

    if (frame.error) {
        // Bus errors are comparatively rare and important - always shown
        // immediately, never subject to the display-rate throttle below.
        if (displayMode_ == DisplayMode::Waterfall) {
            addErrorRow(frame);
        } else {
            handlePeriodicErrorFrame(frame);
        }
        statusLabel_->setText(QString("Captured %1 frame(s)").arg(frameCount_));
        return;
    }

    if (displayIntervalMs_ <= 0) {
        // Unlimited: exact previous behavior, no buffering.
        if (displayMode_ == DisplayMode::Waterfall) {
            handleWaterfallFrame(frame);
        } else {
            handlePeriodicFrame(frame);
        }
        statusLabel_->setText(QString("Captured %1 frame(s)").arg(frameCount_));
        return;
    }

    // Throttled: keep only the latest frame per row (Periodic) or queue it
    // (Waterfall) and let displayFlushTimer_ apply them at a bounded rate -
    // see flushPendingDisplay(). Full decode still happens for whichever
    // frame ends up displayed, just not for every single one on a fast bus.
    if (displayMode_ == DisplayMode::Waterfall) {
        pendingWaterfallFrames_.push_back(frame);
    } else {
        pendingPeriodicFrames_[frameKey(frame)] = frame;
    }
}

void MainWindow::flushPendingDisplay() {
    if (pendingPeriodicFrames_.empty() && pendingWaterfallFrames_.empty()) return;

    for (auto& [key, frame] : pendingPeriodicFrames_) {
        handlePeriodicFrame(frame);
    }
    pendingPeriodicFrames_.clear();

    for (const DecodedCanFrame& frame : pendingWaterfallFrames_) {
        handleWaterfallFrame(frame);
    }
    pendingWaterfallFrames_.clear();

    statusLabel_->setText(QString("Captured %1 frame(s)").arg(frameCount_));
}

void MainWindow::onDisplayRateChanged() {
    displayIntervalMs_ = displayRateCombo_->currentData().toInt();
    displayFlushTimer_.stop();
    // Flush anything already buffered rather than stranding it under the
    // old rate/mode - harmless no-op if nothing is pending.
    flushPendingDisplay();
    if (displayIntervalMs_ > 0) {
        displayFlushTimer_.start(displayIntervalMs_);
    }
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
    startButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    channelCombo_->setEnabled(true);
    importDbcButton_->setEnabled(true);
    saveRuneButton_->setEnabled(true);
    loadRuneButton_->setEnabled(true);
    startReplayButton_->setEnabled(true);
    if (!logging_) outputFileButton_->setEnabled(true); // stays disabled if the user is still manually logging
    statusLed_->setCapturing(false);
    statusLabel_->setText("Capture stopped");

    // Only stop a log that Start Capture itself started - a log the user
    // started independently (not via the auto-start option) keeps running
    // past the end of this particular capture.
    if (loggingAutoStarted_) stopLogging();
}

} // namespace cantrip
