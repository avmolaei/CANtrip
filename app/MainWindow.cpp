#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPixmap>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabBar>
#include <QTableWidget>
#include <QtAlgorithms>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "AscLogWriter.h"
#include "CanControllerDialog.h"
#include "CsvLogWriter.h"
#include "GatewayText.h"
#include "LogFilenameTemplate.h"
#include "TransmitMessageDialog.h"
#include "Version.h"

namespace cantrip {

namespace {

constexpr const char* kTestInterfaceId = kSyntheticInterfaceId;

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

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // An experimental build says so where it cannot be missed.
    setWindowTitle(QString(kVersion).contains("experimental") ? QString("CANtrip v%1 - %2").arg(kVersion, kCodename) : QString("CANtrip"));
    resize(1000, 600);
    connect(&gatewayTimer_, &QTimer::timeout, this, &MainWindow::pumpGateway);
    gatewayTimer_.start(20);

    udsClient_.setTransmitter(gateway_);
    diagnosticsView_ = new DiagnosticsView(&udsClient_, this);

    ribbon_ = new QTabWidget(this);
    ribbon_->setMaximumHeight(110);
    ribbon_->addTab(buildHomeTab(), "Home");
    ribbon_->addTab(buildHardwareTab(), "Hardware");
    ribbon_->addTab(buildAnalysisTab(), "Analysis && Measurement");
    ribbon_->addTab(buildLoggingTab(), "Logging");
    ribbon_->addTab(buildStimulationTab(), "Stimulation");
    ribbon_->addTab(diagnosticsView_->ribbonPage(), "Diagnostics");
    ribbon_->addTab(buildAboutTab(), "About...");

    // Start/Stop capture pinned into the tab bar's own top-left corner
    // (same row as the tab labels, sitting before "Home") rather than
    // living only on the Home tab's page - a capture running from, say,
    // the Stimulation tab otherwise had no visible way to stop it without
    // switching tabs first.
    ribbon_->setCornerWidget(buildPinnedToolbar(), Qt::TopLeftCorner);

    traceModel_ = new TraceModel(&dbcDecoder_, this);
    traceHeader_ = new TraceHeaderView(this);
    frameTree_ = new QTreeView(this);
    frameTree_->setHeader(traceHeader_);
    frameTree_->setModel(traceModel_);
    traceHeader_->applyBehavior(); // setHeader() turns section clicks off
    frameTree_->setUniformRowHeights(true);
    frameTree_->setColumnWidth(TraceModel::Time, 90);
    frameTree_->setColumnWidth(TraceModel::Id, 80);
    frameTree_->setColumnWidth(TraceModel::Flags, 100);
    frameTree_->setColumnWidth(TraceModel::Data, 220);
    frameTree_->setColumnWidth(TraceModel::Message, 160);
    connect(traceHeader_, &TraceHeaderView::sortRequested, traceModel_, &TraceModel::sort);
    connect(traceHeader_, &TraceHeaderView::filterEdited, this, [this](int column, const QString& text) {
        traceModel_->setColumnFilter(column, text);
        updateFrameCountStatus();
    });
    applyDefaultTraceSort();
    frameTree_->setColumnHidden(TraceModel::Bus, true); // shown once a second bus is in play

    graphWindows_ = new GraphWindowContainer(&signalHistory_, this);
    stimulationView_ = new StimulationView(this);
    busLoadView_ = new BusLoadView(&busLoadTracker_, this);
    busLoadTabs_ = new QTabWidget(this);
    busLoadTabs_->addTab(busLoadView_, "CAN 1");
    busLoadTabs_->tabBar()->setVisible(false);
    messageSender_.setBusTransmitter(gateway_);

    contentStack_ = new QStackedWidget(this);
    contentStack_->addWidget(frameTree_);
    contentStack_->addWidget(graphWindows_);
    contentStack_->addWidget(stimulationView_);
    contentStack_->addWidget(busLoadTabs_);
    contentStack_->addWidget(diagnosticsView_);

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

    // Bus Load tracks real bus-occupying traffic only - live capture and
    // CANtrip's own transmitted frames both really consume bus time.
    // Deliberately NOT replaySource_: a replayed log isn't occupying the
    // bus right now, so it must not count toward live bus load.
    connect(&capture_, &TsharkCapture::frameReceived, &busLoadTracker_, &BusLoadTracker::recordFrame);
    connect(&messageSender_, &MessageSender::frameSent, &busLoadTracker_, &BusLoadTracker::recordFrame);
    connect(busLoadButton_, &QPushButton::clicked, this, [this]() { contentStack_->setCurrentWidget(busLoadTabs_); });

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

    // Send Message frames flow through the exact same pipeline a live
    // capture does - logging, Trace/Graph display, decode - all for free.
    connect(&messageSender_, &MessageSender::frameSent, this, &MainWindow::onFrameReceived);
    connect(&messageSender_, &MessageSender::sendFailed, this, &MainWindow::onSendFailed);
    connect(clearReceivedButton_, &QPushButton::clicked, this, &MainWindow::onClearReceived);

    connect(stimulationView_, &StimulationView::newMessageRequested, this, &MainWindow::onNewMessage);
    connect(stimulationView_, &StimulationView::editMessageRequested, this, &MainWindow::onEditMessage);
    connect(stimulationView_, &StimulationView::sendNowRequested, this, &MainWindow::onSendNow);
    connect(stimulationView_, &StimulationView::deleteMessagesRequested, this, &MainWindow::onDeleteMessages);
    connect(stimulationView_, &StimulationView::clearAllRequested, this, &MainWindow::onClearAllMessages);
    connect(stimulationView_, &StimulationView::copyRequested, this, &MainWindow::onCopyMessages);
    connect(stimulationView_, &StimulationView::cutRequested, this, &MainWindow::onCutMessages);
    connect(stimulationView_, &StimulationView::pasteRequested, this, &MainWindow::onPasteMessages);

    // Selecting the Stimulation ribbon tab switches the content area to its
    // split view - the only ribbon tab whose selection has this side effect,
    // since the split view *is* what Stimulation mode means (unlike Analysis
    // & Measurement, where Trace/Graphics are separate explicit buttons).
    connect(ribbon_, &QTabWidget::currentChanged, this, &MainWindow::onRibbonTabChanged);

    connect(&staleTimer_, &QTimer::timeout, this, &MainWindow::checkStaleRows);
    staleTimer_.start(500);

    connect(&displayFlushTimer_, &QTimer::timeout, this, &MainWindow::flushPendingDisplay);
    displayIntervalMs_ = displayRateCombo_->currentData().toInt();
    if (displayIntervalMs_ > 0) displayFlushTimer_.start(displayIntervalMs_);

    refreshChannels();
}

QWidget* MainWindow::buildPinnedToolbar() {
    // No QGroupBox chrome here - this sits inline with the tab strip itself
    // (see QTabWidget::setCornerWidget in the constructor), which is much
    // shorter than a page full of grouped controls, so it needs to stay as
    // compact as a couple of plain buttons.
    auto* bar = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(4, 0, 4, 0);
    // Same ▶/⏸ convention as Start Sending/Stop Sending on the Stimulation
    // tab (see buildStimulationTab()), just larger - this is the primary
    // capture control, pinned where it's always reachable.
    startButton_ = new QPushButton(QString::fromUtf8("\xE2\x96\xB6 Start"), bar); // ▶
    stopButton_ = new QPushButton(QString::fromUtf8("\xE2\x8F\xB8 Stop"), bar); // ⏸
    stopButton_->setEnabled(false);
    QFont bigFont = startButton_->font();
    bigFont.setPointSize(bigFont.pointSize() + 1);
    bigFont.setBold(true);
    startButton_->setFont(bigFont);
    stopButton_->setFont(bigFont);
    // Sized via stylesheet padding, not setMinimumSize - a hard minimum
    // height fought QTabWidget's own tab-bar row height and got clipped at
    // the top instead of growing the row (confirmed for real, 2026-07-14).
    // Padding lets the button ask for what it needs and the tab bar grows
    // to actually fit it.
    const QString buttonStyle =
        "QPushButton { background-color: %1; color: white; border-radius: 4px; padding: 6px 16px; }"
        "QPushButton:hover:!disabled { background-color: %2; }"
        "QPushButton:disabled { background-color: #444; color: #888; }";
    startButton_->setStyleSheet(buttonStyle.arg("#2e7d32", "#388e3c"));
    stopButton_->setStyleSheet(buttonStyle.arg("#c62828", "#d32f2f"));
    layout->addWidget(startButton_);
    layout->addWidget(stopButton_);

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopCapture);

