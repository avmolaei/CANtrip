#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QComboBox>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

#include <dbcppp/Network.h>

#include "../common/AVlabsCanBackend.h"
#include "BusAutoDetector.h"
#include "GraphWindowContainer.h"
#include "LogReplaySource.h"
#include "LogWriter.h"
#include "LoggingOptionsDialog.h"
#include "MessageSender.h"
#include "RuneFile.h"
#include "SignalHistory.h"
#include "StatusLed.h"
#include "StimulationView.h"
#include "TransmitMessage.h"
#include "TsharkCapture.h"

namespace cantrip {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void refreshChannels();
    void importDbc();
    void openCanController();
    void autoDetectBusConfig();
    void saveRune();
    void loadRune();
    void startCapture();
    void stopCapture();
    void onFrameReceived(const DecodedCanFrame& frame);
    void onCaptureError(const QString& message);
    void onCaptureStopped();
    void onDisplayModeChanged();
    void onDisplayRateChanged();
    void flushPendingDisplay();
    void checkStaleRows();
    void showAboutDialog();
    void chooseLogOutputFile();
    void openLoggingOptions();
    void startLogging();
    void stopLogging();
    void startReplay();
    void stopReplay();
    void onReplayStopped();
    void onRibbonTabChanged(int index);
    void onNewMessage();
    void onEditMessage(int index);
    void onSendNow(const std::vector<int>& indices);
    void onDeleteMessages(const std::vector<int>& indices);
    void onClearAllMessages();
    void onCopyMessages(const std::vector<int>& indices);
    void onCutMessages(const std::vector<int>& indices);
    void onPasteMessages();
    void onStartSending();
    void onStopSending();
    void onSendFailed(const TransmitMessage& message, const QString& error);
    void onStimDisplayModeChanged();
    void onClearReceived();

private:
    struct ChannelEntry {
        QString interfaceId;
        QString displayName;
    };

    // Periodic mode keeps exactly one row per CAN ID, updated in place -
    // this struct is the bookkeeping needed for that: when the row last
    // updated (to detect staleness and to compute the period) and whether
    // it's currently shown grayed-out (to avoid re-applying the same brush
    // every timer tick).
    struct PeriodicRowState {
        QTreeWidgetItem* item = nullptr;
        qint64 lastArrivalMs = 0;
        qint64 periodMs = -1;
        bool stale = false;
    };

    enum class DisplayMode { Waterfall, Periodic };

    QWidget* buildHomeTab();
    QWidget* buildPinnedToolbar();
    QWidget* buildHardwareTab();
    QWidget* buildAnalysisTab();
    QWidget* buildStimulationTab();
    QWidget* buildLoggingTab();
    QWidget* buildAboutTab();

    static QString findTsharkExe();
    static QString findCan2pcapExe();
    static uint64_t frameKey(const DecodedCanFrame& frame);
    bool loadDbcFile(const QString& path, QString* error);
    void resetDisplay();
    QString resolveMessageName(const DecodedCanFrame& frame) const;
    QString currentBusNameForFilename() const;
    std::unique_ptr<ILogWriter> makeLogWriter() const;
    QString currentLogExtension() const;
    void rotateLogFileIfNeeded();
    void setLoggingUiRunning(bool running);
    void setOutputFileLabel(const QString& path);
    void setCaptureControlsEnabledForReplay(bool replaying);
    void populateDecodedChildren(QTreeWidgetItem* item, const DecodedCanFrame& frame);
    void handleWaterfallFrame(const DecodedCanFrame& frame);
    void handlePeriodicFrame(const DecodedCanFrame& frame);
    void addErrorRow(const DecodedCanFrame& frame);
    void handlePeriodicErrorFrame(const DecodedCanFrame& frame);
    void handleStimWaterfallFrame(const DecodedCanFrame& frame);
    void handleStimPeriodicFrame(const DecodedCanFrame& frame);
    void addStimErrorRow(const DecodedCanFrame& frame);
    void handleStimPeriodicErrorFrame(const DecodedCanFrame& frame);
    void openTransmitMessageDialog(int editIndex); // -1 = new message
    void refreshTransmitList();
    void setSendingUiState();
    bool ensureSenderPortOpen();

    QTabWidget* ribbon_;

    // Home tab
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QRadioButton* waterfallRadio_;
    QRadioButton* periodicRadio_;
    QComboBox* displayRateCombo_;
    QPushButton* saveRuneButton_;
    QPushButton* loadRuneButton_;

    // Hardware tab
    QComboBox* channelCombo_;
    QPushButton* refreshButton_;
    QPushButton* canControllerButton_;
    QPushButton* autoDetectButton_;

    // Analysis & Measurement tab
    QPushButton* importDbcButton_;
    QLabel* dbcStatusLabel_;

    // Logging tab
    QPushButton* startLoggingButton_;
    QPushButton* stopLoggingButton_;
    QPushButton* outputFileButton_;
    QLabel* outputFileLabel_;
    QRadioButton* ascFormatRadio_;
    QRadioButton* csvFormatRadio_;
    QRadioButton* mf4FormatRadio_;
    QPushButton* loggingOptionsButton_;
    QPushButton* startReplayButton_;
    QPushButton* stopReplayButton_;

