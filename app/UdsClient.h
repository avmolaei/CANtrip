// The tester side of UDS diagnostics: sends a request to one ECU and waits
// for its answer.
//
// Requests go out through the session's GatewayRouter, so the capture thread
// that owns the bus transmits them, and answers come in as the ordinary
// received frames every other view sees. ISO-TP (see common/IsoTp.h) splits
// and joins the long ones. Plain QObject with no widgets, so headless mode
// or a script can drive it later.
#pragma once

#include <memory>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include "../common/GatewayRouter.h"
#include "../common/IsoTp.h"
#include "TsharkCapture.h"

namespace cantrip {

class UdsClient : public QObject {
    Q_OBJECT
public:
    struct Target {
        int bus = 1;
        uint32_t requestId = 0x7E0;  // tester to ECU
        uint32_t responseId = 0x7E8; // ECU to tester
        bool extended = false;       // 29-bit IDs
    };

    // How long an ECU gets to answer, and how long once it has said
    // "response pending". Longer than the standard's 50 ms because answers
    // reach this class through the capture pipeline, not straight off the
    // adapter.
    static constexpr int kResponseTimeoutMs = 1000;
    static constexpr int kPendingTimeoutMs = 5000;
    static constexpr int kTesterPresentPeriodMs = 2000;

    explicit UdsClient(QObject* parent = nullptr);

    void setTransmitter(std::shared_ptr<GatewayRouter> router) { router_ = std::move(router); }
    // Ignored while a request is waiting for its answer.
    void setTarget(const Target& target);
    const Target& target() const { return target_; }

    // One request at a time.
    bool busy() const { return busy_; }
    // Sends a request. False with *error set if it cannot go out right now.
    // The outcome arrives as responseReceived() or failed().
    bool request(const QByteArray& bytes, QString* error);

    // Sends Tester Present every two seconds whenever no request is waiting,
    // which keeps an ECU from dropping back to its default session.
    void setTesterPresent(bool on);
    bool testerPresent() const { return testerPresentTimer_.isActive(); }

    // Forget any request in flight, for when the capture stops.
    void cancel();

public slots:
    // Every frame of the session. Only the target's responses are used.
    void onFrame(const DecodedCanFrame& frame);

signals:
    void requestSent(const QByteArray& request);
    // The ECU said "response pending": it is still working on it.
    void responsePending(const QByteArray& request);
    // The final answer, positive or negative.
    void responseReceived(const QByteArray& request, const QByteArray& response);
    // No usable answer: nothing came back, or the transfer broke.
    void failed(const QByteArray& request, const QString& reason);
    void busyChanged(bool busy);

private slots:
    void tick();
    void sendTesterPresent();

private:
    quint64 nowMs() const { return static_cast<quint64>(clock_.elapsed()); }
    // Hands the transport's frames to the bus. False if the bus has no capture.
    bool flushFrames();
    void finish();
    void fail(const QString& reason);
    void handleMessage(const std::vector<uint8_t>& message);

    std::shared_ptr<GatewayRouter> router_;
    Target target_;
    IsoTpChannel transport_;
    QElapsedTimer clock_;
    QTimer tickTimer_;
    QTimer testerPresentTimer_;

    bool busy_ = false;
    bool expectAnswer_ = true;
    QByteArray pending_;
    quint64 deadlineMs_ = 0;
};

} // namespace cantrip
