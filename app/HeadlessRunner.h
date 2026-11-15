// Headless entry point: drives a capture (and optionally sends configured
// messages) with no GUI at all - see docs/future/cli-and-headless-mode.md
// for the design and app/main.cpp for how --headless dispatches here.
// Built entirely on top of plain, non-GUI classes (TsharkCapture,
// ILogWriter, MessageSender, DbcDecoder) - this class is just CLI parsing
// and wiring, not new capture/log/send logic.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "../common/AVlabsCanBackend.h"
#include "BusTimeAligner.h"
#include "GatewayText.h"
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
    void pumpGateway();

private:
    static QString findTsharkExe();
    // Same display-name matching convention RuneConfig::channelDisplayName/
    // MainWindow::refreshChannels() already use - includes the synthetic
    // test source, which probeAvailableBackends() doesn't enumerate (it's
    // not a real backend).
    bool resolveChannel(const QString& displayName, QString* interfaceId, QString* error) const;
    void stopAndFinish(int exitCode);

    TsharkCapture capture_; // bus 1
    // Buses 2 to 4, when --bus was given. Bus 1 keeps its own member so a
    // plain single-bus run goes through exactly the code it always did.
    std::vector<std::unique_ptr<TsharkCapture>> extraCaptures_;
    BusTimeAligner timeAligner_;
    std::unique_ptr<ILogWriter> logWriter_;
    DbcDecoder dbcDecoder_;
    MessageSender messageSender_;

    bool sending_ = false;
    uint64_t frameCount_ = 0;
    QElapsedTimer runClock_;
    QTimer durationTimer_;
    QTimer interruptPollTimer_;
    // Forwards between buses when --gateway or the rune asks for it. The
    // timer writes what it forwarded to the log, as Tx on the bus it went to.
    std::shared_ptr<GatewayRouter> gateway_ = std::make_shared<GatewayRouter>();
    QTimer gatewayTimer_;
    GatewaySentPump gatewayPump_;
    bool stopping_ = false;
};

} // namespace cantrip
