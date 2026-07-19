#include "BusLoadView.h"

#include <algorithm>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "GraphView.h"

namespace cantrip {

namespace {

QString formatBytes(uint64_t bytes) {
    if (bytes >= 1024 * 1024) return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
    if (bytes >= 1024) return QString::number(bytes / 1024.0, 'f', 2) + " KB";
    return QString::number(bytes) + " B";
}

// Sorts the "% of Bus" and "Frames" columns numerically via Qt::UserRole
// data rather than the default lexicographic text compare, which would
// otherwise put "10.0 %" before "9.0 %" - same fix as FrameTreeItem
// applies to the main Trace view's ID/DLC/Data columns.
class PerIdTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 1 || column == 2) {
            return data(column, Qt::UserRole).toDouble() < other.data(column, Qt::UserRole).toDouble();
        }
        return text(column) < other.text(column);
    }
};

QLabel* statRow(QVBoxLayout* layout, const QString& caption, QWidget* parent) {
    auto* row = new QHBoxLayout();
    auto* captionLabel = new QLabel(caption, parent);
    auto* valueLabel = new QLabel("-", parent);
    valueLabel->setAlignment(Qt::AlignRight);
    QFont bold = valueLabel->font();
    bold.setBold(true);
    valueLabel->setFont(bold);
    row->addWidget(captionLabel);
    row->addStretch(1);
    row->addWidget(valueLabel);
    layout->addLayout(row);
    return valueLabel;
}

} // namespace

BusLoadGauge::BusLoadGauge(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(60);
}

QSize BusLoadGauge::sizeHint() const {
    return QSize(70, 300);
}

void BusLoadGauge::setPercent(double percent) {
    percent_ = std::clamp(percent, 0.0, 100.0);
    update();
}

void BusLoadGauge::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QString label = QString::number(percent_, 'f', 0) + " %";
    const QFontMetrics fm(painter.font());
    const int labelHeight = fm.height() + 4;

    const QRect barRect(6, 4, width() - 12, height() - labelHeight - 8);
    painter.setPen(QPen(QColor(90, 90, 90)));
    painter.setBrush(QColor(30, 30, 30));
    painter.drawRect(barRect);

    const int fillHeight = static_cast<int>(barRect.height() * (percent_ / 100.0));
    const QRect fillRect(barRect.left(), barRect.bottom() - fillHeight, barRect.width(), fillHeight);
    // Green under half load, amber approaching capacity, red near 100% -
    // same "don't make someone read a number to know if it's bad" idea as
    // the rest of CANtrip's color-coding (TX tint, bus-error red rows).
    QColor fillColor = QColor(0x2e, 0x7d, 0x32);
    if (percent_ >= 90.0) fillColor = QColor(0xc6, 0x28, 0x28);
    else if (percent_ >= 70.0) fillColor = QColor(0xcc, 0x7a, 0x00);
    painter.fillRect(fillRect, fillColor);

    // Tick marks every 10%.
    painter.setPen(QPen(QColor(70, 70, 70)));
    for (int i = 1; i < 10; ++i) {
        const int y = barRect.top() + barRect.height() * i / 10;
        painter.drawLine(barRect.left(), y, barRect.right(), y);
    }

    painter.setPen(QPen(Qt::white));
    painter.drawText(QRect(0, height() - labelHeight, width(), labelHeight), Qt::AlignCenter, label);
}

