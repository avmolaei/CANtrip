// Plain, non-GUI class computing live bus-load statistics from the exact
// same frame stream the Trace/Graph views see - see BusLoadView for the
// display side. Matches the established pattern for real logic living in
// a plain class with thin UI wiring on top (TsharkCapture, MessageSender,
// ILogWriter).
#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "../common/AVlabsCanBackend.h"
#include "SignalHistory.h"
#include "TsharkCapture.h"

namespace cantrip {

struct BusLoadPerIdStats {
    uint32_t id = 0;
    bool extended = false;
    double percentOfBus = 0.0;
    uint64_t frameCount = 0;
    // Real accumulated bus-occupancy time for this ID (seconds) - what
    // percentOfBus is actually derived from, not just a frame-count share.
    double totalBusSeconds = 0.0;
};

struct BusLoadStats {
    bool hasData = false;

    double instantPercent = 0.0;
    double maxPercent = 0.0;
    double minPercent = 0.0;
    QString maxTimestamp;
    QString minTimestamp;
    double meanPercent = 0.0;

    double instantFrameRate = 0.0;
    double averageFrameRate = 0.0;
    uint64_t totalFrames = 0;
    uint64_t totalBytes = 0;
    double errorFrameRate = 0.0;
    uint64_t totalErrorFrames = 0;
    double peakBurstRate = 0.0;
    double idlePercent = 0.0;

    // Sorted by percentOfBus, descending.
    std::vector<BusLoadPerIdStats> perId;
};

class BusLoadTracker : public QObject {
    Q_OBJECT
public:
    explicit BusLoadTracker(QObject* parent = nullptr);

    // Nominal/data bitrate + FD-ness, needed for the bit-time math below -
    // called once whenever a capture starts (MainWindow::startCapture()).
    void setBusConfig(const CanBitrateConfig& config);

    // Clears every running statistic and the plotted history - called on
    // every new capture start and by BusLoadView's Reset button.
    void reset();

    // Dedicated store, not MainWindow's shared signalHistory_ - keeps this
    // synthetic "BusLoad.Percent" pseudo-signal out of the general Graph
    // view's signal list. BusLoadView's embedded GraphView (simple mode)
    // plots straight from this via the normal signalAdded/sampleAdded path.
    SignalHistoryStore* history() { return &history_; }

public slots:
    // Connected directly to TsharkCapture::frameReceived and
    // MessageSender::frameSent (both real bus-occupying traffic) -
    // deliberately NOT LogReplaySource::frameReceived, a replayed log
    // isn't occupying the bus right now. See MainWindow's constructor.
    void recordFrame(const DecodedCanFrame& frame);

signals:
    void statsUpdated(const BusLoadStats& stats);

private slots:
    void onTick();

private:
    // Wall-clock seconds this frame actually occupies the bus at the
    // current busConfig_, split across the nominal/data bitrate as
    // appropriate - see BusLoadTracker.cpp for the bit-count formula and
    // why it's a documented approximation, not exact bit-level simulation.
    double frameSeconds(const DecodedCanFrame& frame) const;

    static constexpr int kTickMs = 250;
    static constexpr double kInstantWindowSec = 1.0;

    CanBitrateConfig busConfig_;
    QElapsedTimer clock_;
    QTimer tickTimer_;

    struct FrameEvent {
        double timeSec = 0.0;
        double busSeconds = 0.0; // this frame's real bus occupancy time
    };
    // Only what's needed for the rolling instant-load/frame-rate window -
    // evicted once older than kInstantWindowSec, so this never grows
    // unbounded regardless of capture length.
    std::deque<FrameEvent> recentEvents_;
    int framesSinceLastTick_ = 0;

    bool hasSample_ = false;
    double maxPercent_ = 0.0;
    double minPercent_ = 0.0;
    QString maxTimestamp_;
    QString minTimestamp_;
    double sumPercentSamples_ = 0.0;
    int percentSampleCount_ = 0;

    uint64_t totalFrames_ = 0;
    uint64_t totalBytes_ = 0;
    uint64_t totalErrorFrames_ = 0;
    double totalBusSecondsAllTime_ = 0.0;
    double peakBurstRate_ = 0.0;
    int idleTicks_ = 0;
    int totalTicks_ = 0;

    // Keyed by (extended << 32 | id) rather than plain id, so a standard
    // and extended frame that happen to share the same numeric ID value
    // are never merged into one row.
    std::unordered_map<uint64_t, BusLoadPerIdStats> perIdStats_;

    SignalHistoryStore history_;
    double lastHistorySampleSec_ = -1.0;
};

} // namespace cantrip
