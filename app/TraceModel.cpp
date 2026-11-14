#include "TraceModel.h"

#include <algorithm>

#include <QColor>
#include <QStringList>

namespace cantrip {

QBrush TraceModel::txBrush() {
    // Light blue, distinct from the plain default text color and from the
    // red used for bus errors - MessageSender's own sends flow through the
    // exact same onFrameReceived pipeline as real captured traffic (see its
    // class comment), so without this a frame CANtrip itself put on the bus
    // is visually indistinguishable from one actually received off the wire.
    return QBrush(QColor(100, 181, 246));
}

QString TraceModel::flagsText(const DecodedCanFrame& f) {
    QStringList flags;
    flags << (f.extended ? "EXT" : "STD");
    if (f.fd) flags << "FD";
    if (f.brs) flags << "BRS";
    if (f.esi) flags << "ESI";
    if (f.rtr) flags << "RTR";
    if (f.direction == FrameDirection::Tx) flags << "TX";
    return flags.join(" ");
}

QString TraceModel::idText(const DecodedCanFrame& f) {
    return QString("0x%1").arg(f.id, f.extended ? 8 : 3, 16, QChar('0')).toUpper();
}

uint64_t TraceModel::frameKey(const DecodedCanFrame& f) {
    return (static_cast<uint64_t>(f.extended ? 1 : 0) << 32) | f.id;
}

TraceModel::TraceModel(const DbcDecoder* decoder, QObject* parent)
    : QAbstractItemModel(parent), decoder_(decoder) {}

QModelIndex TraceModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= kColumnCount) return QModelIndex();
    if (!parent.isValid()) {
        return row < static_cast<int>(visible_.size()) ? createIndex(row, column, quintptr(0)) : QModelIndex();
    }
    if (parent.internalId() != 0 || parent.row() >= static_cast<int>(visible_.size())) return QModelIndex();
    return createIndex(row, column, visible_[parent.row()] + 1);
}

QModelIndex TraceModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || child.internalId() == 0) return QModelIndex();
    const int row = rowOf(child.internalId() - 1);
    return row < 0 ? QModelIndex() : createIndex(row, 0, quintptr(0));
}

int TraceModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) return static_cast<int>(visible_.size());
    if (parent.internalId() != 0 || parent.column() != 0 || parent.row() >= static_cast<int>(visible_.size())) return 0;
    return rowAt(visible_[parent.row()]).signalCount;
}

int TraceModel::columnCount(const QModelIndex&) const {
    return kColumnCount;
}

QVariant TraceModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();

    const bool isChild = index.internalId() != 0;
    quintptr uid;
    if (isChild) {
        uid = index.internalId() - 1;
        if (uid < firstUid_ || uid - firstUid_ >= rows_.size()) return QVariant();
    } else {
        if (index.row() >= static_cast<int>(visible_.size())) return QVariant();
        uid = visible_[index.row()];
    }
    const Row& row = rowAt(uid);

    if (role == Qt::ForegroundRole) {
        if (row.stale) return QBrush(QColor(Qt::gray));
        if (isChild) return QVariant();
        if (row.frame.error) return QBrush(QColor(Qt::red));
        if (row.frame.direction == FrameDirection::Tx) return txBrush();
        return QVariant();
    }
    if (role != Qt::DisplayRole) return QVariant();

    if (!isChild) return cellText(row, index.column());

    const auto& decoded = signalsOf(row);
    if (index.row() >= static_cast<int>(decoded.size())) return QVariant();
    if (index.column() == Message) return decoded[index.row()].name;
    if (index.column() == Value) return signalValueText(decoded[index.row()]);
    return QVariant();
}

QVariant TraceModel::headerData(int section, Qt::Orientation orientation, int role) const {
    static const char* const kNames[kColumnCount] = {"Time", "ID", "Flags", "DLC", "Data", "Message/Signal", "Value"};
    if (orientation != Qt::Horizontal || section < 0 || section >= kColumnCount) return QVariant();
    if (role == Qt::UserRole) return filters_[section];
    if (role != Qt::DisplayRole) return QVariant();
    const QString name(kNames[section]);
    return filters_[section].isEmpty() ? name : name + ": " + filters_[section];
}

void TraceModel::sort(int column, Qt::SortOrder order) {
    if (column == sortColumn_ && order == sortOrder_) return;
    sortColumn_ = column;
    sortOrder_ = order;
    replaceVisible(matchingRowsInOrder());
}

