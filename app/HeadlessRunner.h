// Headless entry point: drives a capture (and optionally sends configured
// messages) with no GUI at all - see docs/future/cli-and-headless-mode.md
// for the design and app/main.cpp for how --headless dispatches here.
// Built entirely on top of plain, non-GUI classes (TsharkCapture,
// ILogWriter, MessageSender, DbcDecoder) - this class is just CLI parsing
// and wiring, not new capture/log/send logic.
#pragma once

#include <cstdint>
#include <memory>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "../common/AVlabsCanBackend.h"
#include "DbcDecoder.h"
#include "LogWriter.h"
#include "MessageSender.h"
#include "TsharkCapture.h"

namespace cantrip {

class HeadlessRunner : public QObject {
    Q_OBJECT
public:
    explicit HeadlessRunner(QObject* parent = nullptr);

    // args is the CLI argument list with argv[0] and --headless already
    // stripped (see main.cpp). Returns false (with *error set) on any
    // config problem - caller should print the error and exit non-zero
    // without ever starting the Qt event loop.
    bool start(const QStringList& args, QString* error);

signals:
    // Connected to qApp->quit() (with the exit code stashed) in main.cpp.
    void finished(int exitCode);

private slots:
    void onFrameReceived(const DecodedCanFrame& frame);
    void onCaptureError(const QString& message);
    void onCaptureStopped();
    void onDurationElapsed();
    void checkInterrupted();

private:
    static QString findTsharkExe();
    // Same display-name matching convention RuneConfig::channelDisplayName/
    // MainWindow::refreshChannels() already use - includes the synthetic
    // test source, which probeAvailableBackends() doesn't enumerate (it's
    // not a real backend).
    bool resolveChannel(const QString& displayName, QString* interfaceId, QString* error) const;
    void stopAndFinish(int exitCode);

    TsharkCapture capture_;
    std::unique_ptr<ILogWriter> logWriter_;
    DbcDecoder dbcDecoder_;
    MessageSender messageSender_;

    bool sending_ = false;
    uint64_t frameCount_ = 0;
    QElapsedTimer runClock_;
    QTimer durationTimer_;
    QTimer interruptPollTimer_;
    bool stopping_ = false;
};

} // namespace cantrip
