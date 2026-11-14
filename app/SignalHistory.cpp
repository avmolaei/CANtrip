#include "SignalHistory.h"

#include <algorithm>
#include <limits>

namespace cantrip {

void SignalSeries::append(double timeSec, double value) {
    const size_t index = samples_.size();
    if (index > 0 && timeSec < samples_[index - 1].timeSec) timeSec = samples_[index - 1].timeSec;

    if (index == 0) {
        valueMin_ = valueMax_ = value;
    } else {
        valueMin_ = std::min(valueMin_, value);
        valueMax_ = std::max(valueMax_, value);
    }
    samples_.push_back({timeSec, value});
    addToLevels(index, value);
}

void SignalSeries::addToLevels(size_t index, double value) {
    for (size_t level = 0;; ++level) {
        const size_t bucket = index >> (kLevelShift * (level + 1));
        if (level == levels_.size()) {
            // First sample past the top level's single bucket, so everything
            // before it sits in bucket 0 of the level below (or, for the
            // very first level, there is nothing before it at all).
            levels_.emplace_back();
            if (level > 0) levels_[level].push_back(levels_[level - 1][0]);
        }
        Buckets& buckets = levels_[level];
        if (bucket == buckets.size()) {
            buckets.push_back({value, value});
        } else {
            MinMax& mm = buckets[bucket];
            mm.min = std::min(mm.min, value);
            mm.max = std::max(mm.max, value);
        }
        if (bucket == 0) break;
    }
}

SignalSeries::MinMax SignalSeries::minMax(size_t first, size_t last) const {
    MinMax result{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    // level 0 is the samples themselves, level n is levels_[n - 1].
    auto take = [&](size_t level, size_t i) {
        if (level == 0) {
            const double v = samples_[i].value;
            result.min = std::min(result.min, v);
            result.max = std::max(result.max, v);
        } else {
            const MinMax& mm = levels_[level - 1][i];
            result.min = std::min(result.min, mm.min);
            result.max = std::max(result.max, mm.max);
        }
    };

    const size_t mask = (size_t(1) << kLevelShift) - 1;
    for (size_t level = 0; first < last; ++level) {
        if (level == levels_.size()) {
            for (size_t i = first; i < last; ++i) take(level, i);
            break;
        }
        // Peel the ragged ends off at this level, then cover the aligned
        // middle with the next level's buckets.
        while (first < last && (first & mask)) take(level, first++);
        while (first < last && (last & mask)) take(level, --last);
        first >>= kLevelShift;
        last >>= kLevelShift;
    }
    return result;
}

size_t SignalSeries::lowerBound(double timeSec, size_t first, size_t last) const {
    while (first < last) {
        const size_t mid = first + (last - first) / 2;
        if (samples_[mid].timeSec < timeSec) first = mid + 1;
        else last = mid;
    }
    return first;
}

size_t SignalSeries::bytes() const {
    size_t total = samples_.bytes();
    for (const Buckets& level : levels_) total += level.bytes();
    return total;
}

void SignalSeries::decimate(double tMin, double tMax, int columns, QList<QPointF>* out) const {
    out->clear();
    const size_t n = samples_.size();
    if (n == 0 || columns <= 0 || !(tMax > tMin)) return;

    size_t first = lowerBound(tMin);
    if (first > 0) --first;
    size_t last = lowerBound(tMax, first, n);
    while (last < n && samples_[last].timeSec <= tMax) ++last; // samples exactly at tMax
    if (last < n) ++last;
    if (last <= first) return;

    auto emitRaw = [&](size_t a, size_t b) {
        for (size_t i = a; i < b; ++i) out->append(QPointF(samples_[i].timeSec, samples_[i].value));
    };

    if (last - first <= static_cast<size_t>(columns) * 2) {
        out->reserve(static_cast<qsizetype>(last - first));
        emitRaw(first, last);
        return;
    }

    out->reserve(static_cast<qsizetype>(columns) * 2);
    const double span = tMax - tMin;
    size_t a = first;
    for (int column = 0; column < columns && a < last; ++column) {
        const size_t b = column == columns - 1 ? last : lowerBound(tMin + span * (column + 1) / columns, a, last);
        if (b <= a) continue;
        if (b - a <= 2) {
            emitRaw(a, b);
        } else {
            // Which of min and max came first inside one pixel column can't
            // be seen, so order them the way the column trends: that keeps
            // the line into and out of the column pointing the right way.
            const MinMax mm = minMax(a, b);
            const SignalSample& head = samples_[a];
            const SignalSample& tail = samples_[b - 1];
            const bool rising = head.value <= tail.value;
            out->append(QPointF(head.timeSec, rising ? mm.min : mm.max));
            out->append(QPointF(tail.timeSec, rising ? mm.max : mm.min));
        }
        a = b;
    }
}

void SignalSeries::coarsenOldestHalf() {
    const size_t n = samples_.size();
    const size_t half = (n / 2) & ~size_t(3);
    if (half < 8) return;

    size_t write = 0;
    for (size_t group = 0; group < half; group += 4) {
        size_t lo = group, hi = group;
        for (size_t i = group + 1; i < group + 4; ++i) {
            if (samples_[i].value < samples_[lo].value) lo = i;
            if (samples_[i].value > samples_[hi].value) hi = i;
        }
        // A flat group keeps its first and last sample instead.
        const size_t keepA = lo == hi ? group : std::min(lo, hi);
        const size_t keepB = lo == hi ? group + 3 : std::max(lo, hi);
        const SignalSample a = samples_[keepA];
        const SignalSample b = samples_[keepB];
        samples_[write++] = a;
        samples_[write++] = b;
    }
    for (size_t i = half; i < n; ++i) samples_[write++] = samples_[i];
    samples_.truncate(write);

    levels_.clear();
    for (size_t i = 0; i < write; ++i) addToLevels(i, samples_[i].value);
}

SignalHistoryStore::SignalHistoryStore(QObject* parent) : QObject(parent) {}

SignalHistoryStore::~SignalHistoryStore() {
    qDeleteAll(series_);
}

void SignalHistoryStore::recordSample(const QString& qualifiedName, const QString& unit, double timeSec, double value) {
    auto it = series_.find(qualifiedName);
    const bool isNew = it == series_.end();
    if (isNew) {
        it = series_.insert(qualifiedName, new SignalSeries);
        it.value()->unit_ = unit;
    }

    SignalSeries* s = it.value();
    const size_t before = s->bytes();
    s->append(timeSec, value);
    memoryBytes_ += static_cast<qint64>(s->bytes()) - static_cast<qint64>(before);
    latestTimeSec_ = std::max(latestTimeSec_, timeSec);
    s->revision_ = ++revision_;

    if (memoryBytes_ > memoryLimitBytes_) coarsenLargestSeries();
    if (isNew) emit signalAdded(qualifiedName);
}

void SignalHistoryStore::coarsenLargestSeries() {
    SignalSeries* largest = nullptr;
    for (SignalSeries* s : std::as_const(series_)) {
        if (!largest || s->size() > largest->size()) largest = s;
    }
    if (!largest) return;

    const size_t before = largest->bytes();
    const size_t sizeBefore = largest->size();
    largest->coarsenOldestHalf();
    memoryBytes_ += static_cast<qint64>(largest->bytes()) - static_cast<qint64>(before);
    largest->revision_ = ++revision_;
    if (largest->size() < sizeBefore) coarsened_ = true;
}

void SignalHistoryStore::reset() {
    qDeleteAll(series_);
    series_.clear();
    latestTimeSec_ = 0.0;
    memoryBytes_ = 0;
    coarsened_ = false;
    ++revision_;
}

QStringList SignalHistoryStore::signalNames() const {
    return series_.keys();
}

QString SignalHistoryStore::unitFor(const QString& qualifiedName) const {
    const SignalSeries* s = series(qualifiedName);
    return s ? s->unit() : QString();
}

const SignalSeries* SignalHistoryStore::series(const QString& qualifiedName) const {
    return series_.value(qualifiedName, nullptr);
}

} // namespace cantrip
