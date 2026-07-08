#include "GraphView.h"

#include <algorithm>
#include <limits>

#include <QBrush>
#include <QColor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QGraphicsLineItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>
#include <QtCharts/QXYSeries>

namespace cantrip {

namespace {

// A small fixed palette cycled as new signals are plotted, so multiple
// series sharing one axis are distinguishable before the user customizes
// anything via GraphSignalStyleDialog.
const QColor kPalette[] = {
    QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c),
    QColor(0xd6, 0x27, 0x28), QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b),
    QColor(0xe3, 0x77, 0xc2), QColor(0x7f, 0x7f, 0x7f),
};

Qt::PenStyle toPenStyle(GraphLineStyle style) {
    switch (style) {
        case GraphLineStyle::Dashed: return Qt::DashLine;
        case GraphLineStyle::Dotted: return Qt::DotLine;
        default: return Qt::SolidLine;
    }
}

const char* toStyleLabel(GraphLineStyle style) {
    switch (style) {
        case GraphLineStyle::Dashed: return "Dashed";
        case GraphLineStyle::Dotted: return "Dotted";
        case GraphLineStyle::Scatter: return "Scatter";
        default: return "Solid";
    }
}

} // namespace

// Not declared in GraphView.h (private to this translation unit) but
// deliberately NOT in an anonymous namespace either - Q_OBJECT classes and
// anonymous namespaces don't mix well with moc/ODR, so these live directly
// in the cantrip namespace instead, same as GraphView itself.

// Overridden only to make dragged items carry their qualified signal name
// as plain text - QListWidget's default drag payload is a Qt-internal
// item-model MIME type, not something a plain QTreeWidget drop target can
// read directly.
class SignalListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit SignalListWidget(QWidget* parent = nullptr) : QListWidget(parent) {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        // Ctrl+click / Shift+click to select several signals, then drag them
        // all onto an axis at once - default QListWidget selection is
        // single-item only.
        setSelectionMode(QAbstractItemView::ExtendedSelection);
    }

    bool isDragInProgress() const { return dragInProgress_; }

signals:
    // Emitted once QAbstractItemView::startDrag()'s own nested event loop
    // (started internally by QDrag::exec()) has fully unwound.
    void dragFinished();

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        auto* mime = new QMimeData();
        // Newline-joined rather than a custom MIME type: keeps the drop
        // target (AxisTreeWidget, a plain QTreeWidget) able to read it via
        // the same hasText()/text() path a single-signal drag already used.
        QStringList names;
        for (const QListWidgetItem* item : items) names << item->text();
        mime->setText(names.join('\n'));
        return mime;
    }

    // QAbstractItemView::startDrag() blocks inside QDrag::exec()'s own
    // nested event loop for as long as the mouse button is held - during
    // which TsharkCapture's socket-notifier-driven frame decoding keeps
    // running normally. If a brand-new signal type showed up mid-drag,
    // GraphView::onSignalAdded() would otherwise insert a new item into
    // this exact list *while it's the active drag source* - a real Qt
    // reentrancy hazard (this is the crash the user hit dragging a signal
    // during a live capture: reproduced by starting a capture against a DBC
    // with signals not seen yet, then dragging before they'd all appeared).
    // Track drag-in-progress so GraphView can defer those insertions until
    // the drag has fully finished instead.
    void startDrag(Qt::DropActions supportedActions) override {
        dragInProgress_ = true;
        QListWidget::startDrag(supportedActions);
        dragInProgress_ = false;
        emit dragFinished();
    }

private:
    bool dragInProgress_ = false;
};

// Accepts drops of a signal's qualified name (plain text) and reports which
// top-level (axis) item it landed on - GraphView resolves the rest.
class AxisTreeWidget : public QTreeWidget {
    Q_OBJECT
public:
    explicit AxisTreeWidget(QWidget* parent = nullptr) : QTreeWidget(parent) {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
    }

signals:
    void signalDropped(QTreeWidgetItem* axisItem, const QStringList& qualifiedNames);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasText()) event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->mimeData()->hasText()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        if (!event->mimeData()->hasText()) return;
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        while (target && target->parent()) target = target->parent();
        if (target) {
            const QStringList names = event->mimeData()->text().split('\n', Qt::SkipEmptyParts);
            emit signalDropped(target, names);
        }
        event->acceptProposedAction();
    }
};

