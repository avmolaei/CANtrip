// The Bus Load tab's content: a left-hand instant-load gauge, a middle
// history graph (an embedded GraphView in simple mode - see GraphView.h),
// and a right-hand statistics panel, modeled on PEAK PCAN-View's own Bus
// Load window. BusLoadTracker owns the actual math; this widget only
// displays what it emits.
#pragma once

#include <QWidget>

#include "BusLoadTracker.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace cantrip {

class GraphView;

// Small vertical bar gauge: filled bottom-up by percentage, numeric
// readout at the bottom, color shifting toward red near 100% - same
// custom-paint spirit as StatusLed, just for a continuous value instead
// of an on/off state.
class BusLoadGauge : public QWidget {
    Q_OBJECT
public:
    explicit BusLoadGauge(QWidget* parent = nullptr);
    QSize sizeHint() const override;

public slots:
    void setPercent(double percent);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double percent_ = 0.0;
};

class BusLoadView : public QWidget {
    Q_OBJECT
public:
    explicit BusLoadView(BusLoadTracker* tracker, QWidget* parent = nullptr);

private slots:
    void onStatsUpdated(const BusLoadStats& stats);
    void onResetClicked();

private:
    BusLoadTracker* tracker_;
    BusLoadGauge* gauge_;
    GraphView* graph_;

    QLabel* maxLabel_;
    QLabel* minLabel_;
    QLabel* maxTimeLabel_;
    QLabel* minTimeLabel_;
    QLabel* meanLabel_;
    QLabel* frameRateLabel_;
    QLabel* totalFramesLabel_;
    QLabel* totalBytesLabel_;
    QLabel* errorRateLabel_;
    QLabel* peakBurstLabel_;
    QLabel* idlePercentLabel_;
    QTreeWidget* perIdTree_;
};

} // namespace cantrip