BusLoadView::BusLoadView(BusLoadTracker* tracker, QWidget* parent)
    : QWidget(parent), tracker_(tracker) {
    auto* root = new QHBoxLayout(this);

    auto* gaugeGroup = new QGroupBox("Bus Load", this);
    gaugeGroup->setMaximumWidth(100);
    auto* gaugeLayout = new QVBoxLayout(gaugeGroup);
    gauge_ = new BusLoadGauge(gaugeGroup);
    gaugeLayout->addWidget(gauge_, /*stretch=*/1);

    auto* graphGroup = new QGroupBox("Bus Load History", this);
    auto* graphLayout = new QVBoxLayout(graphGroup);
    graph_ = new GraphView(tracker_->history(), graphGroup, /*simpleMode=*/true);
    graph_->configureSimpleModeAxis("Bus Load %", "BusLoad.Percent");
    graphLayout->addWidget(graph_);

    auto* statsGroup = new QGroupBox("Statistics", this);
    statsGroup->setMaximumWidth(280);
    auto* statsLayout = new QVBoxLayout(statsGroup);

    auto* resetButton = new QPushButton("Reset", statsGroup);
    statsLayout->addWidget(resetButton);
    statsLayout->addSpacing(8);

    maxLabel_ = statRow(statsLayout, "Maximum Bus Load:", statsGroup);
    minLabel_ = statRow(statsLayout, "Minimum Bus Load:", statsGroup);
    statsLayout->addSpacing(4);
    maxTimeLabel_ = statRow(statsLayout, "Time of max. Bus Load:", statsGroup);
    minTimeLabel_ = statRow(statsLayout, "Time of min. Bus Load:", statsGroup);
    statsLayout->addSpacing(4);
    meanLabel_ = statRow(statsLayout, "Bus Load Mean Value:", statsGroup);
    statsLayout->addSpacing(8);

    frameRateLabel_ = statRow(statsLayout, "Frame Rate (inst. / avg.):", statsGroup);
    totalFramesLabel_ = statRow(statsLayout, "Total Frames:", statsGroup);
    totalBytesLabel_ = statRow(statsLayout, "Total Bytes:", statsGroup);
    errorRateLabel_ = statRow(statsLayout, "Error Frame Rate:", statsGroup);
    peakBurstLabel_ = statRow(statsLayout, "Peak Burst Rate:", statsGroup);
    idlePercentLabel_ = statRow(statsLayout, "Idle Time:", statsGroup);
    statsLayout->addSpacing(8);

    statsLayout->addWidget(new QLabel("Bus Load by CAN ID:", statsGroup));
    perIdTree_ = new QTreeWidget(statsGroup);
    perIdTree_->setColumnCount(3);
    perIdTree_->setHeaderLabels({"ID", "% of Bus", "Frames"});
    perIdTree_->setColumnWidth(0, 80);
    perIdTree_->setColumnWidth(1, 80);
    perIdTree_->setColumnWidth(2, 60);
    perIdTree_->setSortingEnabled(true);
    perIdTree_->sortByColumn(1, Qt::DescendingOrder);
    perIdTree_->setRootIsDecorated(false);
    statsLayout->addWidget(perIdTree_, /*stretch=*/1);

    root->addWidget(gaugeGroup);
    root->addWidget(graphGroup, /*stretch=*/1);
    root->addWidget(statsGroup);

    connect(tracker_, &BusLoadTracker::statsUpdated, this, &BusLoadView::onStatsUpdated);
    connect(resetButton, &QPushButton::clicked, this, &BusLoadView::onResetClicked);
    // Clear Graph clears just the plotted history (and its backing
    // samples), same scope as everywhere else it appears - the wider
    // Reset button above is the one that also resets the running
    // statistics (max/min/mean/totals/per-ID).
    connect(graph_, &GraphView::clearRequested, this, [this]() {
        tracker_->history()->reset();
        graph_->clearData();
    });
}

void BusLoadView::onStatsUpdated(const BusLoadStats& stats) {
    gauge_->setPercent(stats.instantPercent);

    maxLabel_->setText(QString::number(stats.maxPercent, 'f', 1) + " %");
    minLabel_->setText(QString::number(stats.minPercent, 'f', 1) + " %");
    maxTimeLabel_->setText(stats.maxTimestamp);
    minTimeLabel_->setText(stats.minTimestamp);
    meanLabel_->setText(QString::number(stats.meanPercent, 'f', 1) + " %");

    frameRateLabel_->setText(QString("%1 / %2 fps")
        .arg(stats.instantFrameRate, 0, 'f', 0).arg(stats.averageFrameRate, 0, 'f', 0));
    totalFramesLabel_->setText(QString::number(stats.totalFrames));
    totalBytesLabel_->setText(formatBytes(stats.totalBytes));
    errorRateLabel_->setText(QString::number(stats.errorFrameRate, 'f', 1) + " /s ("
        + QString::number(stats.totalErrorFrames) + " total)");
    peakBurstLabel_->setText(QString::number(stats.peakBurstRate, 'f', 0) + " fps");
    idlePercentLabel_->setText(QString::number(stats.idlePercent, 'f', 1) + " %");

    perIdTree_->setSortingEnabled(false);
    perIdTree_->clear();
    for (const BusLoadPerIdStats& row : stats.perId) {
        auto* item = new PerIdTreeItem();
        const QString idText = QString("0x%1").arg(row.id, row.extended ? 8 : 3, 16, QChar('0')).toUpper();
        item->setText(0, idText);
        item->setText(1, QString::number(row.percentOfBus, 'f', 1) + " %");
        item->setText(2, QString::number(row.frameCount));
        item->setData(1, Qt::UserRole, row.percentOfBus);
        item->setData(2, Qt::UserRole, static_cast<double>(row.frameCount));
        perIdTree_->addTopLevelItem(item);
    }
    perIdTree_->setSortingEnabled(true);
}

void BusLoadView::onResetClicked() {
    tracker_->reset();
    graph_->clearData();
}

} // namespace cantrip