// Qt Charts has no built-in wheel-to-zoom - this reports the wheel event's
// position (in the chart's own local coordinate system, matching
// QChart::plotArea()) and zoom direction back to GraphView, which decides
// whether that's over the time axis, a stacked Y axis, or the plot body.
class GraphChartView : public QChartView {
    Q_OBJECT
public:
    explicit GraphChartView(QChart* chart, QWidget* parent = nullptr) : QChartView(chart, parent) {
        // Needed so mouseMoveEvent fires on plain hover, not just while a
        // button is held - the hover-cursor readout needs this.
        setMouseTracking(true);
    }

signals:
    void wheelZoom(QPointF chartPos, double factor);
    // Raw chart-local mouse position/button-state reports - GraphView owns
    // deciding what these mean (hover cursor vs. delta-drag vs. nothing),
    // this class just reports geometry. The base class's own handling
    // (rubber-band zoom-select) still runs too, via the explicit base calls
    // below, so that tool keeps working independently of these signals.
    void chartMouseMove(QPointF chartPos, bool leftButtonDown);
    void chartMousePress(QPointF chartPos);
    void chartMouseRelease(QPointF chartPos);

protected:
    void wheelEvent(QWheelEvent* event) override {
        const double factor = event->angleDelta().y() > 0 ? 0.9 : (1.0 / 0.9);
        const QPointF chartPos = toChartPos(event->position());
        emit wheelZoom(chartPos, factor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QChartView::mouseMoveEvent(event);
        emit chartMouseMove(toChartPos(event->position()), event->buttons() & Qt::LeftButton);
    }
    void mousePressEvent(QMouseEvent* event) override {
        QChartView::mousePressEvent(event);
        if (event->button() == Qt::LeftButton) emit chartMousePress(toChartPos(event->position()));
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QChartView::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton) emit chartMouseRelease(toChartPos(event->position()));
    }

private:
    QPointF toChartPos(const QPointF& widgetPos) const {
        return chart()->mapFromScene(mapToScene(widgetPos.toPoint()));
    }
};

GraphView::GraphView(SignalHistoryStore* history, QWidget* parent)
    : QWidget(parent), history_(history) {
    auto* root = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);

    leftLayout->addWidget(new QLabel("Signals", leftPanel));
    searchEdit_ = new QLineEdit(leftPanel);
    searchEdit_->setPlaceholderText("Search signals...");
    leftLayout->addWidget(searchEdit_);
    signalList_ = new SignalListWidget(leftPanel);
    leftLayout->addWidget(signalList_, /*stretch=*/1);
    connect(static_cast<SignalListWidget*>(signalList_), &SignalListWidget::dragFinished,
            this, &GraphView::flushPendingSignalAdds);

    addAxisButton_ = new QPushButton("Add Y Axis", leftPanel);
    leftLayout->addWidget(addAxisButton_);
    axisTree_ = new AxisTreeWidget(leftPanel);
    axisTree_->setColumnCount(2);
    axisTree_->setHeaderLabels({"Axes", ""});
    axisTree_->header()->setStretchLastSection(false);
    axisTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    axisTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    axisTree_->setColumnWidth(1, 30);
    axisTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    leftLayout->addWidget(axisTree_, /*stretch=*/1);

    chart_ = new QChart();
    chart_->legend()->hide(); // series names are already shown in the axis tree
    timeAxis_ = new QValueAxis(chart_);
    timeAxis_->setTitleText("Time (s)");
    timeAxis_->setLineVisible(true);
    chart_->addAxis(timeAxis_, Qt::AlignBottom);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout();
    zoomSelectButton_ = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8D"), rightPanel); // magnifier
    zoomSelectButton_->setCheckable(true);
    zoomSelectButton_->setToolTip("Drag a rectangle to zoom into it");
    zoomResetButton_ = new QPushButton(QString::fromUtf8("\xF0\x9F\x94\x8D-"), rightPanel); // magnifier + "-"
    zoomResetButton_->setToolTip("Reset zoom to the full capture");
    clearGraphButton_ = new QPushButton("Clear Graph", rightPanel);
    clearGraphButton_->setToolTip("Clear all plotted data and restart the timeline from 0 - keeps axis setup");
    cursorToolButton_ = new QPushButton(QString::fromUtf8("\xE2\x9C\x9A"), rightPanel); // ruler/cursor glyph
    cursorToolButton_->setCheckable(true);
    cursorToolButton_->setToolTip("Hover for X/Y values; click and drag on a curve to measure Δt/Δvalue");
    toolbar->addWidget(zoomSelectButton_);
    toolbar->addWidget(zoomResetButton_);
    toolbar->addWidget(cursorToolButton_);
    toolbar->addWidget(clearGraphButton_);
    toolbar->addStretch(1);
    rightLayout->addLayout(toolbar);

    chartView_ = new GraphChartView(chart_, rightPanel);
    chartView_->setRenderHint(QPainter::Antialiasing);
    rightLayout->addWidget(chartView_, /*stretch=*/1);

    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 750});

    root->addWidget(splitter);

    connect(searchEdit_, &QLineEdit::textChanged, this, &GraphView::onSearchTextChanged);
    connect(addAxisButton_, &QPushButton::clicked, this, &GraphView::addAxis);
    connect(axisTree_, &QTreeWidget::itemDoubleClicked, this, &GraphView::onAxisItemDoubleClicked);
    connect(axisTree_, &QTreeWidget::customContextMenuRequested, this, &GraphView::onAxisTreeContextMenu);
    connect(static_cast<AxisTreeWidget*>(axisTree_), &AxisTreeWidget::signalDropped, this, &GraphView::onSignalDropped);

    connect(zoomSelectButton_, &QPushButton::toggled, this, [this](bool checked) {
        chartView_->setRubberBand(checked ? QChartView::RectangleRubberBand : QChartView::NoRubberBand);
        if (checked) {
            // Picking up the zoom tool hands range control to the user -
            // "Reset Zoom" is what hands it back to auto-fit.
            timeZoomed_ = true;
            for (auto& a : axes_) a->autoScale = false;
            cursorToolButton_->setChecked(false); // mutually exclusive with the cursor tool
        }
    });
    connect(zoomResetButton_, &QPushButton::clicked, this, &GraphView::resetZoom);
    connect(clearGraphButton_, &QPushButton::clicked, this, &GraphView::clearRequested);
    connect(cursorToolButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            zoomSelectButton_->setChecked(false); // mutually exclusive with rubber-band zoom
        } else {
            clearCursorOverlay();
        }
    });
    connect(static_cast<GraphChartView*>(chartView_), &GraphChartView::wheelZoom, this, &GraphView::onWheelZoom);
    connect(static_cast<GraphChartView*>(chartView_), &GraphChartView::chartMouseMove, this, &GraphView::onChartMouseMove);
    connect(static_cast<GraphChartView*>(chartView_), &GraphChartView::chartMousePress, this, &GraphView::onChartMousePress);
    connect(static_cast<GraphChartView*>(chartView_), &GraphChartView::chartMouseRelease, this, &GraphView::onChartMouseRelease);

    connect(history_, &SignalHistoryStore::signalAdded, this, &GraphView::onSignalAdded);
    connect(history_, &SignalHistoryStore::sampleAdded, this, &GraphView::onSampleAdded);

    for (const QString& name : history_->signalNames()) onSignalAdded(name);
}

