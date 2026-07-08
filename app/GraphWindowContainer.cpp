#include "GraphWindowContainer.h"

#include <cmath>

#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStringList>
#include <QVBoxLayout>

namespace cantrip {

GraphWindowContainer::GraphWindowContainer(SignalHistoryStore* history, QWidget* parent)
    : QWidget(parent), history_(history) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout();
    addWindowButton_ = new QPushButton("Add Graph Window", this);
    removeWindowButton_ = new QPushButton("Remove Graph Window", this);
    layoutToggleButton_ = new QPushButton(this);
    exportAllButton_ = new QPushButton("Export All...", this);
    exportAllButton_->setToolTip("Export every graph window to its own PNG/SVG/PDF file in a chosen folder");
    toolbar->addWidget(addWindowButton_);
    toolbar->addWidget(removeWindowButton_);
    toolbar->addWidget(layoutToggleButton_);
    toolbar->addWidget(exportAllButton_);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    gridHost_ = new QWidget();
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidget(gridHost_);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scrollArea_, /*stretch=*/1);

    connect(addWindowButton_, &QPushButton::clicked, this, &GraphWindowContainer::addGraphWindow);
    connect(removeWindowButton_, &QPushButton::clicked, this, &GraphWindowContainer::removeGraphWindow);
    connect(layoutToggleButton_, &QPushButton::clicked, this, &GraphWindowContainer::toggleLayout);
    connect(exportAllButton_, &QPushButton::clicked, this, &GraphWindowContainer::exportAllGraphs);

    addGraphWindow(); // always start with exactly one window
}

void GraphWindowContainer::reset() {
    for (GraphView* w : windows_) w->reset();
}

void GraphWindowContainer::clearAllData() {
    for (GraphView* w : windows_) w->clearData();
}

std::vector<std::vector<GraphView::AxisLayout>> GraphWindowContainer::exportLayout() const {
    std::vector<std::vector<GraphView::AxisLayout>> result;
    result.reserve(windows_.size());
    for (GraphView* w : windows_) result.push_back(w->exportLayout());
    return result;
}

void GraphWindowContainer::importLayout(const std::vector<std::vector<GraphView::AxisLayout>>& windows) {
    while (windows_.size() < windows.size() && windows_.size() < static_cast<size_t>(kMaxGraphWindows)) {
        addGraphWindow();
    }
    for (size_t i = 0; i < windows.size() && i < windows_.size(); ++i) {
        windows_[i]->importLayout(windows[i]);
    }
}

void GraphWindowContainer::addGraphWindow() {
    if (windows_.size() >= static_cast<size_t>(kMaxGraphWindows)) return;

    auto* view = new GraphView(history_, gridHost_);
    connect(view, &GraphView::clearRequested, this, &GraphWindowContainer::clearRequested);
    windows_.push_back(view);

    rebuildLayout();
    updateToolbarState();
}

void GraphWindowContainer::removeGraphWindow() {
    if (windows_.size() <= 1) return;

    GraphView* last = windows_.back();
    windows_.pop_back();
    delete last;

    rebuildLayout();
    updateToolbarState();
}

void GraphWindowContainer::toggleLayout() {
    layoutMode_ = (layoutMode_ == LayoutMode::Stacked) ? LayoutMode::Grid : LayoutMode::Stacked;
    rebuildLayout();
}

void GraphWindowContainer::rebuildLayout() {
    // Simplest correct approach: tear down and rebuild the grid from scratch
    // rather than incrementally reflow it - window counts here are always
    // small (<= kMaxGraphWindows), so this stays cheap, and it avoids having
    // to hunt down/clear stale row/column stretch factors left over from a
    // previous window count or layout mode.
    delete gridHost_->layout();
    auto* grid = new QGridLayout(gridHost_);

    const int n = static_cast<int>(windows_.size());
    const int columns = (layoutMode_ == LayoutMode::Stacked)
        ? 1
        : std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));

    for (int i = 0; i < n; ++i) {
        const int row = i / columns;
        const int col = i % columns;
        grid->addWidget(windows_[static_cast<size_t>(i)], row, col);
        grid->setRowStretch(row, 1);
        grid->setColumnStretch(col, 1);

        // Stacked mode: give each pane a real minimum height and let the
        // scroll area handle overflow, rather than squeezing every pane
        // down to fit whatever's currently visible. Grid mode resets this
        // so its panes keep dividing the viewport evenly, as before.
        windows_[static_cast<size_t>(i)]->setMinimumHeight(
            layoutMode_ == LayoutMode::Stacked ? kStackedPaneMinHeight : 0);
    }

    layoutToggleButton_->setText(layoutMode_ == LayoutMode::Stacked ? "Layout: Stacked" : "Layout: Grid");
}

void GraphWindowContainer::updateToolbarState() {
    addWindowButton_->setEnabled(windows_.size() < static_cast<size_t>(kMaxGraphWindows));
    removeWindowButton_->setEnabled(windows_.size() > 1);
}

void GraphWindowContainer::exportAllGraphs() {
    // Mirrors MainWindow's Logging tab default (Documents/CANtrip/captures) -
    // same convention, sibling folder.
    const QString graphsDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/CANtrip/graphs";
    QDir().mkpath(graphsDir);

    const QString dir = QFileDialog::getExistingDirectory(this, "Export All Graphs - Choose Folder", graphsDir);
    if (dir.isEmpty()) return;

    bool ok = false;
    const QStringList formats = {"PNG", "SVG", "PDF"};
    const QString format = QInputDialog::getItem(this, "Export All Graphs", "Format:", formats, 0, false, &ok);
    if (!ok) return;
    const QString extension = format.toLower();

    int failures = 0;
    for (size_t i = 0; i < windows_.size(); ++i) {
        const QString path = QString("%1/graph_%2.%3").arg(dir).arg(i + 1).arg(extension);
        if (!windows_[i]->exportToFile(path)) ++failures;
    }

    if (failures > 0) {
        QMessageBox::warning(this, "Export All Graphs",
            QString("%1 of %2 graph(s) could not be exported.").arg(failures).arg(windows_.size()));
    }
}

} // namespace cantrip
