// Replays a previously-logged .asc or .csv file as a stream of
// DecodedCanFrame, on the same signal surface TsharkCapture exposes
// (frameReceived/errorOccurred/stopped) so MainWindow can treat a replay
// exactly like a live capture without new branching logic.
//
// Parses CANtrip's own AscLogWriter/CsvLogWriter output specifically, not
// arbitrary third-party CANalyzer files - full-generality ASC parsing
// (every optional field/line-type real CANalyzer can produce) is a much
// larger scope than this pass covers.
#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include "TsharkCapture.h"

namespace cantrip {

class LogReplaySource : public QObject {
    Q_OBJECT
public:
    explicit LogReplaySource(QObject* parent = nullptr);

    // Loads the file (format inferred from extension) and immediately
    // begins replaying, paced by the frames' own recorded relative
    // timestamps. Returns false (with *error set) if the file can't be
    // parsed; never partially starts.
    bool start(const QString& path, QString* error);
    void stop();
    bool isRunning() const;

signals:
    void frameReceived(const DecodedCanFrame& frame);
    void errorOccurred(const QString& message);
    void stopped();

private slots:
    void playNext();

private:
    bool loadAsc(const QString& path, QString* error);
    bool loadCsv(const QString& path, QString* error);

    QVector<DecodedCanFrame> frames_;
    int nextIndex_ = 0;
    bool running_ = false;
    QTimer timer_;
};

} // namespace cantrip
