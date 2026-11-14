// The Trace view's column header. Clicking a column's name opens a text box
// that filters that column as you type, and a small arrow at the right edge
// sorts. Dragging still reorders columns and the section edges still resize.
#pragma once

#include <QHeaderView>
#include <QPoint>
#include <QTimer>

class QLineEdit;

namespace cantrip {

class TraceHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit TraceHeaderView(QWidget* parent = nullptr);

    // QHeaderView::restoreState() brings back the settings saved with the
    // state, including ones from before this header existed - call this
    // after it to put this header's own behavior back.
    void applyBehavior();

signals:
    void filterEdited(int column, const QString& text);
    // Connect this, not sortIndicatorChanged: Qt flips the indicator itself
    // on every section click, which here has to be undone again.
    void sortRequested(int column, Qt::SortOrder order);

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void toggleSort(int logicalIndex);
    void openEditor(int logicalIndex);
    void closeEditor();
    void positionEditor();

    QLineEdit* editor_;
    int editorColumn_ = -1;
    // Keeps typing responsive: each keystroke rescans every row in the trace.
    QTimer debounce_;
    QPoint pressPos_;
    int clickedSection_ = -1;
};

} // namespace cantrip