    return bar;
}

QWidget* MainWindow::buildHomeTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

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

    layout->addWidget(displayGroup);
    layout->addWidget(rateGroup);
    layout->addWidget(configGroup);
    layout->addStretch(1);

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
    moreBusesButton_ = new QPushButton("More buses...", hwGroup);
    moreBusesButton_->setToolTip("Capture more buses alongside this one");
    hwLayout->addWidget(channelCombo_);
    hwLayout->addWidget(refreshButton_);
    hwLayout->addWidget(moreBusesButton_);

    auto* controllerGroup = new QGroupBox("Bus Configuration", page);
    auto* controllerLayout = new QHBoxLayout(controllerGroup);
    canControllerButton_ = new QPushButton("CAN Controller...", controllerGroup);
    controllerLayout->addWidget(canControllerButton_);

    auto* autoDetectGroup = new QGroupBox("Autodetect Bus Config", page);
    auto* autoDetectLayout = new QHBoxLayout(autoDetectGroup);
    autoDetectButton_ = new QPushButton("Click to check", autoDetectGroup);
    autoDetectLayout->addWidget(autoDetectButton_);

    auto* busLoadGroup = new QGroupBox("Bus Load", page);
    auto* busLoadLayout = new QHBoxLayout(busLoadGroup);
    busLoadButton_ = new QPushButton("Bus Load...", busLoadGroup);
    busLoadLayout->addWidget(busLoadButton_);

    auto* gatewayGroup = new QGroupBox("Gateway", page);
    auto* gatewayLayout = new QHBoxLayout(gatewayGroup);
    gatewayButton_ = new QPushButton("Gateway...", gatewayGroup);
    gatewayButton_->setToolTip("Forward frames from one bus to another");
    gatewayLayout->addWidget(gatewayButton_);

    layout->addWidget(hwGroup);
    layout->addWidget(controllerGroup);
    layout->addWidget(autoDetectGroup);
    layout->addWidget(busLoadGroup);
    layout->addWidget(gatewayGroup);
    layout->addStretch(1);

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshChannels);
    connect(moreBusesButton_, &QPushButton::clicked, this, &MainWindow::openMoreBuses);
    connect(gatewayButton_, &QPushButton::clicked, this, &MainWindow::openGateway);
    connect(canControllerButton_, &QPushButton::clicked, this, &MainWindow::openCanController);
    connect(autoDetectButton_, &QPushButton::clicked, this, &MainWindow::autoDetectBusConfig);
    // busLoadButton_'s own click->contentStack_ swap is wired later in the
    // constructor, once busLoadView_ actually exists (same reason
    // clearReceivedButton_'s connect is deferred past buildStimulationTab()
    // - this function runs before contentStack_'s pages are built).

    return page;
}

QWidget* MainWindow::buildAnalysisTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* dbGroup = new QGroupBox("CAN Database", page);
    auto* dbLayout = new QHBoxLayout(dbGroup);
    dbcManageButton_ = new QPushButton("DBC management...", dbGroup);
    dbcManageButton_->setToolTip("Import DBC files and assign them to buses");
    dbLayout->addWidget(dbcManageButton_);

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

    connect(dbcManageButton_, &QPushButton::clicked, this, &MainWindow::openDbcManagement);
    connect(traceButton, &QPushButton::clicked, this, [this]() { contentStack_->setCurrentWidget(frameTree_); });
    connect(graphicsButton, &QPushButton::clicked, this, [this]() { contentStack_->setCurrentWidget(graphWindows_); });

    return page;
}

QWidget* MainWindow::buildStimulationTab() {
    auto* page = new QWidget(ribbon_);
    auto* layout = new QHBoxLayout(page);

    auto* messagesGroup = new QGroupBox("Messages", page);
    auto* messagesLayout = new QHBoxLayout(messagesGroup);
    newMessageButton_ = new QPushButton("New Message...", messagesGroup);
    messagesLayout->addWidget(newMessageButton_);

    auto* sendingGroup = new QGroupBox("Sending", page);
    auto* sendingLayout = new QHBoxLayout(sendingGroup);
    startSendingButton_ = new QPushButton(QString::fromUtf8("\xE2\x96\xB6 Start Sending"), sendingGroup); // ▶
    stopSendingButton_ = new QPushButton(QString::fromUtf8("\xE2\x8F\xB8 Stop Sending"), sendingGroup); // ⏸
    sendNowButton_ = new QPushButton("Send Now", sendingGroup);
    sendingLayout->addWidget(startSendingButton_);
    sendingLayout->addWidget(stopSendingButton_);
    sendingLayout->addWidget(sendNowButton_);

    auto* receivedGroup = new QGroupBox("Received", page);
    auto* receivedLayout = new QHBoxLayout(receivedGroup);
    // Own Waterfall/Periodic toggle, independent of the Home tab's -
    // defaults to Periodic since that's what actually keeps a cyclic Send
    // Now/Start Sending burst from flooding this pane with a new row per
    // send the way plain Waterfall mode would (confirmed as a real
    // complaint, 2026-07-14: "the received message window gets spammed
    // with the sent messages").
    stimWaterfallRadio_ = new QRadioButton("Waterfall", receivedGroup);
    stimPeriodicRadio_ = new QRadioButton("Periodic", receivedGroup);
    stimPeriodicRadio_->setChecked(true);
    clearReceivedButton_ = new QPushButton("Clear", receivedGroup);
    receivedLayout->addWidget(stimWaterfallRadio_);
    receivedLayout->addWidget(stimPeriodicRadio_);
    receivedLayout->addWidget(clearReceivedButton_);

    layout->addWidget(messagesGroup);
    layout->addWidget(sendingGroup);
    layout->addWidget(receivedGroup);
    layout->addStretch(1);

    connect(newMessageButton_, &QPushButton::clicked, this, &MainWindow::onNewMessage);
    connect(startSendingButton_, &QPushButton::clicked, this, &MainWindow::onStartSending);
    connect(stopSendingButton_, &QPushButton::clicked, this, &MainWindow::onStopSending);
    connect(sendNowButton_, &QPushButton::clicked, this, [this]() { onSendNow(stimulationView_->selectedTransmitIndices()); });
    connect(stimWaterfallRadio_, &QRadioButton::toggled, this, &MainWindow::onStimDisplayModeChanged);
    connect(stimPeriodicRadio_, &QRadioButton::toggled, this, &MainWindow::onStimDisplayModeChanged);

    setSendingUiState();
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
    return dbcDecoder_.resolveMessageName(frame);
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
    box.setWindowTitle(QString("CANtrip v%1 - %2").arg(kVersion, kCodename));
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
    // One tab per bus in use. Each tab is a whole CAN Controller page, so
    // every bus gets its own mode, bitrates, FD timing and access setting.
    QDialog host(this);
    host.setWindowTitle("CAN Controller");
    auto* layout = new QVBoxLayout(&host);
    auto* tabs = new QTabWidget(&host);
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &host);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &host, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &host, &QDialog::reject);

    std::vector<CanControllerDialog*> pages;
    std::vector<int> pageBus; // 1 for CAN 1, else index + 2 into extraBuses_
    auto addPage = [&](int bus, const CanBitrateConfig& config, bool listenOnly) {
        auto* page = new CanControllerDialog(&host);
        page->embedAsPage();
        page->setConfig(config);
        page->setListenOnly(listenOnly);
        tabs->addTab(page, QString("CAN %1").arg(bus));
        pages.push_back(page);
        pageBus.push_back(bus);
    };
    addPage(1, busConfig_, listenOnlyMode_);
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        if (extraBuses_[i].enabled) addPage(static_cast<int>(i) + 2, extraBuses_[i].config, extraBuses_[i].listenOnly);
    }

    // OK only while every tab's timing is usable.
    auto syncOk = [&]() {
        bool ok = true;
        for (CanControllerDialog* page : pages) ok = ok && page->isAcceptable();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
    };
    for (CanControllerDialog* page : pages) connect(page, &CanControllerDialog::acceptableChanged, &host, syncOk);
    syncOk();

    if (host.exec() != QDialog::Accepted) return;
    for (size_t p = 0; p < pages.size(); ++p) {
        if (pageBus[p] == 1) {
            busConfig_ = pages[p]->config();
            listenOnlyMode_ = pages[p]->listenOnly();
        } else {
            ExtraBus& extra = extraBuses_[static_cast<size_t>(pageBus[p] - 2)];
            extra.config = pages[p]->config();
            extra.listenOnly = pages[p]->listenOnly();
        }
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

    autoDetectButton_->setText("Checking...");
    autoDetectButton_->setEnabled(false);
    // BusAutoDetector::detectClassicBitrate() blocks synchronously for
    // roughly a second - repaint explicitly now, since the event loop won't
    // get a turn to show "Checking..." until this handler returns otherwise.
    autoDetectButton_->repaint();

    // Every bus in use is checked, one after the other, and each keeps what
    // was found for it.
    struct Target {
        int bus;
        QString interfaceId;
        CanBitrateConfig* config;
    };
    std::vector<Target> targets = {{1, channelCombo_->currentData().toString(), &busConfig_}};
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        if (!extraBuses_[i].enabled) continue;
        const int channelIndex = channelCombo_->findText(extraBuses_[i].channelDisplayName);
        if (channelIndex < 0) continue;
        targets.push_back({static_cast<int>(i) + 2, channelCombo_->itemData(channelIndex).toString(), &extraBuses_[i].config});
    }

    QStringList results;
    bool anyFound = false;
    for (const Target& target : targets) {
        const std::optional<uint32_t> bitrate = BusAutoDetector::detectClassicBitrate(target.interfaceId);
        if (bitrate) {
            target.config->fd = false;
            target.config->nominalBitrateBps = *bitrate;
            anyFound = true;
        }
        if (targets.size() == 1) {
            results << (bitrate ? QString("%1 kbit/s").arg(*bitrate / 1000) : QString("No config found"));
        } else {
            results << QString("CAN %1: %2").arg(target.bus).arg(bitrate ? QString("%1k").arg(*bitrate / 1000) : QString("none"));
        }
    }
    autoDetectButton_->setText(results.join(", "));
    // Reuse the same Save Rune flow the Home tab button uses, so a
    // successful detection isn't lost if nothing was loaded/saved yet.
    if (anyFound) saveRune();
    autoDetectButton_->setEnabled(true);
}

