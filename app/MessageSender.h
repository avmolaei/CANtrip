// Transmit side of the Stimulation tab's Send Message feature, plus the
// cyclic scheduler for its configured messages. Plain QObject, no QWidget
// dependency, matching TsharkCapture/ILogWriter/LogReplaySource's pattern:
// a future scripting layer (v3.0) can call into this directly instead of
// needing a rewrite.
//
// Two different transmit mechanisms depending on vendor, since Vector and
// PEAK's driver architectures don't support the same trick:
//  - Vector: opens a second, listen-only port directly on the same channel
//    (see IAvlabsCanBackend::initialize's requestOwnership) and calls
//    writeFrame() on it directly.
//  - PEAK: PCAN-Basic has no equivalent to Vector's permission-mask
//    concept - a second handle can't join a channel can2pcap.exe already
//    initialized (confirmed for real: CAN_Initialize on an already-active
//    channel returns PCAN_ERROR_INITIALIZE). Instead this connects to a
//    named pipe can2pcap.exe itself creates and writes raw CanFrame bytes
//    to it - can2pcap.exe transmits on the handle it already owns (see
//    extcap/can2pcap.cpp's createTxPipe()/tryReadTxPipe()).
#pragma once

#include <memory>
#include <vector>

#include <windows.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "../common/AVlabsCanBackend.h"
#include "TransmitMessage.h"
#include "TsharkCapture.h"

namespace cantrip {

class MessageSender : public QObject {
    Q_OBJECT
public:
    explicit MessageSender(QObject* parent = nullptr);

    // Resolves interfaceId the same way TsharkCapture::Config::interfaceId
    // does (backend + channel lookup) and connects the right way for that
    // vendor (see class comment) - called lazily, on the first actual
    // Start Sending/Send Now click, not automatically at capture start
    // (opening immediately raced against capture_.start() actually
    // finishing launching tshark/can2pcap.exe, which broke live PEAK
    // capture for real - see MainWindow::ensureSenderPortOpen()).
    bool openPort(const QString& interfaceId, const CanBitrateConfig& config, QString* error);
    void closePort();
    bool isPortOpen() const { return portOpen_; }

    int addMessage(const TransmitMessage& message); // returns its index
    void updateMessage(int index, const TransmitMessage& message);
    void removeMessages(const std::vector<int>& indices);
    void clearMessages();
    const std::vector<TransmitMessage>& messages() const { return messages_; }

    // Enables/disables the cyclic scheduler for every non-paused message.
    void start();
    void stop();
    bool isSending() const { return sending_; }

    // Fires the given messages once immediately via writeFrame(), regardless
    // of the cyclic scheduler or paused state - only needs the port open.
    void sendNow(const std::vector<int>& indices);

signals:
    // Constructed with direction = Tx - MainWindow connects this straight to
    // onFrameReceived, the same pipeline capture_/replaySource_ already feed,
    // so logging/display/Graph-view decode all pick it up for free.
    void frameSent(const DecodedCanFrame& frame);

    // A configured message actually failed to reach the wire (broken pipe,
    // lost handle, etc.) - distinct from frameSent, since nothing went out.
    // MainWindow shows this as a red row, the same treatment a real bus
    // error frame already gets.
    void sendFailed(const TransmitMessage& message, const QString& error);

private slots:
    void onTimerTick();

private:
    enum class SendMode { DirectBackend, NamedPipe };

    void transmitOne(int index);
    DecodedCanFrame toDecodedFrame(const TransmitMessage& message) const;
    bool writeFrameOut(const CanFrame& frame, std::string* error);

    SendMode sendMode_ = SendMode::DirectBackend;
    std::unique_ptr<IAvlabsCanBackend> backend_; // DirectBackend mode only
    uint64_t channelId_ = 0;                     // DirectBackend mode only
    HANDLE txPipe_ = INVALID_HANDLE_VALUE;        // NamedPipe mode only
    bool portOpen_ = false;
    QElapsedTimer portClock_;

    std::vector<TransmitMessage> messages_;
    // Parallel to messages_ - next scheduled fire time in ms since portClock_
    // started, for whichever messages are cyclic and not paused.
    std::vector<qint64> nextDueMs_;

    bool sending_ = false;
    QTimer timer_;
};

} // namespace cantrip
