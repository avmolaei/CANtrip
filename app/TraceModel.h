// The Trace view's data: raw frames in a capped buffer, formatted into text
// only for rows actually on screen. A QTreeWidget allocates an item per row
// and per signal, so its cost grew with the length of the capture (a refresh
// took 186 ms at 50,000 rows, measured at 4,000 frames/s) - this stays flat
// however long a capture runs. Logging and the Graph view are fed before this
// point (see MainWindow::onFrameReceived), so nothing here can lose data.
//
// Filtering and sorting live here rather than in a QSortFilterProxyModel: the
// proxy added about 370 ns per visible row to every layout, enough to miss a
// 30 Hz refresh at the row cap on its own.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QAbstractItemModel>
#include <QBrush>
#include <QHash>
#include <QString>

#include "DbcDecoder.h"
#include "TsharkCapture.h"

namespace cantrip {

class TraceModel : public QAbstractItemModel {
    Q_OBJECT
public:
    // Bus is last so that column numbers saved in older runes keep their
    // meaning. The Trace view hides it while only one bus is in use.
    enum Column { Time, Id, Flags, Dlc, Data, Message, Value, Bus, kColumnCount };

    // Waterfall keeps this many newest rows, about 25 s of a full 500 kbit/s
    // bus. Periodic never gets near it, it has one row per ID.
    static constexpr int kMaxRows = 100000;

    // The Stimulation tab's received pane formats rows the same way.
    static QString idText(const DecodedCanFrame& frame);
    static QString flagsText(const DecodedCanFrame& frame);
    static QBrush txBrush();
    // One Periodic row per this key: a CAN ID, kept apart by standard/extended
    // and by bus, since the same ID on two buses is two different messages.
    static uint64_t frameKey(const DecodedCanFrame& frame);

    explicit TraceModel(const DbcDecoder* decoder, QObject* parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    // Rows passing the filters. See totalRows() for everything held.
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    // Qt::UserRole gives a column's filter text, Qt::DisplayRole shows it in
    // the header alongside the column's name.
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    int totalRows() const { return static_cast<int>(rows_.size()); }
    void clear();
    // Call after loading a DBC: names and child rows all depend on it.
    void decoderChanged();

    // Waterfall: one row per frame, oldest rows dropped past kMaxRows.
    void appendFrames(const std::vector<DecodedCanFrame>& frames);
    // Periodic: one row per CAN ID (or per error type, which has no ID),
    // updated in place. nowMs is the arrival time the period is measured from.
    // Takes effect on the next endPeriodicBatch(), so that any number of rows
    // can change in one refresh for the price of one re-sort.
    void upsertPeriodic(const DecodedCanFrame& frame, qint64 nowMs);
    void endPeriodicBatch();
    // Grays out Periodic rows with nothing new for longer than timeoutMs.
    void markStaleRows(qint64 nowMs, qint64 timeoutMs);

    // Empty text clears that column's filter. Filters combine, a row has to
    // match all of them. Only what's on screen changes, every row stays held.
    void setColumnFilter(int column, const QString& text);
    bool hasFilter() const;

private:
    struct Row {
        DecodedCanFrame frame;
        double timeSec = 0.0;
        int signalCount = 0;
        bool visible = false;
        bool periodic = false;
        qint64 lastArrivalMs = 0;
        qint64 periodMs = -1;
        bool stale = false;
        // Filled when a row is expanded, so unexpanded rows cost no decode.
        mutable std::unique_ptr<std::vector<DbcDecoder::DecodedSignal>> signalCache;
    };

    Row makeRow(const DecodedCanFrame& frame) const;
    const Row& rowAt(quintptr uid) const { return rows_[uid - firstUid_]; }
    Row& rowAt(quintptr uid) { return rows_[uid - firstUid_]; }
    const std::vector<DbcDecoder::DecodedSignal>& signalsOf(const Row& row) const;
    QString cellText(const Row& row, int column) const;
    static QString signalValueText(const DbcDecoder::DecodedSignal& signal);

    bool matchesFilters(const Row& row) const;
    bool cellMatches(const Row& row, int column, const QString& needle) const;
    bool sortKeyLess(const Row& a, const Row& b) const;
    // The on-screen order: the chosen sort, then age, so it's a strict total
    // order and rowOf() can binary search it.
    bool before(quintptr a, quintptr b) const;
    int rowOf(quintptr uid) const;
    void insertVisible(std::vector<quintptr> uids);
    void removeVisible(const std::vector<quintptr>& uids);
    void replaceVisible(std::deque<quintptr> next);
    std::deque<quintptr> matchingRowsInOrder() const;
    void emitAllRowsChanged(const QList<int>& roles);

    const DbcDecoder* decoder_;
    // Every held row, oldest first. A row's uid is permanent while row numbers
    // shift as the oldest rows drop: uid = firstUid_ + position in rows_.
    std::deque<Row> rows_;
    quintptr firstUid_ = 0;
    // The rows on screen, in on-screen order. A view row number is a position
    // in here. Child indexes store their parent's uid + 1, top-level ones 0.
    std::deque<quintptr> visible_;

    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
    QString filters_[kColumnCount];

    std::unordered_map<uint64_t, quintptr> periodicRows_;
    QHash<QString, quintptr> periodicErrorRows_;
    std::vector<quintptr> periodicDirty_;
};

} // namespace cantrip