void MainWindow::refreshChannels() {
    channelCombo_->clear();
    channels_.clear();

    for (auto& backend : probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            ChannelEntry entry;
            entry.interfaceId = QString::fromStdString(interfaceIdFor(*backend, ch));
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
    if (!dbcDecoder_.loadFile(path, error)) return false;
    onDbcSetChanged();
    return true;
}

void MainWindow::onDbcSetChanged() {
    traceModel_->decoderChanged();

    const std::vector<DbcDecoder::FileInfo> files = dbcDecoder_.files();
    if (files.empty()) {
        dbcStatusLabel_->setText("No DBC loaded...");
        dbcStatusLabel_->setStyleSheet("color: red;");
    } else {
        dbcStatusLabel_->setText(files.size() == 1 ? QFileInfo(files.front().path).fileName() + " loaded!"
                                                   : QString("%1 DBC files loaded!").arg(files.size()));
        dbcStatusLabel_->setStyleSheet("color: green;");
    }
}

void MainWindow::openDbcManagement() {
    QDialog dialog(this);
    dialog.setWindowTitle("DBC management");
    dialog.resize(560, 380);
    auto* layout = new QVBoxLayout(&dialog);

    // One node per bus in use, its DBC files underneath. Files loaded for
    // every bus (an older rune, or headless --dbc) get a node of their own.
    auto* tree = new QTreeWidget(&dialog);
    tree->setColumnCount(2);
    tree->setHeaderLabels({"Bus / DBC file", "Messages"});
    tree->setColumnWidth(0, 400);
    layout->addWidget(tree, /*stretch=*/1);

    auto* conflictsLabel = new QLabel(&dialog);
    conflictsLabel->setWordWrap(true);
    conflictsLabel->setStyleSheet("color: #c62828;");
    layout->addWidget(conflictsLabel);

    constexpr int kBusRole = Qt::UserRole;       // on every item: the bus it belongs to
    constexpr int kFileRole = Qt::UserRole + 1;  // on file items: index into dbcDecoder_.files()
    auto refresh = [this, tree, conflictsLabel, kBusRole, kFileRole](int selectBus) {
        tree->clear();
        const std::vector<DbcDecoder::FileInfo> files = dbcDecoder_.files();
        std::vector<int> buses = {1};
        for (size_t i = 0; i < extraBuses_.size(); ++i) {
            if (extraBuses_[i].enabled) buses.push_back(static_cast<int>(i) + 2);
        }
        // A bus that has files but is switched off right now still shows them.
        for (const DbcDecoder::FileInfo& file : files) {
            if (file.bus != DbcDecoder::kAllBuses && std::find(buses.begin(), buses.end(), file.bus) == buses.end()) buses.push_back(file.bus);
        }
        std::sort(buses.begin(), buses.end());
        for (const DbcDecoder::FileInfo& file : files) {
            if (file.bus == DbcDecoder::kAllBuses) { buses.insert(buses.begin(), DbcDecoder::kAllBuses); break; }
        }

        for (int bus : buses) {
            auto* busItem = new QTreeWidgetItem(tree);
            busItem->setText(0, bus == DbcDecoder::kAllBuses ? QString("All buses") : QString("CAN %1").arg(bus));
            busItem->setData(0, kBusRole, bus);
            busItem->setData(0, kFileRole, -1);
            QFont bold = busItem->font(0);
            bold.setBold(true);
            busItem->setFont(0, bold);
            for (size_t i = 0; i < files.size(); ++i) {
                if (files[i].bus != bus) continue;
                auto* fileItem = new QTreeWidgetItem(busItem);
                fileItem->setText(0, QFileInfo(files[i].path).fileName());
                fileItem->setToolTip(0, files[i].path);
                fileItem->setText(1, QString::number(files[i].messageCount));
                fileItem->setData(0, kBusRole, bus);
                fileItem->setData(0, kFileRole, static_cast<int>(i));
            }
            busItem->setExpanded(true);
            if (bus == selectBus) tree->setCurrentItem(busItem);
        }
        if (!tree->currentItem() && tree->topLevelItemCount() > 0) tree->setCurrentItem(tree->topLevelItem(0));

        const QStringList conflicts = dbcDecoder_.conflicts();
        // A long list of clashes would push the buttons off the dialog.
        QString text = conflicts.mid(0, 5).join("\n");
        if (conflicts.size() > 5) text += QString("\n... and %1 more.").arg(conflicts.size() - 5);
        conflictsLabel->setText(text);
    };
    refresh(1);

    auto* row = new QHBoxLayout();
    auto* importButton = new QPushButton("Import DBC...", &dialog);
    auto* removeButton = new QPushButton("Remove", &dialog);
    auto* closeButton = new QPushButton("Close", &dialog);
    row->addWidget(importButton);
    row->addWidget(removeButton);
    row->addStretch(1);
    row->addWidget(closeButton);
    layout->addLayout(row);

    auto syncButtons = [tree, importButton, removeButton, kBusRole, kFileRole]() {
        QTreeWidgetItem* item = tree->currentItem();
        importButton->setEnabled(item != nullptr);
        removeButton->setEnabled(item && item->data(0, kFileRole).toInt() >= 0);
        if (item) {
            const int bus = item->data(0, kBusRole).toInt();
            importButton->setText(bus == DbcDecoder::kAllBuses ? QString("Import DBC for all buses...")
                                                               : QString("Import DBC for CAN %1...").arg(bus));
        }
    };
    connect(tree, &QTreeWidget::currentItemChanged, &dialog, syncButtons);
    syncButtons();

    // Imports go to the bus of whatever is selected: a bus, or one of its files.
    connect(importButton, &QPushButton::clicked, &dialog, [this, &dialog, tree, refresh, kBusRole]() {
        if (!tree->currentItem()) return;
        const int bus = tree->currentItem()->data(0, kBusRole).toInt();
        const QStringList paths = QFileDialog::getOpenFileNames(&dialog, "Import DBC", QString(), "DBC files (*.dbc)");
        for (const QString& path : paths) {
            QString error;
            if (!dbcDecoder_.addFile(path, bus, &error)) QMessageBox::warning(&dialog, "Import DBC", error);
        }
        refresh(bus);
    });
    connect(removeButton, &QPushButton::clicked, &dialog, [this, tree, refresh, kBusRole, kFileRole]() {
        QTreeWidgetItem* item = tree->currentItem();
        if (!item || item->data(0, kFileRole).toInt() < 0) return;
        const int bus = item->data(0, kBusRole).toInt();
        dbcDecoder_.removeFile(item->data(0, kFileRole).toInt());
        refresh(bus);
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
    onDbcSetChanged();
}

void MainWindow::saveRune() {
    const QString path = QFileDialog::getSaveFileName(this, "Save Rune", QString(), "Rune files (*.rune)");
    if (path.isEmpty()) return;

    RuneConfig config;
    config.channelDisplayName = channelCombo_->currentIndex() >= 0 ? channelCombo_->currentText() : QString();
    config.busConfig = busConfig_;
    config.displayMode = displayMode_ == DisplayMode::Periodic ? RuneDisplayMode::Periodic : RuneDisplayMode::Waterfall;
    config.displayRateMs = displayIntervalMs_;
    config.dbcPath = dbcDecoder_.path();
    for (const DbcDecoder::FileInfo& file : dbcDecoder_.files()) config.dbcFiles.push_back({file.path, file.bus});
    config.graphWindows = graphWindows_->exportLayout();
    config.transmitMessages = messageSender_.messages();
    config.traceHeaderState = frameTree_->header()->saveState();
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        if (!extraBuses_[i].enabled) continue;
        config.extraBuses.push_back({static_cast<int>(i) + 2, extraBuses_[i].channelDisplayName, extraBuses_[i].config, extraBuses_[i].listenOnly});
    }
    config.listenOnly = listenOnlyMode_;
    config.gatewayRules = gatewayRules_;
    const UdsClient::Target diagTarget = diagnosticsView_->target();
    config.diagBus = diagTarget.bus;
    config.diagRequestId = diagTarget.requestId;
    config.diagResponseId = diagTarget.responseId;
    config.diagExtended = diagTarget.extended;
    config.gatewayEnabled = gatewayEnabled_;

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
    applyRune(*config);
}

void MainWindow::applyRune(const RuneConfig& config) {
    const int channelIndex = channelCombo_->findText(config.channelDisplayName);
    if (channelIndex < 0) {
        QMessageBox::warning(this, "Load Rune",
            "Channel not found among currently available hardware:\n" + config.channelDisplayName +
            "\n\nLeaving the current channel selection as-is.");
    } else {
        channelCombo_->setCurrentIndex(channelIndex);
    }

    busConfig_ = config.busConfig;

    displayMode_ = config.displayMode == RuneDisplayMode::Periodic ? DisplayMode::Periodic : DisplayMode::Waterfall;
    periodicRadio_->setChecked(displayMode_ == DisplayMode::Periodic);
    waterfallRadio_->setChecked(displayMode_ == DisplayMode::Waterfall);

    const int rateIndex = displayRateCombo_->findData(config.displayRateMs);
    if (rateIndex >= 0) displayRateCombo_->setCurrentIndex(rateIndex);

    if (!config.dbcFiles.empty()) {
        dbcDecoder_.clear();
        for (const RuneDbcFile& file : config.dbcFiles) {
            QString dbcError;
            if (!dbcDecoder_.addFile(file.path, file.bus, &dbcError)) {
                QMessageBox::warning(this, "Load Rune", "Could not reload a DBC referenced by this rune:\n" + dbcError);
            }
        }
        onDbcSetChanged();
    } else if (!config.dbcPath.isEmpty()) {
        QString dbcError;
        if (!loadDbcFile(config.dbcPath, &dbcError)) {
            QMessageBox::warning(this, "Load Rune", "Could not reload the DBC referenced by this rune:\n" + dbcError);
        }
    }

    graphWindows_->reset();
    graphWindows_->importLayout(config.graphWindows);

    messageSender_.clearMessages();
    for (const TransmitMessage& message : config.transmitMessages) {
        messageSender_.addMessage(message);
    }
    refreshTransmitList();

    // Absent (empty QByteArray) for a rune saved before this field existed -
    // QHeaderView::restoreState() on an empty/invalid byte array is a
    // documented no-op, so the header is simply left as whatever it
    // currently is rather than needing an explicit "if saved" check.
    traceHeader_->restoreState(config.traceHeaderState);
    traceHeader_->applyBehavior();
    traceModel_->sort(traceHeader_->sortIndicatorSection(), traceHeader_->sortIndicatorOrder());
    listenOnlyMode_ = config.listenOnly;

    extraBuses_.assign(3, ExtraBus{});
    for (const RuneExtraBus& extra : config.extraBuses) {
        const size_t index = static_cast<size_t>(extra.bus - 2);
        if (index >= extraBuses_.size()) extraBuses_.resize(index + 1);
        extraBuses_[index] = {true, extra.channelDisplayName, extra.busConfig, extra.listenOnly};
    }
    updateMoreBusesButton();
    diagnosticsView_->setTarget({config.diagBus, config.diagRequestId, config.diagResponseId, config.diagExtended});
    gatewayRules_ = config.gatewayRules;
    gatewayEnabled_ = config.gatewayEnabled;
    applyGateway();
}

void MainWindow::updateMoreBusesButton() {
    int enabled = 0;
    for (const ExtraBus& extra : extraBuses_) enabled += extra.enabled ? 1 : 0;
    moreBusesButton_->setText(enabled > 0 ? QString("More buses (%1)...").arg(enabled) : QString("More buses..."));
}

void MainWindow::openMoreBuses() {
    QDialog dialog(this);
    dialog.setWindowTitle("More buses");
    auto* layout = new QVBoxLayout(&dialog);
    auto* rows = new QGridLayout();
    layout->addLayout(rows);

    std::vector<QCheckBox*> checks;
    std::vector<QComboBox*> channelCombos;
    // Row 0 is CAN 1, the bus the Hardware tab's own dropdown selects.
    auto addRow = [&](bool enabled, const QString& channelName) {
        const int row = static_cast<int>(checks.size());
        auto* check = new QCheckBox(QString("CAN %1").arg(row + 1), &dialog);
        auto* combo = new QComboBox(&dialog);
        combo->setMinimumWidth(340);
        for (const ChannelEntry& entry : channels_) combo->addItem(entry.displayName);
        // A saved channel that isn't plugged in right now is kept, not
        // silently swapped for whatever happens to be first in the list.
        if (!channelName.isEmpty() && combo->findText(channelName) < 0) combo->addItem(channelName);
        if (!channelName.isEmpty()) combo->setCurrentText(channelName);
        check->setChecked(enabled);
        combo->setEnabled(enabled);
        if (row == 0) check->setEnabled(false); // CAN 1 is always there
        connect(check, &QCheckBox::toggled, combo, &QComboBox::setEnabled);
        rows->addWidget(check, row, 0);
        rows->addWidget(combo, row, 1);
        checks.push_back(check);
        channelCombos.push_back(combo);
    };
    addRow(true, channelCombo_->currentText());
    for (const ExtraBus& extra : extraBuses_) addRow(extra.enabled, extra.channelDisplayName);

    auto* moreRow = new QHBoxLayout();
    auto* addButton = new QPushButton("+", &dialog);
    addButton->setFixedWidth(32);
    addButton->setToolTip("Add another bus");
    moreRow->addWidget(new QLabel("More", &dialog));
    moreRow->addWidget(addButton);
    moreRow->addStretch(1);
    layout->addLayout(moreRow);
    connect(addButton, &QPushButton::clicked, &dialog, [&]() {
        addRow(true, QString());
        addButton->setEnabled(static_cast<int>(checks.size()) < kMaxBuses);
    });
    addButton->setEnabled(static_cast<int>(checks.size()) < kMaxBuses);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    const int bus1Index = channelCombo_->findText(channelCombos[0]->currentText());
    if (bus1Index >= 0) channelCombo_->setCurrentIndex(bus1Index);
    // Rows added with "+" get the same starting configuration a new bus
    // always had. Existing buses keep theirs.
    extraBuses_.resize(checks.size() - 1);
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        extraBuses_[i].enabled = checks[i + 1]->isChecked();
        extraBuses_[i].channelDisplayName = channelCombos[i + 1]->currentText();
    }
    // Unticked buses at the end are dropped again, down to the usual CAN 2 to 4.
    while (extraBuses_.size() > 3 && !extraBuses_.back().enabled) extraBuses_.pop_back();
    updateMoreBusesButton();
}

void MainWindow::applyGateway() {
    gateway_->setRules(gatewayEnabled_ ? gatewayRules_ : std::vector<GatewayRule>{});
    const int rules = gatewayEnabled_ ? static_cast<int>(gatewayRules_.size()) : 0;
    gatewayButton_->setText(rules > 0 ? QString("Gateway (%1)...").arg(rules) : QString("Gateway..."));
}

void MainWindow::pumpGateway() {
    // Forwarded frames show up on the bus they went out on, as Tx rows, the
    // same way Send Message's frames do.
    if (!capture_.isRunning()) {
        gateway_->drainSent();
        gatewayPump_.reset();
        return;
    }
    for (const DecodedCanFrame& frame : gatewayPump_.pump(*gateway_, timeAligner_, sessionClock_.nsecsElapsed() / 1e9)) {
        onFrameReceived(frame);
        // A transmitted frame occupies its bus like any other.
        if (frame.bus == 1) busLoadTracker_.recordFrame(frame);
        for (ExtraBusLoad& load : extraBusLoads_) {
            if (load.bus == frame.bus) load.tracker->recordFrame(frame);
        }
    }

    const uint64_t failed = gateway_->failed();
    if (failed != gatewayFailuresShown_) {
        gatewayFailuresShown_ = failed;
        statusLabel_->setText(QString("Gateway: %1 frame(s) could not be transmitted (%2)")
                                  .arg(failed).arg(QString::fromStdString(gateway_->lastError())));
    }
}

void MainWindow::openGateway() {
    QDialog dialog(this);
    dialog.setWindowTitle("Gateway");
    dialog.resize(460, 320);
    auto* layout = new QVBoxLayout(&dialog);

    auto* enable = new QCheckBox("Forward frames between buses", &dialog);
    enable->setChecked(gatewayEnabled_);
    layout->addWidget(enable);

    std::vector<int> buses = {1};
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        if (extraBuses_[i].enabled) buses.push_back(static_cast<int>(i) + 2);
    }

    // One row per rule: frames from one bus, with these IDs, go out on another.
    auto* table = new QTableWidget(0, 3, &dialog);
    table->setHorizontalHeaderLabels({"From", "To", "IDs"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table, /*stretch=*/1);

    auto addRule = [table, buses](const GatewayRule& rule) {
        const int row = table->rowCount();
        table->insertRow(row);
        for (int col = 0; col < 2; ++col) {
            auto* combo = new QComboBox(table);
            const int wanted = col == 0 ? rule.fromBus : rule.toBus;
            std::vector<int> listed = buses;
            // A rule for a bus that is switched off right now keeps its bus.
            if (std::find(listed.begin(), listed.end(), wanted) == listed.end()) listed.push_back(wanted);
            for (int bus : listed) combo->addItem(QString("CAN %1").arg(bus), bus);
            combo->setCurrentIndex(combo->findData(wanted));
            table->setCellWidget(row, col, combo);
        }
        auto* ids = new QTableWidgetItem(gatewayIdRangeText(rule));
        ids->setToolTip("all, one ID (0x100), or a range (0x100-0x1FF)");
        table->setItem(row, 2, ids);
        table->selectRow(row);
    };
    for (const GatewayRule& rule : gatewayRules_) addRule(rule);

    auto* row = new QHBoxLayout();
    auto* addButton = new QPushButton("+", &dialog);
    addButton->setFixedWidth(32);
    addButton->setToolTip("Add a rule");
    auto* removeButton = new QPushButton("Remove", &dialog);
    auto* totals = new QLabel(&dialog);
    row->addWidget(addButton);
    row->addWidget(removeButton);
    row->addStretch(1);
    row->addWidget(totals);
    layout->addLayout(row);

    // Forwarding needs two buses. With one, say where to get a second.
    addButton->setEnabled(buses.size() >= 2);
    if (buses.size() < 2) totals->setText("Tick a second bus in More buses first.");
    connect(addButton, &QPushButton::clicked, &dialog, [addRule, buses]() {
        GatewayRule rule;
        rule.fromBus = buses[0];
        rule.toBus = buses[1];
        addRule(rule);
    });
    connect(removeButton, &QPushButton::clicked, &dialog, [table]() {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });

    // What the running gateway has done so far, kept current while open.
    QTimer refresh;
    auto showTotals = [this, totals, &buses]() {
        if (buses.size() < 2 || !capture_.isRunning()) return;
        uint64_t forwarded = 0;
        for (uint64_t n : gateway_->forwardedCounts()) forwarded += n;
        QString text = QString("%1 forwarded").arg(forwarded);
        if (gateway_->dropped()) text += QString(", %1 dropped").arg(gateway_->dropped());
        if (gateway_->failed()) text += QString(", %1 failed").arg(gateway_->failed());
        totals->setText(text);
    };
    connect(&refresh, &QTimer::timeout, &dialog, showTotals);
    refresh.start(250);
    showTotals();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::vector<GatewayRule> edited;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        edited.clear();
        for (int r = 0; r < table->rowCount(); ++r) {
            GatewayRule rule;
            rule.fromBus = static_cast<QComboBox*>(table->cellWidget(r, 0))->currentData().toInt();
            rule.toBus = static_cast<QComboBox*>(table->cellWidget(r, 1))->currentData().toInt();
            if (rule.fromBus == rule.toBus) {
                table->selectRow(r);
                QMessageBox::warning(&dialog, "Gateway", QString("Rule %1 forwards a bus to itself.").arg(r + 1));
                return;
            }
            if (!parseGatewayIdRange(table->item(r, 2)->text(), &rule.idLow, &rule.idHigh)) {
                table->selectRow(r);
                QMessageBox::warning(&dialog, "Gateway",
                    QString("Rule %1: \"%2\" is not an ID or a range of IDs.\n\nUse all, one ID such as 0x100, or a range such as 0x100-0x1FF.")
                        .arg(r + 1).arg(table->item(r, 2)->text()));
                return;
            }
            edited.push_back(rule);
        }
        dialog.accept();
    });
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;
    gatewayRules_ = edited;
    gatewayEnabled_ = enable->isChecked();
    // Takes effect at once, also in the middle of a capture.
    applyGateway();
}