void GraphView::reset() {
    for (auto& axisPtr : axes_) {
        for (auto& sig : axisPtr->plottedSignals) {
            chart_->removeSeries(sig.series);
            delete sig.series;
        }
        chart_->removeAxis(axisPtr->axis);
        delete axisPtr->axis;
    }
    axes_.clear();
    axisTree_->clear();
    signalList_->clear();
    axisCounter_ = 0;
    paletteIndex_ = 0;
    timeDataMax_ = 1.0;
    timeZoomed_ = false;
    zoomSelectButton_->setChecked(false);
    timeAxis_->setRange(0, 1);
    // Every series just got destroyed - a dangling cursorSeries_ would be a
    // real use-after-free the next time the cursor tool is used.
    cursorSeries_ = nullptr;
    hasCursorA_ = hasCursorB_ = false;
    clearCursorOverlay();
}

void GraphView::clearData() {
    for (auto& axisPtr : axes_) {
        for (auto& sig : axisPtr->plottedSignals) {
            sig.series->clear();
        }
        axisPtr->dataMin = 0.0;
        axisPtr->dataMax = 1.0;
        axisPtr->hasData = false;
        // Auto-scaling axes would otherwise keep showing the old data's
        // range until a fresh sample arrives to overwrite it - snap back to
        // a neutral default immediately instead of waiting.
        if (axisPtr->autoScale) axisPtr->axis->setRange(0, 1);
    }
    timeDataMax_ = 1.0;
    timeZoomed_ = false;
    zoomSelectButton_->setChecked(false);
    timeAxis_->setRange(0, 1);
    // The series themselves survive clearData() (unlike reset()), but their
    // point data doesn't - any in-progress delta measurement no longer
    // means anything against emptied series.
    cursorSeries_ = nullptr;
    hasCursorA_ = hasCursorB_ = false;
    clearCursorOverlay();
}

std::vector<GraphView::AxisLayout> GraphView::exportLayout() const {
    std::vector<AxisLayout> result;
    for (auto& axisPtr : axes_) {
        AxisLayout al;
        al.name = axisPtr->name;
        al.autoScale = axisPtr->autoScale;
        al.min = axisPtr->min;
        al.max = axisPtr->max;
        al.hidden = axisPtr->hidden;
        for (auto& sig : axisPtr->plottedSignals) {
            al.plottedSignals.push_back({sig.qualifiedName, sig.color, sig.style});
        }
        result.push_back(std::move(al));
    }
    return result;
}