void TraceModel::clear() {
    beginResetModel();
    rows_.clear();
    visible_.clear();
    firstUid_ = 0;
    periodicRows_.clear();
    periodicErrorRows_.clear();
    periodicDirty_.clear();
    endResetModel();
}

void TraceModel::decoderChanged() {
    beginResetModel();
    for (Row& row : rows_) {
        row.signalCache.reset();
        row.signalCount = row.frame.error ? 0 : decoder_->signalCount(row.frame);
    }
    endResetModel();
}

TraceModel::Row TraceModel::makeRow(const DecodedCanFrame& frame) const {
    Row row;
    row.frame = frame;
    row.timeSec = frame.timestamp.toDouble();
    row.signalCount = frame.error ? 0 : decoder_->signalCount(frame);
    return row;
}

void TraceModel::appendFrames(const std::vector<DecodedCanFrame>& frames) {
    if (frames.empty()) return;

    const int incoming = static_cast<int>(std::min<size_t>(frames.size(), kMaxRows));
    const int overflow = static_cast<int>(rows_.size()) + incoming - kMaxRows;
    if (overflow > 0) {
        std::vector<quintptr> dropped;
        for (int i = 0; i < overflow; ++i) {
            if (rows_[i].visible) dropped.push_back(firstUid_ + i);
        }
        removeVisible(dropped);
        rows_.erase(rows_.begin(), rows_.begin() + overflow);
        firstUid_ += overflow;
    }

    std::vector<quintptr> added;
    added.reserve(incoming);
    for (auto it = frames.end() - incoming; it != frames.end(); ++it) {
        rows_.push_back(makeRow(*it));
        if (matchesFilters(rows_.back())) added.push_back(firstUid_ + rows_.size() - 1);
    }
    insertVisible(std::move(added));
}

void TraceModel::upsertPeriodic(const DecodedCanFrame& frame, qint64 nowMs) {
    quintptr uid;
    bool known;
    if (frame.error) {
        auto it = periodicErrorRows_.constFind(frame.errorDescription);
        known = it != periodicErrorRows_.constEnd();
        uid = known ? it.value() : firstUid_ + rows_.size();
    } else {
        auto it = periodicRows_.find(frameKey(frame));
        known = it != periodicRows_.end();
        uid = known ? it->second : firstUid_ + rows_.size();
    }

    if (!known) {
        Row row = makeRow(frame);
        row.periodic = true;
        row.lastArrivalMs = nowMs;
        rows_.push_back(std::move(row));
        if (frame.error) periodicErrorRows_.insert(frame.errorDescription, uid);
        else periodicRows_[frameKey(frame)] = uid;
        periodicDirty_.push_back(uid);
        return;
    }

    Row& row = rowAt(uid);
    row.periodMs = nowMs - row.lastArrivalMs;
    row.lastArrivalMs = nowMs;
    row.stale = false;
    row.frame = frame;
    row.timeSec = frame.timestamp.toDouble();
    row.signalCache.reset();
    periodicDirty_.push_back(uid);
}

void TraceModel::endPeriodicBatch() {
    if (periodicDirty_.empty()) return;

    // A row can change more than once in a batch, and must only appear once.
    std::sort(periodicDirty_.begin(), periodicDirty_.end());
    periodicDirty_.erase(std::unique(periodicDirty_.begin(), periodicDirty_.end()), periodicDirty_.end());

    // New data can start or stop matching a filter.
    std::vector<quintptr> show, hide;
    for (quintptr uid : periodicDirty_) {
        const Row& row = rowAt(uid);
        const bool matches = matchesFilters(row);
        if (matches && !row.visible) show.push_back(uid);
        else if (!matches && row.visible) hide.push_back(uid);
    }
    periodicDirty_.clear();

    if (!hide.empty()) {
        // Sorting by a value that just changed leaves the row positions
        // removeVisible() searches for out of date, so go through a full
        // rebuild whenever a row has to leave.
        replaceVisible(matchingRowsInOrder());
    } else if (!std::is_sorted(visible_.begin(), visible_.end(), [this](quintptr a, quintptr b) { return before(a, b); })) {
        replaceVisible(matchingRowsInOrder());
    } else {
        insertVisible(std::move(show));
    }

    emitAllRowsChanged({});
}

void TraceModel::markStaleRows(qint64 nowMs, qint64 timeoutMs) {
    if (periodicRows_.empty() && periodicErrorRows_.empty()) return;

    bool changed = false;
    for (Row& row : rows_) {
        if (row.stale || (nowMs - row.lastArrivalMs) <= timeoutMs) continue;
        row.stale = true;
        changed = true;
    }
    if (changed) emitAllRowsChanged({Qt::ForegroundRole});
}