QString MainWindow::findTsharkExe() {
    QString found = QStandardPaths::findExecutable("tshark");
    if (!found.isEmpty()) return found;
    const QString defaultInstall = "C:/Program Files/Wireshark/tshark.exe";
    if (QFileInfo::exists(defaultInstall)) return defaultInstall;
    return "tshark"; // let QProcess try PATH and fail with a clear error if not found
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
    config.listenOnly = listenOnlyMode_;

    // Every extra bus is checked before anything starts, so a problem with
    // bus 3 doesn't leave buses 1 and 2 running.
    std::vector<TsharkCapture::Config> extraConfigs;
    for (size_t i = 0; i < extraBuses_.size(); ++i) {
        const ExtraBus& extra = extraBuses_[i];
        if (!extra.enabled) continue;
        const int bus = static_cast<int>(i) + 2;
        const int channelIndex = channelCombo_->findText(extra.channelDisplayName);
        if (channelIndex < 0) {
            QMessageBox::warning(this, "Start Capture",
                QString("CAN %1's channel isn't available:\n%2\n\nPick another in More buses, or turn that bus off.")
                    .arg(bus).arg(extra.channelDisplayName));
            return;
        }
        TsharkCapture::Config extraConfig;
        extraConfig.tsharkPath = config.tsharkPath;
        extraConfig.interfaceId = channelCombo_->itemData(channelIndex).toString();
        extraConfig.bus = bus;
        extraConfig.nominalBitrateBps = extra.config.nominalBitrateBps;
        extraConfig.dataBitrateBps = extra.config.dataBitrateBps;
        extraConfig.fd = extra.config.fd;
        extraConfig.nomBrp = extra.config.nominalTiming.brp;
        extraConfig.nomTseg1 = extra.config.nominalTiming.tseg1;
        extraConfig.nomTseg2 = extra.config.nominalTiming.tseg2;
        extraConfig.nomSjw = extra.config.nominalTiming.sjw;
        extraConfig.dataBrp = extra.config.dataTiming.brp;
        extraConfig.dataTseg1 = extra.config.dataTiming.tseg1;
        extraConfig.dataTseg2 = extra.config.dataTiming.tseg2;
        extraConfig.dataSjw = extra.config.dataTiming.sjw;
        extraConfig.listenOnly = extra.listenOnly;
        extraConfig.gateway = gateway_;
        bool channelTaken = extraConfig.interfaceId == config.interfaceId;
        for (const TsharkCapture::Config& other : extraConfigs) channelTaken = channelTaken || other.interfaceId == extraConfig.interfaceId;
        if (channelTaken && extraConfig.interfaceId != kTestInterfaceId) {
            QMessageBox::warning(this, "Start Capture",
                QString("Bus %1 uses a channel another bus already uses:\n%2").arg(bus).arg(extra.channelDisplayName));
            return;
        }
        extraConfigs.push_back(extraConfig);
    }

    resetDisplay();
    busLoadTracker_.setBusConfig(busConfig_);
    busLoadTracker_.reset();
    statusLabel_->setText("Starting capture on " + channelCombo_->currentText() + "...");
    statusLed_->setCapturing(true);
    gateway_->reset();
    gatewayFailuresShown_ = 0;
    config.gateway = gateway_;
    capture_.start(config);

    extraCaptures_.clear();
    timeAligner_.reset();
    sessionClock_.start();
    for (ExtraBusLoad& load : extraBusLoads_) {
        busLoadTabs_->removeTab(busLoadTabs_->indexOf(load.view));
        delete load.view;
    }
    extraBusLoads_.clear();
    for (const TsharkCapture::Config& extraConfig : extraConfigs) {
        auto capture = std::make_unique<TsharkCapture>();
        const int bus = extraConfig.bus;
        connect(capture.get(), &TsharkCapture::frameReceived, this, &MainWindow::onFrameReceived);
        connect(capture.get(), &TsharkCapture::errorOccurred, this, [this, bus](const QString& message) {
            statusLabel_->setText(QString("Bus %1: %2").arg(bus).arg(message.trimmed()));
        });
        // An extra bus ending on its own doesn't end the session: the other
        // buses keep capturing, and the status bar says which one stopped.
        connect(capture.get(), &TsharkCapture::stopped, this, [this, bus]() {
            if (capture_.isRunning()) statusLabel_->setText(QString("Bus %1 stopped").arg(bus));
        });
        // Its own Bus Load tab, fed by its own capture.
        ExtraBusLoad load;
        load.bus = bus;
        load.tracker = std::make_unique<BusLoadTracker>();
        load.tracker->setBusConfig(extraBuses_[static_cast<size_t>(bus - 2)].config);
        load.tracker->reset();
        connect(capture.get(), &TsharkCapture::frameReceived, load.tracker.get(), &BusLoadTracker::recordFrame);
        load.view = new BusLoadView(load.tracker.get(), busLoadTabs_);
        busLoadTabs_->addTab(load.view, QString("CAN %1").arg(bus));
        extraBusLoads_.push_back(std::move(load));

        capture->start(extraConfig);
        extraCaptures_.push_back(std::move(capture));
    }
    busLoadTabs_->tabBar()->setVisible(!extraBusLoads_.empty());
    frameTree_->setColumnHidden(TraceModel::Bus, extraCaptures_.empty());
    moreBusesButton_->setEnabled(false);

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
    dbcManageButton_->setEnabled(false);
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

    // MessageSender's own port (see MessageSender.h) is opened lazily, on
    // the first actual Start Sending/Send Now click (see
    // ensureSenderPortOpen()) rather than here - opening it immediately
    // raced against capture_.start() actually finishing launching
    // tshark/can2pcap.exe and initializing the channel, which broke live
    // capture for real on PEAK hardware (confirmed 2026-07-13: the second,
    // near-simultaneous initialize call reset the channel the capture had
    // just set up). By the time the user clicks Start Sending, capture is
    // reliably already flowing.
    setSendingUiState();
}

