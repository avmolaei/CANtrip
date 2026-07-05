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
#include "StatusLed.h"
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
    void startCapture();
    void stopCapture();
    void onFrameReceived(const DecodedCanFrame& frame);
    void onCaptureError(const QString& message);
    void onCaptureStopped();
    void onDisplayModeChanged();
    void checkStaleRows();
    void showAboutDialog();

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
    QWidget* buildHardwareTab();
    QWidget* buildAnalysisTab();
    QWidget* buildStimulationTab();
    QWidget* buildLoggingTab();
    QWidget* buildAboutTab();

    static QString findTsharkExe();
    static uint64_t frameKey(const DecodedCanFrame& frame);
    void resetDisplay();
    void populateDecodedChildren(QTreeWidgetItem* item, const DecodedCanFrame& frame);
    void handleWaterfallFrame(const DecodedCanFrame& frame);
    void handlePeriodicFrame(const DecodedCanFrame& frame);
    void addErrorRow(const DecodedCanFrame& frame);
    void handlePeriodicErrorFrame(const DecodedCanFrame& frame);

    QTabWidget* ribbon_;

    // Home tab
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QRadioButton* waterfallRadio_;
    QRadioButton* periodicRadio_;

    // Hardware tab
    QComboBox* channelCombo_;
    QPushButton* refreshButton_;
    QPushButton* canControllerButton_;

    // Analysis & Measurement tab
    QPushButton* importDbcButton_;
    QLabel* dbcStatusLabel_;

    QStackedWidget* contentStack_;
    QTreeWidget* frameTree_;

    StatusLed* statusLed_;
    QLabel* statusLabel_;

    std::vector<ChannelEntry> channels_;
    std::unique_ptr<dbcppp::INetwork> dbcNetwork_;
    TsharkCapture capture_;
    int frameCount_ = 0;

    // Populated by the CAN Controller dialog; applied the next time a
    // capture is started (not live-reconfigured while one is running).
    CanBitrateConfig busConfig_;

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
};

} // namespace cantrip