void GraphView::importLayout(const std::vector<AxisLayout>& layout) {
    for (const AxisLayout& al : layout) {
        addAxis();
        AxisEntry& entry = *axes_.back();
        entry.name = al.name;
        entry.autoScale = al.autoScale;
        entry.min = al.min;
        entry.max = al.max;
        applyAxisRange(entry);
        refreshAxisItemText(entry);

        for (const AxisLayoutSignal& sl : al.plottedSignals) {
            addSignalToAxis(entry, sl.qualifiedName);
            PlottedSignal* sig = nullptr;
            for (auto& s : entry.plottedSignals) {
                if (s.qualifiedName == sl.qualifiedName) { sig = &s; break; }
            }
            if (!sig) continue; // addSignalToAxis no-ops on an unrecognized/duplicate name

            sig->color = sl.color;
            if (sl.style != sig->style) {
                // Scatter <-> Line needs a different QXYSeries subclass -
                // same rebuild-in-place pattern as the style dialog's own
                // handling in onAxisItemDoubleClicked().
                QXYSeries* oldSeries = sig->series;
                sig->style = sl.style;
                buildSeriesForSignal(entry, *sig);
                sig->series->attachAxis(timeAxis_);
                sig->series->attachAxis(entry.axis);
                chart_->removeSeries(oldSeries);
                delete oldSeries;
            }
            applySeriesStyle(*sig);
            refreshSignalItemText(*sig);
        }

        if (al.hidden) toggleAxisVisibility(entry); // reuses the same hide effects as the eye button
    }
}

void GraphView::onSignalAdded(const QString& qualifiedName) {
    // Guard against re-adding a name already present (e.g. the constructor's
    // initial population racing a live signalAdded emission).
    const QList<QListWidgetItem*> existing = signalList_->findItems(qualifiedName, Qt::MatchExactly);
    if (!existing.isEmpty()) return;

    // Inserting into signalList_ while it's the source of its own active
    // drag is a real Qt reentrancy hazard (see SignalListWidget::startDrag) -
    // defer until the drag finishes rather than mutate the list mid-drag.
    if (static_cast<SignalListWidget*>(signalList_)->isDragInProgress()) {
        if (!pendingSignalAdds_.contains(qualifiedName)) pendingSignalAdds_.append(qualifiedName);
        return;
    }

    auto* item = new QListWidgetItem(qualifiedName, signalList_);
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    item->setHidden(!searchEdit_->text().isEmpty() && !qualifiedName.contains(searchEdit_->text(), Qt::CaseInsensitive));
}

void GraphView::flushPendingSignalAdds() {
    const QStringList pending = pendingSignalAdds_;
    pendingSignalAdds_.clear();
    for (const QString& name : pending) onSignalAdded(name);
}

void GraphView::onSampleAdded(const QString& qualifiedName, SignalSample sample) {
    if (sample.timeSec > timeDataMax_) timeDataMax_ = sample.timeSec;
    if (!timeZoomed_ && sample.timeSec > timeAxis_->max()) timeAxis_->setMax(sample.timeSec);

    for (auto& axisPtr : axes_) {
        bool touched = false;
        for (auto& sig : axisPtr->plottedSignals) {
            if (sig.qualifiedName != qualifiedName) continue;
            sig.series->append(sample.timeSec, sample.value);
            touched = true;
        }
        if (!touched) continue;
        trackAxisDataBounds(*axisPtr, sample.value);
        if (axisPtr->autoScale) applyAxisRange(*axisPtr);
    }
}

void GraphView::onSearchTextChanged(const QString& text) {
    for (int i = 0; i < signalList_->count(); ++i) {
        QListWidgetItem* item = signalList_->item(i);
        item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
    }
}

void GraphView::addAxis() {
    auto entry = std::make_unique<AxisEntry>();
    entry->number = ++axisCounter_;
    entry->axis = new QValueAxis(chart_);
    entry->axis->setLineVisible(true);
    entry->axis->setTitleText(QString("Axis %1").arg(entry->number));
    // All Y axes stack on the left, per explicit request - simpler to read
    // than alternating sides, and axisNearPoint()'s wheel-zoom hit-testing
    // only has to consider one margin.
    chart_->addAxis(entry->axis, Qt::AlignLeft);

    auto* item = new QTreeWidgetItem(axisTree_);
    entry->item = item;
    item->setData(0, Qt::UserRole, QVariant::fromValue<void*>(entry.get()));

    auto* eyeButton = new QPushButton(QString::fromUtf8("\xF0\x9F\x91\x81")); // eye emoji
    eyeButton->setCheckable(true);
    eyeButton->setFlat(true);
    eyeButton->setFixedWidth(28);
    eyeButton->setToolTip("Hide this axis");
    axisTree_->setItemWidget(item, 1, eyeButton);
    entry->eyeButton = eyeButton;

    AxisEntry* raw = entry.get();
    connect(eyeButton, &QPushButton::toggled, this, [this, raw](bool) { toggleAxisVisibility(*raw); });

    axes_.push_back(std::move(entry));
    refreshAxisItemText(*raw);
    item->setExpanded(true);
}

GraphView::AxisEntry* GraphView::axisForItem(QTreeWidgetItem* item) {
    if (!item) return nullptr;
    QTreeWidgetItem* topLevel = item;
    while (topLevel->parent()) topLevel = topLevel->parent();
    return static_cast<AxisEntry*>(topLevel->data(0, Qt::UserRole).value<void*>());
}