void MainWindow::stopCapture() {
    if (!capture_.isRunning()) return;
    for (auto& capture : extraCaptures_) capture->stop();
    capture_.stop();
}

void MainWindow::onDisplayModeChanged() {
    displayMode_ = periodicRadio_->isChecked() ? DisplayMode::Periodic : DisplayMode::Waterfall;
    resetDisplay();
    applyDefaultTraceSort();
}

void MainWindow::applyDefaultTraceSort() {
    // Periodic's Time column is each row's period, which changes on every
    // frame. Sorting on it by default would keep rows jumping around and
    // re-sort constantly, so Periodic starts out in stable ID order.
    const bool periodic = displayMode_ == DisplayMode::Periodic;
    traceHeader_->setSortIndicator(periodic ? TraceModel::Id : TraceModel::Time, periodic ? Qt::AscendingOrder : Qt::DescendingOrder);
    // Direct too: the header starts out on Time descending, so setting that
    // at startup changes nothing and sends no signal.
    traceModel_->sort(traceHeader_->sortIndicatorSection(), traceHeader_->sortIndicatorOrder());
}

void MainWindow::resetDisplay() {
    traceModel_->clear();
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

void MainWindow::populateDecodedChildren(QTreeWidgetItem* item, const DecodedCanFrame& frame) {
    const QString dataText = QString::fromLatin1(frame.data.toHex(' ').toUpper());

    item->setText(1, TraceModel::idText(frame));
    item->setText(2, TraceModel::flagsText(frame));
    item->setText(3, QString::number(frame.dlc));
    item->setText(4, dataText);
    // Reset (or apply) the TX tint every call, not just on first creation -
    // periodic mode reuses the same item across many frames, and a row that
    // was never Tx must not keep a stale color from some earlier state.
    for (int col = 0; col < item->columnCount(); ++col) {
        item->setForeground(col, frame.direction == FrameDirection::Tx ? TraceModel::txBrush() : QBrush());
    }

    const QString messageName = dbcDecoder_.resolveMessageName(frame);
    const std::vector<DbcDecoder::DecodedSignal> decodedSignals = dbcDecoder_.decodeSignals(frame);

    qDeleteAll(item->takeChildren());
    item->setText(5, messageName);
    item->setText(6, QString());

    for (const DbcDecoder::DecodedSignal& sig : decodedSignals) {
        auto* sigItem = new QTreeWidgetItem(item);
        sigItem->setText(5, sig.name);
        sigItem->setText(6, QString::number(sig.physicalValue) + " " + sig.unit);
    }
}

void MainWindow::showFrame(const DecodedCanFrame& frame) {
    if (displayMode_ == DisplayMode::Waterfall) traceModel_->appendFrames({frame});
    else {
        traceModel_->upsertPeriodic(frame, QDateTime::currentMSecsSinceEpoch());
        traceModel_->endPeriodicBatch();
    }
}

// Stimulation tab's received pane, own Waterfall/Periodic dedup. Runs against
// stimulationView_->receivedTree() and the stimPeriodicRows_/
// stimPeriodicErrorRows_ maps, independent of the Trace view's TraceModel.
void MainWindow::handleStimWaterfallFrame(const DecodedCanFrame& frame) {
    populateDecodedChildren(stimulationView_->addReceivedRow(frame.timestamp), frame);
}

void MainWindow::handleStimPeriodicFrame(const DecodedCanFrame& frame) {
    const uint64_t key = TraceModel::frameKey(frame);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto it = stimPeriodicRows_.find(key);
    if (it == stimPeriodicRows_.end()) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, "--");
        stimulationView_->receivedTree()->addTopLevelItem(item);
        it = stimPeriodicRows_.emplace(key, PeriodicRowState{item, now, -1, false}).first;
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

void MainWindow::addStimErrorRow(const DecodedCanFrame& frame) {
    QTreeWidgetItem* item = stimulationView_->addReceivedRow(frame.timestamp);
    item->setText(5, "BUS ERROR: " + frame.errorDescription);
    for (int col = 0; col < item->columnCount(); ++col) {
        item->setForeground(col, QBrush(QColor(Qt::red)));
    }
}

void MainWindow::handleStimPeriodicErrorFrame(const DecodedCanFrame& frame) {
    const QString key = frame.errorDescription;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    auto it = stimPeriodicErrorRows_.find(key);
    if (it == stimPeriodicErrorRows_.end()) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, "--");
        item->setText(5, "BUS ERROR: " + frame.errorDescription);
        stimulationView_->receivedTree()->addTopLevelItem(item);
        it = stimPeriodicErrorRows_.insert(key, PeriodicRowState{item, now, -1, false});
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

void MainWindow::onStimDisplayModeChanged() {
    stimDisplayMode_ = stimPeriodicRadio_->isChecked() ? DisplayMode::Periodic : DisplayMode::Waterfall;
    onClearReceived();
}

void MainWindow::onClearReceived() {
    stimulationView_->clearReceived();
    stimPeriodicRows_.clear();
    stimPeriodicErrorRows_.clear();
}

void MainWindow::onFrameReceived(const DecodedCanFrame& received) {
    // With several buses, each capture counts time from its own first frame.
    // Put them on one clock before anything below sees them (see
    // BusTimeAligner.h). A single-bus session is passed through untouched.
    DecodedCanFrame aligned;
    const bool align = !extraCaptures_.empty() && capture_.isRunning() && received.direction == FrameDirection::Rx;
    if (align) {
        aligned = received;
        const double t = timeAligner_.align(received.bus, received.timestamp.toDouble(), sessionClock_.nsecsElapsed() / 1e9);
        aligned.timestamp = QString::number(t, 'f', 6);
    }
    const DecodedCanFrame& frame = align ? aligned : received;

    // The ECU's answers, when a diagnostic request is waiting for one.
    udsClient_.onFrame(frame);

    // A replayed multi-bus log brings its bus numbers with it.
    if (frame.bus > 1 && frameTree_->isColumnHidden(TraceModel::Bus)) frameTree_->setColumnHidden(TraceModel::Bus, false);

    ++frameCount_;

    // Logging always sees every frame, regardless of the display-rate
    // throttle below - that throttle only bounds how often the UI repaints,
    // it was never meant to drop data from the log (bus errors get the same
    // "never throttled" treatment for the same reason, a few lines down).
    // Channel hardcoded to 1 until multi-bus exists - see LogWriter.h.
    if (logging_) {
        logWriter_->writeFrame(frame.bus, frame);
        rotateLogFileIfNeeded();
    }

    // The Graph view also sees every frame, here rather than where rows are
    // drawn: display throttling must not thin out plotted data.
    if (!frame.error && dbcDecoder_.isLoaded()) {
        const double timeSec = frame.timestamp.toDouble();
        for (const DbcDecoder::DecodedSignal& sig : dbcDecoder_.decodeSignals(frame)) {
            // The same message on two buses is two different sets of signals.
            const QString name = frame.bus > 1 ? QString("CAN%1.%2").arg(frame.bus).arg(sig.qualifiedName) : sig.qualifiedName;
            signalHistory_.recordSample(name, sig.unit, timeSec, sig.physicalValue);
        }
    }

    // Stimulation view's received pane gets every frame too (Rx and Tx
    // alike, since MessageSender's transmitted frames flow through this same
    // slot - marked and colored distinctly, see TraceModel::txBrush()) -
    // only while it's actually the visible page, same spirit as the
    // display-rate throttle not doing UI work nobody's looking at. Uses its
    // own Waterfall/Periodic toggle (stimDisplayMode_), same reasoning as
    // the main Trace view's: a cyclic Send Now/Start Sending burst would
    // otherwise flood this pane with a new row per send.
    if (contentStack_->currentWidget() == stimulationView_) {
        if (frame.error) {
            if (stimDisplayMode_ == DisplayMode::Waterfall) {
                addStimErrorRow(frame);
            } else {
                handleStimPeriodicErrorFrame(frame);
            }
        } else if (stimDisplayMode_ == DisplayMode::Waterfall) {
            handleStimWaterfallFrame(frame);
        } else {
            handleStimPeriodicFrame(frame);
        }
    }

    if (frame.error) {
        // Bus errors are comparatively rare and important - always shown
        // immediately, never subject to the display-rate throttle below.
        showFrame(frame);
        updateFrameCountStatus();
        return;
    }

    if (displayIntervalMs_ <= 0) {
        // Unlimited: no buffering, one model update per frame.
        showFrame(frame);
        updateFrameCountStatus();
        return;
    }

    // Throttled: keep only the latest frame per row (Periodic) or queue it
    // (Waterfall) and let displayFlushTimer_ apply them at a bounded rate -
    // see flushPendingDisplay().
    if (displayMode_ == DisplayMode::Waterfall) {
        pendingWaterfallFrames_.push_back(frame);
    } else {
        pendingPeriodicFrames_[TraceModel::frameKey(frame)] = frame;
    }
}

void MainWindow::flushPendingDisplay() {
    if (pendingPeriodicFrames_.empty() && pendingWaterfallFrames_.empty()) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto& [key, frame] : pendingPeriodicFrames_) {
        traceModel_->upsertPeriodic(frame, now);
    }
    traceModel_->endPeriodicBatch();
    pendingPeriodicFrames_.clear();

    traceModel_->appendFrames(pendingWaterfallFrames_);
    pendingWaterfallFrames_.clear();

    updateFrameCountStatus();
}

