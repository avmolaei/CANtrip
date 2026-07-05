// Keeps a bounded time-series history of every decoded DBC signal seen so
// far in the current capture, so the Graph view has something to plot -
// unlike the trace view, which decodes each frame fresh and discards it.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace cantrip {

struct SignalSample {
    double timeSec = 0.0;
    double value = 0.0;
};

class SignalHistoryStore : public QObject {
    Q_OBJECT
public:
    explicit SignalHistoryStore(QObject* parent = nullptr);

    // qualifiedName is "<MessageName>.<SignalName>", e.g. "EngineData.EngineSpeed".
    void recordSample(const QString& qualifiedName, const QString& unit, double timeSec, double value);
    // Clears all history - called when a new capture starts.
    void reset();

    QStringList signalNames() const;
    QString unitFor(const QString& qualifiedName) const;
    const QVector<SignalSample>& samplesFor(const QString& qualifiedName) const;

signals:
    // Emitted the first time a given qualifiedName is recorded, so the
    // Graph view's signal list can grow live as new message types appear.
    void signalAdded(const QString& qualifiedName);
    void sampleAdded(const QString& qualifiedName, SignalSample sample);

private:
    // Bounds memory for an unattended capture - oldest samples are dropped
    // once a signal's history exceeds this, same spirit as MainWindow's
    // kStaleTimeoutMs bounding periodic-row state.
    static constexpr int kMaxSamplesPerSignal = 5000;

    QHash<QString, QVector<SignalSample>> samples_;
    QHash<QString, QString> units_;
    static const QVector<SignalSample> kEmptySamples;
};

} // namespace cantrip