GraphView::PlottedSignal* GraphView::signalForItem(QTreeWidgetItem* item, AxisEntry** ownerAxis) {
    if (!item || !item->parent()) return nullptr;
    AxisEntry* entry = axisForItem(item);
    if (ownerAxis) *ownerAxis = entry;
    if (!entry) return nullptr;
    for (auto& sig : entry->plottedSignals) {
        if (sig.item == item) return &sig;
    }
    return nullptr;
}

void GraphView::onSignalDropped(QTreeWidgetItem* axisItem, const QStringList& qualifiedNames) {
    AxisEntry* entry = axisForItem(axisItem);
    if (!entry) return;
    for (const QString& name : qualifiedNames) addSignalToAxis(*entry, name);
}

QColor GraphView::nextPaletteColor() {
    const QColor c = kPalette[paletteIndex_ % (sizeof(kPalette) / sizeof(kPalette[0]))];
    ++paletteIndex_;
    return c;
}

void GraphView::addSignalToAxis(AxisEntry& entry, const QString& qualifiedName) {
    for (auto& sig : entry.plottedSignals) {
        if (sig.qualifiedName == qualifiedName) return; // already plotted on this axis
    }

    entry.plottedSignals.push_back(PlottedSignal{});
    PlottedSignal& sig = entry.plottedSignals.back();
    sig.qualifiedName = qualifiedName;
    sig.color = nextPaletteColor();
    sig.style = GraphLineStyle::Solid;

    auto* item = new QTreeWidgetItem(entry.item);
    sig.item = item;

    buildSeriesForSignal(entry, sig);
    sig.series->attachAxis(timeAxis_);
    sig.series->attachAxis(entry.axis);
    applySeriesStyle(sig);
    refreshSignalItemText(sig);

    if (entry.hidden) {
        // Newly-added signals on an already-hidden axis inherit that state
        // rather than popping into view on their own.
        sig.series->setVisible(false);
        sig.item->setForeground(0, QBrush(QColor(Qt::gray)));
    }

    entry.item->setExpanded(true);
}

void GraphView::toggleAxisVisibility(AxisEntry& entry) {
    entry.hidden = !entry.hidden;
    entry.axis->setVisible(!entry.hidden);
    for (auto& sig : entry.plottedSignals) {
        sig.series->setVisible(!entry.hidden);
    }

    const QBrush brush = entry.hidden ? QBrush(QColor(Qt::gray)) : QBrush();
    entry.item->setForeground(0, brush);
    for (int i = 0; i < entry.item->childCount(); ++i) {
        entry.item->child(i)->setForeground(0, brush);
    }

    if (entry.eyeButton) {
        entry.eyeButton->setToolTip(entry.hidden ? "Show this axis" : "Hide this axis");
    }
}

void GraphView::trackAxisDataBounds(AxisEntry& entry, double value) {
    if (!entry.hasData) {
        entry.dataMin = entry.dataMax = value;
        entry.hasData = true;
    } else {
        entry.dataMin = std::min(entry.dataMin, value);
        entry.dataMax = std::max(entry.dataMax, value);
    }
}

void GraphView::applyAxisRange(AxisEntry& entry) {
    if (entry.autoScale) {
        if (!entry.hasData) return;
        const double lo = entry.dataMin;
        const double hi = entry.dataMax;
        entry.axis->setRange(lo == hi ? lo - 1.0 : lo, lo == hi ? hi + 1.0 : hi);
    } else {
        entry.axis->setRange(entry.min, entry.max);
    }
}

void GraphView::buildSeriesForSignal(AxisEntry& entry, PlottedSignal& sig) {
    if (sig.style == GraphLineStyle::Scatter) {
        auto* scatter = new QScatterSeries();
        scatter->setMarkerSize(6.0);
        sig.series = scatter;
    } else {
        sig.series = new QLineSeries();
    }
    sig.series->setName(sig.qualifiedName);

    // Snapshot (a real copy, not a reference) rather than iterate
    // samplesFor()'s result directly: this backfill loop runs synchronously
    // from within AxisTreeWidget::dropEvent(), which - for a drag sourced
    // from signalList_ - executes while still nested inside QDrag::exec()'s
    // own event loop. Live capture decoding keeps running during that nested
    // loop, and SignalHistoryStore::recordSample() appends to this exact
    // signal's QVector on every new sample - if that reallocates while a
    // live reference into it is mid-iteration here, it's a real use-after-
    // free (the actual crash reported when dropping a signal during a live
    // capture). QVector is copy-on-write, so this copy is cheap and stays
    // valid even if recordSample() appends to the "live" vector afterward -
    // Qt detaches the live side instead of mutating shared data in place.
    const QVector<SignalSample> snapshot = history_->samplesFor(sig.qualifiedName);
    for (const SignalSample& s : snapshot) {
        sig.series->append(s.timeSec, s.value);
        if (s.timeSec > timeDataMax_) timeDataMax_ = s.timeSec;
        if (!timeZoomed_ && s.timeSec > timeAxis_->max()) timeAxis_->setMax(s.timeSec);
        trackAxisDataBounds(entry, s.value);
    }

    chart_->addSeries(sig.series);
    applyAxisRange(entry);
}