void TraceModel::emitAllRowsChanged(const QList<int>& roles) {
    const int rows = static_cast<int>(visible_.size());
    if (rows == 0) return;
    emit dataChanged(index(0, 0), index(rows - 1, kColumnCount - 1), roles);

    // A view only ever fetches a row's signals once it's expanded, so a
    // filled cache marks the only rows whose children are on screen.
    for (int i = 0; i < rows; ++i) {
        const Row& row = rowAt(visible_[i]);
        if (row.signalCount == 0) continue;
        if (roles.isEmpty() || row.signalCache) {
            const QModelIndex parentIndex = index(i, 0);
            emit dataChanged(index(0, 0, parentIndex), index(row.signalCount - 1, kColumnCount - 1, parentIndex), roles);
        }
    }
}

void TraceModel::setColumnFilter(int column, const QString& text) {
    if (column < 0 || column >= kColumnCount) return;
    const QString trimmed = text.trimmed();
    if (filters_[column] == trimmed) return;
    filters_[column] = trimmed;
    replaceVisible(matchingRowsInOrder());
    emit headerDataChanged(Qt::Horizontal, column, column);
}

bool TraceModel::hasFilter() const {
    return std::any_of(std::begin(filters_), std::end(filters_), [](const QString& f) { return !f.isEmpty(); });
}

bool TraceModel::matchesFilters(const Row& row) const {
    for (int column = 0; column < kColumnCount; ++column) {
        if (!filters_[column].isEmpty() && !cellMatches(row, column, filters_[column])) return false;
    }
    return true;
}

bool TraceModel::cellMatches(const Row& row, int column, const QString& needle) const {
    if (column == Value) {
        // Not through signalsOf(): caching a decode for every row filtered
        // over would put back the per-row cost this model exists to avoid.
        if (row.frame.error) return false;
        for (const DbcDecoder::DecodedSignal& signal : decoder_->decodeSignals(row.frame)) {
            if (signalValueText(signal).contains(needle, Qt::CaseInsensitive)) return true;
        }
        return false;
    }

    QString text = cellText(row, column);
    if (text.contains(needle, Qt::CaseInsensitive)) return true;

    if (column == Data) {
        // "A0A1" and "A0 A1" should find the same bytes.
        QString compactNeedle = needle;
        compactNeedle.remove(' ');
        return text.remove(' ').contains(compactNeedle, Qt::CaseInsensitive);
    }
    if (column == Message && !row.frame.error) {
        // Typing a signal's name keeps its message on screen.
        if (const QStringList* names = decoder_->signalNames(row.frame)) {
            for (const QString& name : *names) {
                if (name.contains(needle, Qt::CaseInsensitive)) return true;
            }
        }
    }
    return false;
}

bool TraceModel::sortKeyLess(const Row& a, const Row& b) const {
    switch (sortColumn_) {
        case Time:
            return a.periodic ? a.periodMs < b.periodMs : a.timeSec < b.timeSec;
        case Id:
            return frameKey(a.frame) < frameKey(b.frame);
        case Dlc:
            return a.frame.dlc < b.frame.dlc;
        case Data:
            return a.frame.data < b.frame.data;
        default:
            return cellText(a, sortColumn_) < cellText(b, sortColumn_);
    }
}

bool TraceModel::before(quintptr a, quintptr b) const {
    const bool ascending = sortOrder_ == Qt::AscendingOrder;
    if (sortColumn_ >= 0) {
        const Row& x = rowAt(a);
        const Row& y = rowAt(b);
        if (sortKeyLess(x, y)) return ascending;
        if (sortKeyLess(y, x)) return !ascending;
    }
    return ascending ? a < b : a > b;
}

int TraceModel::rowOf(quintptr uid) const {
    if (uid < firstUid_ || uid - firstUid_ >= rows_.size() || !rowAt(uid).visible) return -1;
    const auto it = std::lower_bound(visible_.begin(), visible_.end(), uid, [this](quintptr a, quintptr b) { return before(a, b); });
    return it != visible_.end() && *it == uid ? static_cast<int>(it - visible_.begin()) : -1;
}