void MainWindow::updateFrameCountStatus() {
    QString text = QString("Captured %1 frame(s)").arg(frameCount_);
    if (traceModel_->hasFilter()) {
        text += QString(", showing %1 of %2 row(s)").arg(traceModel_->rowCount()).arg(traceModel_->totalRows());
    }
    if (signalHistory_.coarsened()) {
        text += ", graph history is over 2 GB: its oldest data is now kept at lower detail";
    }
    statusLabel_->setText(text);
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    traceModel_->markStaleRows(now, kStaleTimeoutMs);

    // The Stimulation tab's received pane has its own Waterfall/Periodic
    // toggle (stimDisplayMode_).
    if (stimDisplayMode_ == DisplayMode::Periodic) {
        for (auto& [key, state] : stimPeriodicRows_) {
            if (!state.stale && (now - state.lastArrivalMs) > kStaleTimeoutMs) {
                state.stale = true;
                setItemGrayed(state.item, true);
            }
        }
        for (auto it = stimPeriodicErrorRows_.begin(); it != stimPeriodicErrorRows_.end(); ++it) {
            if (!it->stale && (now - it->lastArrivalMs) > kStaleTimeoutMs) {
                it->stale = true;
                setItemGrayed(it->item, true);
            }
        }
    }
}

void MainWindow::onCaptureError(const QString& message) {
    statusLabel_->setText(message.trimmed());
}

