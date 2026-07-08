// Graph view: a chart with a shared time X axis and any number of Y axes,
// each able to host multiple signals dragged in from the left-hand signal
// list. See app/GraphAxisDialog.h for the per-axis/per-signal config
// dialogs this opens.
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "GraphAxisDialog.h"
#include "SignalHistory.h"

QT_BEGIN_NAMESPACE
class QChart;
class QChartView;
class QGraphicsItem;
class QPaintDevice;
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
    // Axis/signal *structure* only (no sample data, which is always
    // live/session-only) - used by RuneFile to save/restore a graph layout.
    struct AxisLayoutSignal {
        QString qualifiedName;
        QColor color;
        GraphLineStyle style = GraphLineStyle::Solid;
    };
    struct AxisLayout {
        QString name;
        bool autoScale = true;
        double min = 0.0;
        double max = 100.0;
        bool hidden = false;
        std::vector<AxisLayoutSignal> plottedSignals;
    };

    explicit GraphView(SignalHistoryStore* history, QWidget* parent = nullptr);

    // Clears every plotted axis/signal and the signal list, tearing down
    // axis structure entirely - use for a genuinely new capture (changing
    // DBC/hardware), not for an ordinary Start/Stop cycle (see clearData()).
    void reset();

    // Empties every series' plotted points and resets the time origin, but
    // keeps axis/signal *structure* untouched (which signals are on which
    // axis, colors, styles, bounds) - this is what an ordinary Start now
    // does, and what the "Clear Graph" button triggers manually mid-capture.
    void clearData();

    // Renders this window's chart to an image/document file, format chosen
    // by the path's extension (.png/.svg/.pdf) - returns false on failure.
    // Used both by the interactive Export button (exportGraph()) and by
    // GraphWindowContainer's "Export All" action.
    bool exportToFile(const QString& path);

    // Exports the current axis/signal structure for saving to a .rune file.
    std::vector<AxisLayout> exportLayout() const;
    // Rebuilds axes/signals from a previously-exported layout - additive,
    // does not clear existing axes first (call reset() before this if a
    // clean slate is wanted, which is what MainWindow's rune-load path does).
    void importLayout(const std::vector<AxisLayout>& layout);

signals:
    // User clicked "Clear Graph" - MainWindow also needs to clear
    // SignalHistoryStore's samples (GraphView doesn't own that), so this
    // just requests it rather than GraphView doing it all internally.
    void clearRequested();

private slots:
    void onSignalAdded(const QString& qualifiedName);
    void onSampleAdded(const QString& qualifiedName, SignalSample sample);
    void onSearchTextChanged(const QString& text);
    void addAxis();
    void onAxisItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onAxisTreeContextMenu(const QPoint& pos);
    void onSignalDropped(QTreeWidgetItem* axisItem, const QStringList& qualifiedNames);
    void onWheelZoom(QPointF chartPos, double factor);
    void resetZoom();
    void exportGraph();
    void flushPendingSignalAdds();
    void onChartMouseMove(QPointF chartPos, bool leftButtonDown);
    void onChartMousePress(QPointF chartPos);
    void onChartMouseRelease(QPointF chartPos);

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

    // Cursor tool: hovering shows a live X/Y readout for every visible
    // signal at that time; click-and-drag on one curve places two points
    // and shows the delta between them (t and value), similar to an
    // oscilloscope/trace-tool cursor pair.
    double timeValueAt(double chartX) const;
    std::optional<QPointF> nearestSampleOnSeries(QXYSeries* series, double timeValue) const;
    // Searches every visible plotted signal for the single closest actual
    // point (by on-screen distance) to a clicked chart position - this is
    // what "select a curve" means when starting a delta measurement.
    QXYSeries* nearestVisibleSeries(const QPointF& chartPos, QPointF* outSampleValue) const;
    void updateHoverCursor(const QPointF& chartPos);
    void updateDeltaCursor(const QPointF& chartPos);
    void clearCursorOverlay();

    // Shared by every export format - QWidget::render() paints identically
    // into a QPixmap, a QSvgGenerator, or a QPrinter.
    void renderChartTo(QPaintDevice* device);

    SignalHistoryStore* history_;

    QLineEdit* searchEdit_;
    QListWidget* signalList_;
    // Signal names that arrived while signalList_ was the source of its own
    // active drag (see SignalListWidget::startDrag) - inserted once the drag
    // finishes instead of during it.
    QStringList pendingSignalAdds_;
    QTreeWidget* axisTree_;
    QPushButton* addAxisButton_;
    QPushButton* zoomSelectButton_;
    QPushButton* zoomResetButton_;
    QPushButton* clearGraphButton_;
    QPushButton* cursorToolButton_;
    QPushButton* exportGraphButton_;

    // Delta-cursor state. cursorSeries_ is the curve a measurement is
    // active on (nullptr = no measurement set up yet) - cleared if that
    // series is ever removed/rebuilt (see removeSignal() and the
    // style-change rebuild in onAxisItemDoubleClicked()) so this can never
    // dangle. Values are in data space (time, physical value), not scene
    // coordinates, so they stay correct across zoom/pan.
    QXYSeries* cursorSeries_ = nullptr;
    bool hasCursorA_ = false;
    bool hasCursorB_ = false;
    bool cursorDragging_ = false;
    QPointF cursorAValue_;
    QPointF cursorBValue_;
    // Overlay graphics (crosshair line(s) + text labels) - torn down and
    // rebuilt from scratch on every update rather than maintained
    // incrementally; signal counts are small enough that this is simpler
    // and still cheap at interactive mouse-move rates.
    QVector<QGraphicsItem*> cursorOverlayItems_;

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
