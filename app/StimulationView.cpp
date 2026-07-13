#include "StimulationView.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cantrip {

namespace {

QStringList transmitFlags(const TransmitMessage& m) {
    QStringList flags;
    flags << (m.extended ? "EXT" : "STD");
    if (m.fd) flags << "FD";
    if (m.brs) flags << "BRS";
    if (m.rtr) flags << "RTR";
    if (m.paused) flags << "PAUSED";
    return flags;
}

// Bottom-to-top rotated text, narrow enough to sit as a pane label to the
// left of a tree - Qt has no built-in vertical QLabel, and a stylesheet
// transform isn't reliably supported, so this just paints it directly.
class VerticalLabel : public QWidget {
public:
    VerticalLabel(const QString& text, QWidget* parent = nullptr) : QWidget(parent), text_(text) {
        setFixedWidth(20);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.translate(0, height());
        painter.rotate(-90);
        painter.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text_);
    }

private:
    QString text_;
};

// Wraps a tree widget with its vertical pane title to the left, matching
// the split-view layout PEAK's PCAN-View uses for its own Receive/Transmit
// panes.
QWidget* wrapWithLabel(const QString& title, QWidget* tree, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(new VerticalLabel(title, row));
    layout->addWidget(tree, /*stretch=*/1);
    return row;
}

} // namespace

StimulationView::StimulationView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Vertical, this);

    receivedTree_ = buildFrameTree(QString());
    transmitTree_ = buildFrameTree("QTreeWidget { background-color: #232323; }");
    transmitTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    transmitTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    splitter->addWidget(wrapWithLabel("Received Frames", receivedTree_, splitter));
    splitter->addWidget(wrapWithLabel("Transmitted Messages", transmitTree_, splitter));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter);

    connect(transmitTree_, &QTreeWidget::customContextMenuRequested, this, &StimulationView::onTransmitContextMenu);
    connect(transmitTree_, &QTreeWidget::itemDoubleClicked, this, &StimulationView::onTransmitDoubleClicked);
}

QTreeWidget* StimulationView::buildFrameTree(const QString& backgroundStyle) {
    auto* tree = new QTreeWidget(this);
    tree->setColumnCount(7);
    tree->setHeaderLabels({"Time", "ID", "Flags", "DLC", "Data", "Message/Signal", "Value"});
    tree->setColumnWidth(0, 90);
    tree->setColumnWidth(1, 80);
    tree->setColumnWidth(2, 100);
    tree->setColumnWidth(4, 220);
    tree->setColumnWidth(5, 160);
    if (!backgroundStyle.isEmpty()) tree->setStyleSheet(backgroundStyle);
    return tree;
}

QTreeWidgetItem* StimulationView::addReceivedRow(const QString& timestamp) {
    auto* item = new QTreeWidgetItem();
    item->setText(0, timestamp);
    receivedTree_->insertTopLevelItem(0, item);
    return item;
}

void StimulationView::clearReceived() {
    receivedTree_->clear();
}

void StimulationView::refreshTransmitList(const std::vector<TransmitMessage>& messages) {
    transmitTree_->clear();
    for (const TransmitMessage& m : messages) {
        auto* item = new QTreeWidgetItem(transmitTree_);
        item->setText(0, m.cycleTimeMs > 0 ? QString("%1 ms").arg(m.cycleTimeMs) : "One-shot");
        item->setText(1, QString("0x%1").arg(m.id, m.extended ? 8 : 3, 16, QChar('0')).toUpper());
        item->setText(2, transmitFlags(m).join(" "));
        item->setText(3, QString::number(m.dlc));
        item->setText(4, QString::fromLatin1(m.data.toHex(' ').toUpper()));
        item->setText(5, m.comment);
    }
}

std::vector<int> StimulationView::selectedTransmitIndices() const {
    std::vector<int> indices;
    for (int i = 0; i < transmitTree_->topLevelItemCount(); ++i) {
        if (transmitTree_->topLevelItem(i)->isSelected()) indices.push_back(i);
    }
    return indices;
}

void StimulationView::setPasteAvailable(bool available) {
    pasteAvailable_ = available;
}

void StimulationView::onTransmitContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = transmitTree_->itemAt(pos);
    const std::vector<int> selected = selectedTransmitIndices();

    QMenu menu(transmitTree_);
    QAction* editAction = item ? menu.addAction("Edit message") : nullptr;
    QAction* sendNowAction = !selected.empty() ? menu.addAction("Send Now") : nullptr;
    menu.addSeparator();
    QAction* copyAction = !selected.empty() ? menu.addAction("Copy") : nullptr;
    QAction* cutAction = !selected.empty() ? menu.addAction("Cut") : nullptr;
    QAction* pasteAction = pasteAvailable_ ? menu.addAction("Paste") : nullptr;
    QAction* deleteAction = !selected.empty() ? menu.addAction("Delete") : nullptr;
    menu.addSeparator();
    QAction* clearAllAction = transmitTree_->topLevelItemCount() > 0 ? menu.addAction("Clear All") : nullptr;

    QAction* chosen = menu.exec(transmitTree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == editAction) {
        emit editMessageRequested(transmitTree_->indexOfTopLevelItem(item));
    } else if (chosen == sendNowAction) {
        emit sendNowRequested(selected);
    } else if (chosen == copyAction) {
        emit copyRequested(selected);
    } else if (chosen == cutAction) {
        emit cutRequested(selected);
    } else if (chosen == pasteAction) {
        emit pasteRequested();
    } else if (chosen == deleteAction) {
        emit deleteMessagesRequested(selected);
    } else if (chosen == clearAllAction) {
        const auto choice = QMessageBox::question(this, "Clear All Messages",
            "Are you sure you want to delete all messages?", QMessageBox::Yes | QMessageBox::No);
        if (choice == QMessageBox::Yes) emit clearAllRequested();
    }
}

void StimulationView::onTransmitDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;
    emit editMessageRequested(transmitTree_->indexOfTopLevelItem(item));
}

} // namespace cantrip