void MainWindow::onCaptureStopped() {
    // Bus 1 ended, by Stop or by itself. The session is one thing, so the
    // other buses end with it.
    for (auto& capture : extraCaptures_) {
        if (capture->isRunning()) capture->stop();
    }
    moreBusesButton_->setEnabled(true);
    startButton_->setEnabled(true);
    stopButton_->setEnabled(false);
    channelCombo_->setEnabled(true);
    dbcManageButton_->setEnabled(true);
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

    messageSender_.closePort();
    setSendingUiState();
    udsClient_.cancel();
}

void MainWindow::onRibbonTabChanged(int index) {
    if (ribbon_->tabText(index) == "Stimulation") {
        contentStack_->setCurrentWidget(stimulationView_);
    } else if (ribbon_->tabText(index) == "Diagnostics") {
        contentStack_->setCurrentWidget(diagnosticsView_);
    } else {
        // Every other tab defaults back to the normal CAN Trace view -
        // Analysis & Measurement's own Trace/Graphics buttons can still
        // switch to Graphics explicitly once you're there, this only
        // governs what you land on when switching tabs.
        contentStack_->setCurrentWidget(frameTree_);
    }
}

void MainWindow::setSendingUiState() {
    // The port itself isn't opened until first use (see
    // ensureSenderPortOpen()) - these buttons only gate on whether a
    // capture is actually running, matching the original spec ("if a
    // capture is currently going on then the play button becomes
    // available"). A failure to actually open the port on first click
    // surfaces as a warning dialog at that point, not as a permanently
    // disabled button.
    const bool canSend = capture_.isRunning();
    startSendingButton_->setEnabled(canSend && !messageSender_.isSending());
    sendNowButton_->setEnabled(canSend);

    stopSendingButton_->setEnabled(canSend && messageSender_.isSending());
    stopSendingButton_->setStyleSheet(messageSender_.isSending()
        ? "background-color: #cc7a00; color: white;"
        : QString());
}