void TraceModel::insertVisible(std::vector<quintptr> uids) {
    const auto order = [this](quintptr a, quintptr b) { return before(a, b); };
    std::sort(uids.begin(), uids.end(), order);

    // From the end, so positions found for earlier rows stay correct. Rows
    // landing in the same gap go in as one insert: with the default sort
    // that's the whole batch at the top, for one view update.
    size_t end = uids.size();
    while (end > 0) {
        const auto gap = std::lower_bound(visible_.begin(), visible_.end(), uids[end - 1], order);
        const int position = static_cast<int>(gap - visible_.begin());
        size_t start = end - 1;
        while (start > 0 && (position == 0 || before(visible_[position - 1], uids[start - 1]))) --start;

        beginInsertRows(QModelIndex(), position, position + static_cast<int>(end - start) - 1);
        visible_.insert(gap, uids.begin() + start, uids.begin() + end);
        for (size_t i = start; i < end; ++i) rowAt(uids[i]).visible = true;
        endInsertRows();
        end = start;
    }
}

void TraceModel::removeVisible(const std::vector<quintptr>& uids) {
    std::vector<int> positions;
    positions.reserve(uids.size());
    for (quintptr uid : uids) {
        const int row = rowOf(uid);
        if (row >= 0) positions.push_back(row);
    }
    std::sort(positions.begin(), positions.end());

    // From the end, in runs, for the same reasons as insertVisible().
    size_t end = positions.size();
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && positions[start - 1] == positions[start] - 1) --start;
        const int first = positions[start];
        const int last = positions[end - 1];

        beginRemoveRows(QModelIndex(), first, last);
        for (int row = first; row <= last; ++row) rowAt(visible_[row]).visible = false;
        visible_.erase(visible_.begin() + first, visible_.begin() + last + 1);
        endRemoveRows();
        end = start;
    }
}

std::deque<quintptr> TraceModel::matchingRowsInOrder() const {
    std::deque<quintptr> next;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (matchesFilters(rows_[i])) next.push_back(firstUid_ + i);
    }
    // Held rows are in arrival order, already the right order for the default
    // sort once reversed, so that case skips a sort of up to kMaxRows rows.
    if (sortOrder_ == Qt::DescendingOrder) std::reverse(next.begin(), next.end());
    const auto order = [this](quintptr a, quintptr b) { return before(a, b); };
    if (!std::is_sorted(next.begin(), next.end(), order)) std::sort(next.begin(), next.end(), order);
    return next;
}

void TraceModel::replaceVisible(std::deque<quintptr> next) {
    emit layoutAboutToBeChanged();

    // Keeps expanded rows expanded through a filter or sort change. Views
    // track those with persistent indexes, by row number, so renumber them.
    const QModelIndexList from = persistentIndexList();
    std::vector<quintptr> uids;
    uids.reserve(from.size());
    for (const QModelIndex& index : from) {
        uids.push_back(index.internalId() != 0 ? index.internalId() - 1 : visible_[index.row()]);
    }

    for (quintptr uid : visible_) rowAt(uid).visible = false;
    visible_ = std::move(next);
    for (quintptr uid : visible_) rowAt(uid).visible = true;

    QModelIndexList to;
    to.reserve(from.size());
    for (qsizetype i = 0; i < from.size(); ++i) {
        const int row = rowOf(uids[i]);
        if (row < 0) to.append(QModelIndex());
        else if (from[i].internalId() != 0) to.append(from[i]);
        else to.append(index(row, from[i].column()));
    }
    changePersistentIndexList(from, to);

    emit layoutChanged();
}

const std::vector<DbcDecoder::DecodedSignal>& TraceModel::signalsOf(const Row& row) const {
    if (!row.signalCache) {
        row.signalCache = std::make_unique<std::vector<DbcDecoder::DecodedSignal>>(decoder_->decodeSignals(row.frame));
    }
    return *row.signalCache;
}

QString TraceModel::signalValueText(const DbcDecoder::DecodedSignal& signal) {
    return QString::number(signal.physicalValue) + " " + signal.unit;
}

QString TraceModel::cellText(const Row& row, int column) const {
    const DecodedCanFrame& f = row.frame;
    if (column == Time) {
        if (!row.periodic) return f.timestamp;
        return row.periodMs < 0 ? QStringLiteral("--") : QString::number(row.periodMs / 1000.0, 'f', 3) + " s";
    }
    if (f.error) return column == Message ? "BUS ERROR: " + f.errorDescription : QString();

    switch (column) {
        case Id: return idText(f);
        case Flags: return flagsText(f);
        case Dlc: return QString::number(f.dlc);
        case Data: return QString::fromLatin1(f.data.toHex(' ').toUpper());
        case Message: return decoder_->resolveMessageName(f);
        default: return QString();
    }
}

} // namespace cantrip