void GraphView::applySeriesStyle(PlottedSignal& sig) {
    sig.series->setColor(sig.color);
    QPen pen = sig.series->pen();
    pen.setColor(sig.color);
    pen.setStyle(toPenStyle(sig.style));
    pen.setWidth(2);
    sig.series->setPen(pen);
}

void GraphView::refreshAxisItemText(AxisEntry& entry) {
    const QString label = entry.name.isEmpty() ? QString("Axis %1").arg(entry.number) : entry.name;
    const QString bounds = entry.autoScale
        ? "Auto"
        : QString("[%1, %2]").arg(entry.min).arg(entry.max);
    entry.item->setText(0, QString("%1 (%2)").arg(label, bounds));
    entry.axis->setTitleText(label);
}

void GraphView::refreshSignalItemText(PlottedSignal& sig) {
    QPixmap swatch(10, 10);
    swatch.fill(sig.color);
    sig.item->setIcon(0, QIcon(swatch));
    sig.item->setText(0, QString("%1 — %2").arg(sig.qualifiedName, toStyleLabel(sig.style)));
}

void GraphView::removeSignal(AxisEntry& entry, PlottedSignal& sig) {
    if (cursorSeries_ == sig.series) {
        cursorSeries_ = nullptr;
        hasCursorA_ = hasCursorB_ = false;
        clearCursorOverlay();
    }
    chart_->removeSeries(sig.series);
    delete sig.series;
    delete sig.item;
    for (auto it = entry.plottedSignals.begin(); it != entry.plottedSignals.end(); ++it) {
        if (&*it == &sig) {
            entry.plottedSignals.erase(it);
            break;
        }
    }
}

void GraphView::onAxisItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;

    if (!item->parent()) {
        // Top-level item = an axis: edit its shared bounds.
        AxisEntry* entry = axisForItem(item);
        if (!entry) return;

        GraphAxisBoundsDialog dialog(this);
        dialog.setName(entry->name.isEmpty() ? QString("Axis %1").arg(entry->number) : entry->name);
        dialog.setBounds(entry->autoScale, entry->min, entry->max);
        if (dialog.exec() != QDialog::Accepted) return;

        const QString defaultName = QString("Axis %1").arg(entry->number);
        entry->name = (dialog.name() == defaultName) ? QString() : dialog.name();
        entry->autoScale = dialog.autoScale();
        entry->min = dialog.minValue();
        entry->max = dialog.maxValue();
        applyAxisRange(*entry);
        refreshAxisItemText(*entry);
        return;
    }

    // Child item = a signal on some axis: edit its color/line style.
    AxisEntry* owner = nullptr;
    PlottedSignal* sig = signalForItem(item, &owner);
    if (!sig) return;

    GraphSignalStyleDialog dialog(this);
    dialog.setStyle(sig->color, sig->style);
    if (dialog.exec() != QDialog::Accepted) return;

    const bool styleChanged = dialog.lineStyle() != sig->style;
    sig->color = dialog.color();
    sig->style = dialog.lineStyle();

    if (styleChanged) {
        // Scatter <-> Line needs a different QXYSeries subclass; rebuild it
        // in place rather than trying to morph one series type into another.
        QXYSeries* oldSeries = sig->series;
        buildSeriesForSignal(*owner, *sig);
        sig->series->attachAxis(timeAxis_);
        sig->series->attachAxis(owner->axis);
        chart_->removeSeries(oldSeries);
        // Retarget rather than clear - this is still logically the same
        // signal, just restyled, so an active delta measurement on it
        // should keep working against the rebuilt series.
        if (cursorSeries_ == oldSeries) cursorSeries_ = sig->series;
        delete oldSeries;
    }
    applySeriesStyle(*sig);
    refreshSignalItemText(*sig);
}

void GraphView::onAxisTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = axisTree_->itemAt(pos);
    if (!item || !item->parent()) return; // only signal (child) rows get a context menu

    AxisEntry* owner = nullptr;
    PlottedSignal* sig = signalForItem(item, &owner);
    if (!sig || !owner) return;

    QMenu menu(axisTree_);
    QAction* removeAction = menu.addAction("Remove from axis");
    if (menu.exec(axisTree_->viewport()->mapToGlobal(pos)) == removeAction) {
        removeSignal(*owner, *sig);
    }
}

GraphView::AxisEntry* GraphView::axisForValueAxis(QValueAxis* axis) {
    for (auto& a : axes_) {
        if (a->axis == axis) return a.get();
    }
    return nullptr;
}

