#include "SignalHistory.h"

namespace cantrip {

const QVector<SignalSample> SignalHistoryStore::kEmptySamples;

SignalHistoryStore::SignalHistoryStore(QObject* parent) : QObject(parent) {}

void SignalHistoryStore::recordSample(const QString& qualifiedName, const QString& unit, double timeSec, double value) {
    const bool isNew = !samples_.contains(qualifiedName);
    QVector<SignalSample>& vec = samples_[qualifiedName];
    units_[qualifiedName] = unit;

    SignalSample sample{timeSec, value};
    vec.append(sample);
    if (vec.size() > kMaxSamplesPerSignal) {
        vec.remove(0, vec.size() - kMaxSamplesPerSignal);
    }

    if (isNew) emit signalAdded(qualifiedName);
    emit sampleAdded(qualifiedName, sample);
}

void SignalHistoryStore::reset() {
    samples_.clear();
    units_.clear();
}

QStringList SignalHistoryStore::signalNames() const {
    return samples_.keys();
}

QString SignalHistoryStore::unitFor(const QString& qualifiedName) const {
    return units_.value(qualifiedName);
}

const QVector<SignalSample>& SignalHistoryStore::samplesFor(const QString& qualifiedName) const {
    auto it = samples_.find(qualifiedName);
    return it == samples_.end() ? kEmptySamples : it.value();
}

} // namespace cantrip
