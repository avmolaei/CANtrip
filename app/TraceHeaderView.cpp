#include "TraceHeaderView.h"

#include <algorithm>

#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

namespace cantrip {

namespace {

constexpr int kArrowZonePx = 20;
constexpr int kArrowHalfWidthPx = 4;
constexpr int kArrowHalfHeightPx = 3;
constexpr int kMinEditorWidthPx = 140;
constexpr int kFilterDebounceMs = 150;
constexpr int kFilterMarkHeightPx = 2;

} // namespace

TraceHeaderView::TraceHeaderView(QWidget* parent) : QHeaderView(Qt::Horizontal, parent) {
    setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    applyBehavior();

    editor_ = new QLineEdit(viewport());
    editor_->setPlaceholderText("Filter...");
    editor_->setClearButtonEnabled(true);
    editor_->hide();
    editor_->installEventFilter(this);

    debounce_.setSingleShot(true);
    debounce_.setInterval(kFilterDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, [this]() {
        if (editorColumn_ >= 0) emit filterEdited(editorColumn_, editor_->text());
    });
    connect(editor_, &QLineEdit::textEdited, &debounce_, qOverload<>(&QTimer::start));
    connect(editor_, &QLineEdit::returnPressed, this, &TraceHeaderView::closeEditor);

    connect(this, &QHeaderView::sectionClicked, this, [this](int logicalIndex) { clickedSection_ = logicalIndex; });
    connect(this, &QHeaderView::sectionResized, this, &TraceHeaderView::positionEditor);
    connect(this, &QHeaderView::sectionMoved, this, &TraceHeaderView::positionEditor);
}

void TraceHeaderView::applyBehavior() {
    setSectionsClickable(true);
    setSectionsMovable(true);
    // Drawn by paintSection instead. Styles put this indicator wherever they
    // like, and the arrow has to sit where clicking it sorts.
    setSortIndicatorShown(false);
}

void TraceHeaderView::paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const {
    QHeaderView::paintSection(painter, rect, logicalIndex);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);

    const bool sorted = sortIndicatorSection() == logicalIndex;
    const bool ascending = sorted && sortIndicatorOrder() == Qt::AscendingOrder;
    QColor arrowColor = palette().color(QPalette::ButtonText);
    if (!sorted) arrowColor.setAlpha(70);
    painter->setBrush(arrowColor);

    const qreal cx = rect.right() - kArrowZonePx / 2.0;
    const qreal cy = rect.center().y() + 0.5;
    const qreal tip = ascending ? -kArrowHalfHeightPx : kArrowHalfHeightPx;
    painter->drawPolygon(QPolygonF({QPointF(cx - kArrowHalfWidthPx, cy - tip), QPointF(cx + kArrowHalfWidthPx, cy - tip), QPointF(cx, cy + tip)}));

    if (model() && !model()->headerData(logicalIndex, orientation(), Qt::UserRole).toString().isEmpty()) {
        painter->fillRect(QRect(rect.left(), rect.bottom() - kFilterMarkHeightPx + 1, rect.width(), kFilterMarkHeightPx),
                          palette().color(QPalette::Highlight));
    }
    painter->restore();
}

void TraceHeaderView::mousePressEvent(QMouseEvent* event) {
    pressPos_ = event->position().toPoint();
    QHeaderView::mousePressEvent(event);
}

void TraceHeaderView::mouseReleaseEvent(QMouseEvent* event) {
    const int section = sortIndicatorSection();
    const Qt::SortOrder order = sortIndicatorOrder();
    clickedSection_ = -1;
    QHeaderView::mouseReleaseEvent(event); // flips the sort indicator on a click
    setSortIndicator(section, order);

    if (clickedSection_ < 0) return;
    const int sectionRight = sectionViewportPosition(clickedSection_) + sectionSize(clickedSection_);
    if (pressPos_.x() >= sectionRight - kArrowZonePx) toggleSort(clickedSection_);
    else openEditor(clickedSection_);
}

void TraceHeaderView::toggleSort(int logicalIndex) {
    Qt::SortOrder order = Qt::AscendingOrder;
    if (sortIndicatorSection() == logicalIndex) {
        order = sortIndicatorOrder() == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else if (logicalIndex == 0) {
        order = Qt::DescendingOrder; // Time reads newest-first
    }
    setSortIndicator(logicalIndex, order);
    emit sortRequested(logicalIndex, order);
}

void TraceHeaderView::openEditor(int logicalIndex) {
    closeEditor();
    editorColumn_ = logicalIndex;
    editor_->setText(model() ? model()->headerData(logicalIndex, orientation(), Qt::UserRole).toString() : QString());
    positionEditor();
    editor_->show();
    editor_->setFocus();
    editor_->selectAll();
}

void TraceHeaderView::closeEditor() {
    if (editorColumn_ < 0) return;
    const int column = editorColumn_;
    editorColumn_ = -1; // first, because hiding the box drops focus back into here
    debounce_.stop();
    editor_->hide();
    emit filterEdited(column, editor_->text());
}

void TraceHeaderView::positionEditor() {
    if (editorColumn_ < 0) return;
    // A narrow column still gets a box wide enough to type in, kept on screen.
    const int width = std::max(sectionSize(editorColumn_) - kArrowZonePx, kMinEditorWidthPx);
    const int left = std::clamp(sectionViewportPosition(editorColumn_), 0, std::max(0, viewport()->width() - width));
    editor_->setGeometry(left, 0, width, viewport()->height());
}

bool TraceHeaderView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == editor_) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            editor_->clear();
            closeEditor();
            return true;
        }
        if (event->type() == QEvent::FocusOut) closeEditor();
    }
    return QHeaderView::eventFilter(watched, event);
}

} // namespace cantrip
