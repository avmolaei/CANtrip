// Graph view: a chart with a shared time X axis and any number of Y axes,
// each able to host multiple signals dragged in from the left-hand signal
// list. See app/GraphAxisDialog.h for the per-axis/per-signal config
// dialogs this opens.
#pragma once

#include <memory>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include "GraphAxisDialog.h"
#include "SignalHistory.h"

QT_BEGIN_NAMESPACE
class QChart;
class QChartView;
class QValueAxis;
class QXYSeries;
QT_END_NAMESPACE

class QLineEdit;
class QListWidget;
class QPoint;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cantrip {

class GraphView : public QWidget {
    Q_OBJECT
public:
    explicit GraphView(SignalHistoryStore* history, QWidget* parent = nullptr);

    // Clears every plotted axis/signal and the signal list - called when a
    // new capture starts so a fresh run doesn't mix in on old data.
    void reset();

private slots:
    void onSignalAdded(const QString& qualifiedName);
    void onSampleAdded(const QString& qualifiedName, SignalSample sample);
    void onSearchTextChanged(const QString& text);
    void addAxis();
    void onAxisItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onAxisTreeContextMenu(const QPoint& pos);
    void onSignalDropped(QTreeWidgetItem* axisItem, const QString& qualifiedName);
    void onWheelZoom(QPointF chartPos, double factor);
    void resetZoom();

private:
    struct PlottedSignal {
        QString qualifiedName;
        QXYSeries* series = nullptr;
        QColor color;
        GraphLineStyle style = GraphLineStyle::Solid;
        QTreeWidgetItem* item = nullptr;
    };

    struct AxisEntry {
        int number = 0;
        QString name; // user-assigned, empty until renamed via the bounds dialog
        QValueAxis* axis = nullptr;
        QTreeWidgetItem* item = nullptr;
        QPushButton* eyeButton = nullptr;
        bool hidden = false;
        bool autoScale = true;
        double min = 0.0;   // manual bounds, used only when autoScale == false
        double max = 100.0;
        // Observed data bounds across every signal plotted on this axis -
        // tracked ourselves so "Auto" is fully under our control rather
        // than relying on QValueAxis's own implicit auto-range behavior,
        // which can't be re-enabled cleanly once setRange() has been
        // called manually.
        double dataMin = 0.0;
        double dataMax = 1.0;
        bool hasData = false;
        std::vector<PlottedSignal> plottedSignals;
    };

    void refreshAxisItemText(AxisEntry& entry);
    void refreshSignalItemText(PlottedSignal& sig);
    void addSignalToAxis(AxisEntry& entry, const QString& qualifiedName);
    void removeSignal(AxisEntry& entry, PlottedSignal& sig);
    void buildSeriesForSignal(AxisEntry& entry, PlottedSignal& sig);
    void applySeriesStyle(PlottedSignal& sig);
    void applyAxisRange(AxisEntry& entry);
    void trackAxisDataBounds(AxisEntry& entry, double value);
    void toggleAxisVisibility(AxisEntry& entry);
    QColor nextPaletteColor();
    AxisEntry* axisForItem(QTreeWidgetItem* item);
    PlottedSignal* signalForItem(QTreeWidgetItem* item, AxisEntry** ownerAxis = nullptr);
    AxisEntry* axisForValueAxis(QValueAxis* axis);
    AxisEntry* axisNearPoint(const QPointF& chartPos, const QRectF& plotArea, bool leftSide);
    void zoomValueAxis(QValueAxis* axis, const QRectF& plotArea, const QPointF& chartPos, double factor, bool horizontal);

    SignalHistoryStore* history_;

    QLineEdit* searchEdit_;
    QListWidget* signalList_;
    QTreeWidget* axisTree_;
    QPushButton* addAxisButton_;
    QPushButton* zoomSelectButton_;
    QPushButton* zoomResetButton_;

    QChart* chart_;
    QChartView* chartView_;
    QValueAxis* timeAxis_;
    // Full extent of every timestamp seen so far, independent of whatever
    // window the time axis is currently zoomed to - lets "Reset Zoom"
    // restore the true full view rather than whatever the axis's range
    // happened to be overwritten to.
    double timeDataMax_ = 1.0;
    bool timeZoomed_ = false;

    std::vector<std::unique_ptr<AxisEntry>> axes_;
    int axisCounter_ = 0;
    int paletteIndex_ = 0;
};

} // namespace cantrip