    // Stimulation tab
    QPushButton* newMessageButton_;
    QPushButton* startSendingButton_;
    QPushButton* stopSendingButton_;
    QPushButton* sendNowButton_;
    QPushButton* clearReceivedButton_;
    QRadioButton* stimWaterfallRadio_;
    QRadioButton* stimPeriodicRadio_;
    DisplayMode stimDisplayMode_ = DisplayMode::Periodic;
    // Mirrors periodicRows_/periodicErrorRows_ but for the Stimulation
    // tab's own received pane - kept separate since its display mode is
    // independent of the Home tab's (see stimDisplayMode_).
    std::unordered_map<uint64_t, PeriodicRowState> stimPeriodicRows_;
    QHash<QString, PeriodicRowState> stimPeriodicErrorRows_;

    QStackedWidget* contentStack_;
    QTreeWidget* frameTree_;
    GraphWindowContainer* graphWindows_;
    StimulationView* stimulationView_;
    SignalHistoryStore signalHistory_;
    MessageSender messageSender_;
    // MainWindow owns this rather than StimulationView since it's the real
    // TransmitMessage data (StimulationView only ever sees display strings/
    // indices) - see StimulationView::setPasteAvailable.
    std::vector<TransmitMessage> transmitClipboard_;

    StatusLed* statusLed_;
    QLabel* statusLabel_;

    std::vector<ChannelEntry> channels_;
    std::unique_ptr<dbcppp::INetwork> dbcNetwork_;
    // Built once in importDbc() so populateDecodedChildren() (called on
    // every single received frame) doesn't have to linearly scan every
    // message in the DBC to find the one matching a frame's ID - a real,
    // measurable cost on a large real-world DBC at real bus rates.
    std::unordered_map<uint32_t, const dbcppp::IMessage*> messageById_;
    // Path of the currently-loaded DBC, if any - needed to populate
    // RuneConfig::dbcPath on save; empty means no DBC is loaded.
    QString dbcPath_;
    TsharkCapture capture_;
    int frameCount_ = 0;

    // Populated by the CAN Controller dialog; applied the next time a
    // capture is started (not live-reconfigured while one is running).
    CanBitrateConfig busConfig_;
    // "Request bus configuration" checkbox state, same dialog - kept
    // separate from busConfig_ since it's an access-rights concern, not a
    // bus-timing one. See TsharkCapture::Config::listenOnly.
    bool listenOnlyMode_ = false;

    DisplayMode displayMode_ = DisplayMode::Waterfall;
    std::unordered_map<uint64_t, PeriodicRowState> periodicRows_;
    // Bus errors have no CAN ID to key a periodic row on, so they get their
    // own map keyed by error description instead (one row per distinct
    // error type, e.g. "Stuff Error", updated in place same as a data ID).
    // QHash rather than std::unordered_map since Qt doesn't provide
    // std::hash<QString> (it does provide qHash, which QHash uses).
    QHash<QString, PeriodicRowState> periodicErrorRows_;
    QTimer staleTimer_;

    // A row with no new frame for this long is grayed out in periodic mode -
    // deliberately a flat timeout rather than a multiple of the measured
    // period, since a message seen only once has no period to multiply yet.
    static constexpr qint64 kStaleTimeoutMs = 2000;

    // Display-rate throttle: a real, busy CAN bus can produce far more
    // frames/sec than a full decode + tree-widget/chart-series mutation per
    // frame can keep up with in real time - confirmed via ProcDump + cdb
    // (the main thread was CPU-saturated re-processing an ever-refilling
    // backlog, not deadlocked, so capping batch size alone doesn't fix it).
    // 0 means unlimited (decode/paint every frame immediately, exact
    // original behavior); otherwise only the latest frame per row
    // (Periodic) / queued frames (Waterfall) get displayed, applied by
    // displayFlushTimer_ at this interval - see onFrameReceived() and
    // flushPendingDisplay(). Bus errors are always shown immediately,
    // regardless of this setting - they're comparatively rare and
    // important enough not to throttle.
    int displayIntervalMs_ = 33;
    QTimer displayFlushTimer_;
    std::unordered_map<uint64_t, DecodedCanFrame> pendingPeriodicFrames_;
    std::vector<DecodedCanFrame> pendingWaterfallFrames_;

    // Logging state
    std::unique_ptr<ILogWriter> logWriter_;
    bool logging_ = false;
    // Set when Start Capture auto-started logging (LoggingOptionsDialog's
    // "auto-start" checkbox) - distinguishes that case from the user having
    // manually started logging independently, so stopping a capture doesn't
    // stop a log the user asked for explicitly and separately.
    bool loggingAutoStarted_ = false;
    QString logOutputPath_;
    // True only once the user explicitly picks a path via "Output file..." -
    // otherwise startLogging() regenerates a fresh path from the template
    // (new timestamp, current format) every time rather than reusing
    // whatever path the previous session happened to end up at.
    bool logOutputPathExplicit_ = false;
    QString logFilenameTemplate_ = "[user]_[bus]_[date]_[time]";
    int logMaxFileSizeMb_ = 0;
    LoggingOptionsDialog::ExistingFilePolicy logExistingFilePolicy_ = LoggingOptionsDialog::ExistingFilePolicy::AutoIncrement;
    bool logAutoStartWithCapture_ = false;
    // Bumped on every auto-split rollover so the next file gets a distinct
    // "..._partN" name instead of colliding with the previous one.
    int logSplitSequence_ = 0;

    LogReplaySource replaySource_;
};

} // namespace cantrip
