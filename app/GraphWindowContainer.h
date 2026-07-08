// Hosts one or more independent GraphView instances (up to kMaxGraphWindows),
// arranged either stacked (one column) or in an auto-sized grid. Every window
// is fully independent - own signal list, own axes - all pointed at the same
// shared SignalHistoryStore, matching how a single GraphView already worked;
// this is a thin layout/lifecycle wrapper around N of them, not a rework of
// GraphView itself.
#pragma once

#include <vector>

#include <QWidget>

#include "GraphView.h"
#include "SignalHistory.h"

class QGridLayout;
class QPushButton;
class QScrollArea;
class QWidget;

namespace cantrip {

class GraphWindowContainer : public QWidget {
    Q_OBJECT
public:
    explicit GraphWindowContainer(SignalHistoryStore* history, QWidget* parent = nullptr);

    // Tears down every window's axis structure entirely - see GraphView::reset().
    void reset();
    // Clears plotted data in every window but keeps axis/signal structure -
    // see GraphView::clearData().
    void clearAllData();

    // One entry per window, in window order.
    std::vector<std::vector<GraphView::AxisLayout>> exportLayout() const;
    // Adds windows as needed (up to kMaxGraphWindows) to match the saved
    // window count, then imports each window's layout additively - call
    // reset() first for a clean slate, same convention as GraphView::importLayout().
    void importLayout(const std::vector<std::vector<GraphView::AxisLayout>>& windows);

signals:
    // Re-emitted whenever any child window's "Clear Graph" is clicked -
    // every window shares one SignalHistoryStore, so a clear has to be
    // handled globally by whoever owns that store (MainWindow), not just in
    // the one window that happened to request it.
    void clearRequested();

private:
    enum class LayoutMode { Stacked, Grid };

    void addGraphWindow();
    void removeGraphWindow();
    void toggleLayout();
    void rebuildLayout();
    void updateToolbarState();
    void exportAllGraphs();

    static constexpr int kMaxGraphWindows = 6;
    // Stacked mode gives each pane a healthy fixed minimum height rather
    // than dividing the container's visible height evenly across however
    // many windows exist - the scroll area below lets you reach panes that
    // don't fit rather than shrinking every pane to fit the viewport. Grid
    // mode doesn't use this (its panes intentionally divide the viewport).
    static constexpr int kStackedPaneMinHeight = 480;

    SignalHistoryStore* history_;
    std::vector<GraphView*> windows_;
    LayoutMode layoutMode_ = LayoutMode::Stacked;

    QPushButton* addWindowButton_;
    QPushButton* removeWindowButton_;
    QPushButton* layoutToggleButton_;
    QPushButton* exportAllButton_;
    QScrollArea* scrollArea_;
    QWidget* gridHost_;
};

} // namespace cantrip