GraphView::AxisEntry* GraphView::axisNearPoint(const QPointF& chartPos, const QRectF& plotArea, bool /*leftSide*/) {
    // Every Y axis stacks on the left (see addAxis()), so this only ever
    // resolves against the left margin now.
    if (axes_.empty()) return nullptr;
    if (axes_.size() == 1) return axes_.front().get();

    // Multiple stacked axes share the margin outside the plot area; Qt
    // Charts doesn't expose each stacked axis's individual on-screen
    // geometry via public API, so approximate by dividing that margin
    // evenly, closest-to-plot-area slice first (the order axes were added).
    const double marginWidth = plotArea.left();
    const double distFromPlotArea = plotArea.left() - chartPos.x();
    const double slice = marginWidth / static_cast<double>(axes_.size());
    int index = slice > 0 ? static_cast<int>(distFromPlotArea / slice) : 0;
    index = std::clamp(index, 0, static_cast<int>(axes_.size()) - 1);
    return axes_[index].get();
}

void GraphView::zoomValueAxis(QValueAxis* axis, const QRectF& plotArea, const QPointF& chartPos, double factor, bool horizontal) {
    double value;
    if (horizontal) {
        const double fraction = (chartPos.x() - plotArea.left()) / plotArea.width();
        value = axis->min() + fraction * (axis->max() - axis->min());
    } else {
        const double fraction = (plotArea.bottom() - chartPos.y()) / plotArea.height();
        value = axis->min() + fraction * (axis->max() - axis->min());
    }

    const double newLo = value - (value - axis->min()) * factor;
    const double newHi = value + (axis->max() - value) * factor;
    axis->setRange(newLo, newHi);

    if (axis == timeAxis_) {
        timeZoomed_ = true;
        return;
    }
    if (AxisEntry* entry = axisForValueAxis(axis)) {
        // Manual zoom overrides auto-scale - otherwise the next incoming
        // sample would silently snap the range back via applyAxisRange().
        entry->autoScale = false;
        entry->min = newLo;
        entry->max = newHi;
        refreshAxisItemText(*entry);
    }
}

void GraphView::onWheelZoom(QPointF chartPos, double factor) {
    const QRectF plotArea = chart_->plotArea();

    if (chartPos.x() < plotArea.left()) {
        // Over the stacked Y axes' label margin.
        if (AxisEntry* entry = axisNearPoint(chartPos, plotArea, /*leftSide=*/true)) {
            zoomValueAxis(entry->axis, plotArea, chartPos, factor, /*horizontal=*/false);
        }
    } else {
        // Below the plot area (over the time axis labels) or anywhere
        // inside the plot body - zoom time, matching a trace tool's usual
        // "scroll to zoom the timeline" convention.
        zoomValueAxis(timeAxis_, plotArea, chartPos, factor, /*horizontal=*/true);
    }
}

void GraphView::resetZoom() {
    chart_->zoomReset();

    timeZoomed_ = false;
    timeAxis_->setRange(0, timeDataMax_ > 0 ? timeDataMax_ : 1.0);

    for (auto& a : axes_) {
        a->autoScale = true;
        applyAxisRange(*a);
        refreshAxisItemText(*a);
    }

    zoomSelectButton_->setChecked(false);
}

double GraphView::timeValueAt(double chartX) const {
    const QRectF plotArea = chart_->plotArea();
    const double fraction = (chartX - plotArea.left()) / plotArea.width();
    return timeAxis_->min() + fraction * (timeAxis_->max() - timeAxis_->min());
}

std::optional<QPointF> GraphView::nearestSampleOnSeries(QXYSeries* series, double timeValue) const {
    const QList<QPointF> pts = series->points();
    if (pts.isEmpty()) return std::nullopt;
    auto it = std::lower_bound(pts.begin(), pts.end(), timeValue,
        [](const QPointF& p, double t) { return p.x() < t; });
    if (it == pts.begin()) return *it;
    if (it == pts.end()) return *(it - 1);
    const QPointF& after = *it;
    const QPointF& before = *(it - 1);
    return (timeValue - before.x() <= after.x() - timeValue) ? before : after;
}

QXYSeries* GraphView::nearestVisibleSeries(const QPointF& chartPos, QPointF* outSampleValue) const {
    QXYSeries* best = nullptr;
    double bestDistSq = std::numeric_limits<double>::max();
    QPointF bestValue;

    const double timeValue = timeValueAt(chartPos.x());
    for (auto& axisPtr : axes_) {
        if (axisPtr->hidden) continue;
        for (auto& sig : axisPtr->plottedSignals) {
            if (!sig.series->isVisible()) continue;
            const auto sample = nearestSampleOnSeries(sig.series, timeValue);
            if (!sample) continue;
            const QPointF scenePoint = chart_->mapToPosition(*sample, sig.series);
            const double dx = scenePoint.x() - chartPos.x();
            const double dy = scenePoint.y() - chartPos.y();
            const double distSq = dx * dx + dy * dy;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                best = sig.series;
                bestValue = *sample;
            }
        }
    }
    if (best && outSampleValue) *outSampleValue = bestValue;
    return best;
}