bool MainWindow::ensureSenderPortOpen() {
    if (messageSender_.isPortOpen()) return true;

    QString error;
    if (!messageSender_.openPort(channelCombo_->currentData().toString(), busConfig_, &error)) {
        QMessageBox::warning(this, "Send Message",
            "Could not open a transmit port for this channel:\n" + error);
        return false;
    }
    return true;
}

void MainWindow::onSendFailed(const TransmitMessage& message, const QString& error) {
    if (contentStack_->currentWidget() != stimulationView_) return;

    // Same red-row treatment as a real bus error (see onFrameReceived's
    // frame.error branch) - the message never reached the wire, so there's
    // no DecodedCanFrame to hand to the usual populateDecodedChildren() path.
    QTreeWidgetItem* item = stimulationView_->addReceivedRow(
        QString::number(QDateTime::currentMSecsSinceEpoch() / 1000.0, 'f', 6));
    item->setText(1, QString("0x%1").arg(message.id, message.extended ? 8 : 3, 16, QChar('0')).toUpper());
    item->setText(5, "SEND FAILED: " + error);
    for (int col = 0; col < item->columnCount(); ++col) {
        item->setForeground(col, QBrush(QColor(Qt::red)));
    }
}

void MainWindow::onStartSending() {
    if (!ensureSenderPortOpen()) return;
    messageSender_.start();
    setSendingUiState();
}

void MainWindow::onStopSending() {
    messageSender_.stop();
    setSendingUiState();
}

void MainWindow::onSendNow(const std::vector<int>& indices) {
    if (!ensureSenderPortOpen()) return;
    messageSender_.sendNow(indices);
}

void MainWindow::refreshTransmitList() {
    stimulationView_->refreshTransmitList(messageSender_.messages());
}

void MainWindow::openTransmitMessageDialog(int editIndex) {
    TransmitMessageDialog dialog(this);

    const QString busSummary = busConfig_.fd
        ? QString("CAN FD, %1 bps / %2 bps").arg(busConfig_.nominalBitrateBps).arg(busConfig_.dataBitrateBps)
        : QString("CAN, %1 bps").arg(busConfig_.nominalBitrateBps);
    dialog.setBusInfo(busSummary, busConfig_.fd);

    if (editIndex >= 0 && editIndex < static_cast<int>(messageSender_.messages().size())) {
        dialog.setMessage(messageSender_.messages()[static_cast<size_t>(editIndex)]);
    }

    connect(&dialog, &TransmitMessageDialog::hardwareSettingsRequested, this, [this]() {
        for (int i = 0; i < ribbon_->count(); ++i) {
            if (ribbon_->tabText(i) == "Hardware") { ribbon_->setCurrentIndex(i); break; }
        }
    });

    if (dialog.exec() != QDialog::Accepted) return;

    const TransmitMessage message = dialog.message();
    if (editIndex >= 0) {
        messageSender_.updateMessage(editIndex, message);
    } else {
        messageSender_.addMessage(message);
    }
    refreshTransmitList();
}

void MainWindow::onNewMessage() {
    openTransmitMessageDialog(-1);
}

void MainWindow::onEditMessage(int index) {
    openTransmitMessageDialog(index);
}

void MainWindow::onDeleteMessages(const std::vector<int>& indices) {
    messageSender_.removeMessages(indices);
    refreshTransmitList();
}

void MainWindow::onClearAllMessages() {
    messageSender_.clearMessages();
    refreshTransmitList();
}

void MainWindow::onCopyMessages(const std::vector<int>& indices) {
    transmitClipboard_.clear();
    for (int i : indices) {
        if (i >= 0 && i < static_cast<int>(messageSender_.messages().size())) {
            transmitClipboard_.push_back(messageSender_.messages()[static_cast<size_t>(i)]);
        }
    }
    stimulationView_->setPasteAvailable(!transmitClipboard_.empty());
}

void MainWindow::onCutMessages(const std::vector<int>& indices) {
    onCopyMessages(indices);
    onDeleteMessages(indices);
}

void MainWindow::onPasteMessages() {
    for (const TransmitMessage& message : transmitClipboard_) {
        messageSender_.addMessage(message);
    }
    refreshTransmitList();
}

} // namespace cantrip
