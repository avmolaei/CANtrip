// The Stimulation tab's content: received frames on top (same columns as
// CAN Trace), configured transmit messages on the bottom (tinted
// background), matching PEAK PCAN-View's split layout. MainWindow owns the
// actual decode/MessageSender logic - this widget is just the two trees,
// their toolbar-adjacent interactions, and the signals MainWindow reacts to.
#pragma once

#include <vector>

#include <QWidget>

#include "TransmitMessage.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace cantrip {

class StimulationView : public QWidget {
    Q_OBJECT
public:
    explicit StimulationView(QWidget* parent = nullptr);

    // Inserts a new, mostly-empty row (just the Time column set) at the top
    // of the received-frames tree and returns it, so the caller (MainWindow)
    // can fill in the rest via its own existing populateDecodedChildren() -
    // reused as-is, not duplicated, since it only ever operates on whatever
    // QTreeWidgetItem it's given.
    QTreeWidgetItem* addReceivedRow(const QString& timestamp);
    void clearReceived();
    // Exposed so MainWindow's Periodic-mode dedup (handleStimPeriodicFrame/
    // handleStimPeriodicErrorFrame) can update rows in place directly, the
    // same way it already does against frameTree_ for the main Trace view -
    // addReceivedRow() alone only covers the Waterfall-style "always insert
    // a new row" case.
    QTreeWidget* receivedTree() const { return receivedTree_; }

    // Full rebuild of the transmit list from MessageSender's current
    // messages - simplest correct approach given the list is always small
    // (a handful of configured test messages, not a growing log).
    void refreshTransmitList(const std::vector<TransmitMessage>& messages);
    std::vector<int> selectedTransmitIndices() const;

    // MainWindow owns the actual clipboard (a vector<TransmitMessage> - this
    // widget only ever sees indices/counts, never the real message data), so
    // it tells this view whether Paste should be offered in the context menu.
    void setPasteAvailable(bool available);

signals:
    void newMessageRequested();
    void editMessageRequested(int index);
    void sendNowRequested(const std::vector<int>& indices);
    void deleteMessagesRequested(const std::vector<int>& indices);
    void clearAllRequested();
    void copyRequested(const std::vector<int>& indices);
    void cutRequested(const std::vector<int>& indices);
    void pasteRequested();

private slots:
    void onTransmitContextMenu(const QPoint& pos);
    void onTransmitDoubleClicked(QTreeWidgetItem* item, int column);

private:
    QTreeWidget* buildFrameTree(const QString& backgroundStyle);

    QTreeWidget* receivedTree_;
    QTreeWidget* transmitTree_;

    bool pasteAvailable_ = false;
};

} // namespace cantrip
