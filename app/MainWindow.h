#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#include <dbcppp/Network.h>

#include "TsharkCapture.h"

namespace cantrip {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void refreshChannels();
    void importDbc();
    void toggleCapture();
    void onFrameReceived(const DecodedCanFrame& frame);
    void onCaptureError(const QString& message);
    void onCaptureStopped();
    void onDisplayModeChanged();
    void checkStaleRows();

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

    void updateFdControlsEnabled();
    static QString findTsharkExe();
    static uint64_t frameKey(const DecodedCanFrame& frame);
    void resetDisplay();
    void populateDecodedChildren(QTreeWidgetItem* item, const DecodedCanFrame& frame);
    void handleWaterfallFrame(const DecodedCanFrame& frame);
    void handlePeriodicFrame(const DecodedCanFrame& frame);
    void addErrorRow(const DecodedCanFrame& frame);
    void handlePeriodicErrorFrame(const DecodedCanFrame& frame);

    QComboBox* channelCombo_;
    QPushButton* refreshButton_;
    QComboBox* bitrateCombo_;
    QCheckBox* fdCheckBox_;
    QComboBox* dataBitrateCombo_;
    QLineEdit* expertStringEdit_;
    QPushButton* importDbcButton_;
    QLabel* dbcStatusLabel_;
    QPushButton* startStopButton_;
    QComboBox* displayModeCombo_;
    QTreeWidget* frameTree_;

    std::vector<ChannelEntry> channels_;
    std::unique_ptr<dbcppp::INetwork> dbcNetwork_;
    TsharkCapture capture_;
    int frameCount_ = 0;

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