void GraphView::clearCursorOverlay() {
    for (QGraphicsItem* item : cursorOverlayItems_) delete item;
    cursorOverlayItems_.clear();
}

void GraphView::updateHoverCursor(const QPointF& chartPos) {
    clearCursorOverlay();
    const QRectF plotArea = chart_->plotArea();
    if (chartPos.x() < plotArea.left() || chartPos.x() > plotArea.right()) return;

    // Parented to chart_ (not added to the scene directly) so these items
    // share chart_'s own local coordinate system - the same one chartPos
    // and mapToPosition() already use, via GraphChartView's
    // mapFromScene()-based conversion.
    auto* line = new QGraphicsLineItem(chartPos.x(), plotArea.top(), chartPos.x(), plotArea.bottom(), chart_);
    line->setPen(QPen(QColor(128, 128, 128), 1, Qt::DashLine));
    cursorOverlayItems_.append(line);

    const double timeValue = timeValueAt(chartPos.x());
    double labelY = plotArea.top() + 4;
    for (auto& axisPtr : axes_) {
        if (axisPtr->hidden) continue;
        for (auto& sig : axisPtr->plottedSignals) {
            if (!sig.series->isVisible()) continue;
            const auto sample = nearestSampleOnSeries(sig.series, timeValue);
            if (!sample) continue;
            auto* label = new QGraphicsSimpleTextItem(
                QString("%1: %2").arg(sig.qualifiedName).arg(sample->y(), 0, 'g', 6), chart_);
            label->setBrush(sig.color);
            label->setPos(chartPos.x() + 6, labelY);
            cursorOverlayItems_.append(label);
            labelY += 14;
        }
    }

    auto* timeLabel = new QGraphicsSimpleTextItem(QString("t = %1 s").arg(timeValue, 0, 'f', 3), chart_);
    timeLabel->setPos(chartPos.x() + 6, plotArea.bottom() - 16);
    cursorOverlayItems_.append(timeLabel);
}

void GraphView::updateDeltaCursor(const QPointF& chartPos) {
    if (!cursorSeries_) return;
    const double timeValue = timeValueAt(chartPos.x());
    const auto sample = nearestSampleOnSeries(cursorSeries_, timeValue);
    if (!sample) return;
    cursorBValue_ = *sample;
    hasCursorB_ = true;

    clearCursorOverlay();
    const QRectF plotArea = chart_->plotArea();

    auto addVLine = [&](double timeVal) {
        // y=0 is a dummy - only the resulting x is used, and the x/y axis
        // transforms are independent for a plain Cartesian value chart.
        const QPointF scenePt = chart_->mapToPosition(QPointF(timeVal, 0), cursorSeries_);
        auto* line = new QGraphicsLineItem(scenePt.x(), plotArea.top(), scenePt.x(), plotArea.bottom(), chart_);
        line->setPen(QPen(QColor(Qt::gray), 1, Qt::DashLine));
        cursorOverlayItems_.append(line);
    };
    addVLine(cursorAValue_.x());
    addVLine(cursorBValue_.x());

    const double dt = cursorBValue_.x() - cursorAValue_.x();
    const double dv = cursorBValue_.y() - cursorAValue_.y();
    auto* label = new QGraphicsSimpleTextItem(
        QString::fromUtf8("\xCE\x94t = %1 s   \xCE\x94value = %2").arg(dt, 0, 'f', 3).arg(dv, 0, 'g', 6), chart_);
    label->setPos(plotArea.left() + 8, plotArea.top() + 4);
    cursorOverlayItems_.append(label);
}

void GraphView::onChartMouseMove(QPointF chartPos, bool leftButtonDown) {
    if (!cursorToolButton_->isChecked()) return;
    if (cursorDragging_ && leftButtonDown) {
        updateDeltaCursor(chartPos);
    } else if (!leftButtonDown) {
        cursorDragging_ = false; // safety net if a release was ever missed
        updateHoverCursor(chartPos);
    }
}

void GraphView::onChartMousePress(QPointF chartPos) {
    if (!cursorToolButton_->isChecked()) return;
    QPointF sampleValue;
    QXYSeries* series = nearestVisibleSeries(chartPos, &sampleValue);
    if (!series) return;
    cursorSeries_ = series;
    cursorAValue_ = sampleValue;
    hasCursorA_ = true;
    hasCursorB_ = false;
    cursorDragging_ = true;
}

void GraphView::onChartMouseRelease(QPointF chartPos) {
    if (!cursorToolButton_->isChecked()) return;
    cursorDragging_ = false;
    if (hasCursorA_ && cursorSeries_) {
        updateDeltaCursor(chartPos); // finalize cursor B, stays visible until the next click
    }
}

} // namespace cantrip

#include "GraphView.moc"
