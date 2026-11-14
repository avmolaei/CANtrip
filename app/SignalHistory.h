// Keeps the time-series history of every decoded DBC signal for the whole
// capture, so the Graph view can plot any of it at any zoom - unlike the
// trace view, which only keeps its newest rows.
//
// Every raw sample is kept. What makes that affordable to draw is
// SignalSeries::decimate(): it hands the Graph view at most two points per
// pixel column (that column's min and max), found through a small min/max
// pyramid, so a 10 second view and a 10 hour view cost the same to draw.
// Zoomed in far enough, decimate() returns the raw samples themselves.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace cantrip {

struct SignalSample {
    double timeSec = 0.0;
    double value = 0.0;
};

// Append-only array in fixed-size chunks: growing never copies what's already
// stored, unlike a std::vector that has to move hundreds of MB to double.
// Chunks hold 2^ChunkShift elements: small enough that a DBC with thousands
// of rarely-seen signals doesn't reserve much for each of them.
template <typename T, size_t ChunkShift>
class ChunkedArray {
public:
    static constexpr size_t kChunkShift = ChunkShift;
    static constexpr size_t kChunkSize = size_t(1) << kChunkShift;

    size_t size() const { return size_; }
    const T& operator[](size_t i) const { return chunks_[i >> kChunkShift][i & (kChunkSize - 1)]; }
    T& operator[](size_t i) { return chunks_[i >> kChunkShift][i & (kChunkSize - 1)]; }

    void push_back(const T& value) {
        if ((size_ >> kChunkShift) == chunks_.size()) chunks_.emplace_back(new T[kChunkSize]);
        (*this)[size_++] = value;
    }
    // Shrinks to n elements and frees the chunks no longer needed.
    void truncate(size_t n) {
        size_ = n;
        chunks_.resize((n + kChunkSize - 1) >> kChunkShift);
    }
    void clear() { truncate(0); }
    size_t bytes() const { return chunks_.size() * kChunkSize * sizeof(T); }

private:
    std::vector<std::unique_ptr<T[]>> chunks_;
    size_t size_ = 0;
};

class SignalSeries {
public:
    struct MinMax {
        double min;
        double max;
    };

    size_t size() const { return samples_.size(); }
    const SignalSample& at(size_t i) const { return samples_[i]; }
    // First index whose time is >= timeSec, size() if there is none.
    size_t lowerBound(double timeSec) const { return lowerBound(timeSec, 0, samples_.size()); }
    // Min and max value over samples [first, last). Needs first < last.
    MinMax minMax(size_t first, size_t last) const;
    // Over every sample ever recorded, including ones since coarsened.
    double valueMin() const { return valueMin_; }
    double valueMax() const { return valueMax_; }
    // Changes whenever the contents do, and is never reused by another
    // series of the same store, so it stays meaningful across a reset().
    quint64 revision() const { return revision_; }
    const QString& unit() const { return unit_; }
    size_t bytes() const;

    // Fills *out with what to draw for the time range [tMin, tMax] on a plot
    // `columns` pixels wide: the raw samples when there are few enough, else
    // each pixel column's min and max. Includes one sample either side of the
    // range so a line runs to the plot's edges.
    void decimate(double tMin, double tMax, int columns, QList<QPointF>* out) const;

private:
    friend class SignalHistoryStore;

    void append(double timeSec, double value);
    // Halves the oldest half: each 4 samples become their min and max, so
    // spikes survive. Used only when the store goes over its memory limit.
    void coarsenOldestHalf();
    void addToLevels(size_t index, double value);
    size_t lowerBound(double timeSec, size_t first, size_t last) const;

    // Bucket sizes grow 8x per level: levels_[0] holds the min/max of every
    // 8 samples, levels_[1] of every 64, and so on up to a single bucket.
    static constexpr int kLevelShift = 3;

    using Samples = ChunkedArray<SignalSample, 10>;
    using Buckets = ChunkedArray<MinMax, 8>;

    Samples samples_;
    std::vector<Buckets> levels_;
    double valueMin_ = 0.0;
    double valueMax_ = 0.0;
    quint64 revision_ = 0;
    QString unit_;
};

class SignalHistoryStore : public QObject {
    Q_OBJECT
public:
    // Past this, the oldest half of the largest signal gets coarsened (see
    // SignalSeries::coarsenOldestHalf) each time the total goes over again.
    // The whole capture stays plottable, its start just loses detail.
    static constexpr qint64 kDefaultMemoryLimitBytes = qint64(2) * 1024 * 1024 * 1024;

    explicit SignalHistoryStore(QObject* parent = nullptr);
    ~SignalHistoryStore() override;

    // qualifiedName is "<MessageName>.<SignalName>", e.g. "EngineData.EngineSpeed".
    // A sample older than the signal's newest is recorded at the newest's
    // time: frames CANtrip sends are stamped by a different clock than
    // received ones, and a series has to stay in time order to be searched.
    void recordSample(const QString& qualifiedName, const QString& unit, double timeSec, double value);
    // Clears all history - called when a new capture starts. Invalidates
    // every pointer series() has returned.
    void reset();

    QStringList signalNames() const;
    QString unitFor(const QString& qualifiedName) const;
    // Null for a signal never recorded.
    const SignalSeries* series(const QString& qualifiedName) const;

    // Changes whenever anything in the store does.
    quint64 revision() const { return revision_; }
    double latestTimeSec() const { return latestTimeSec_; }
    qint64 memoryBytes() const { return memoryBytes_; }
    // True once the memory limit has cost the oldest data some detail.
    bool coarsened() const { return coarsened_; }
    void setMemoryLimitBytes(qint64 bytes) { memoryLimitBytes_ = bytes; }

signals:
    // Emitted the first time a given qualifiedName is recorded, so the
    // Graph view's signal list can grow live as new message types appear.
    void signalAdded(const QString& qualifiedName);

private:
    void coarsenLargestSeries();

    QHash<QString, SignalSeries*> series_;
    quint64 revision_ = 0;
    double latestTimeSec_ = 0.0;
    qint64 memoryBytes_ = 0;
    qint64 memoryLimitBytes_ = kDefaultMemoryLimitBytes;
    bool coarsened_ = false;
};

} // namespace cantrip
