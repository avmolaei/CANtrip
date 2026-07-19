#include "HeadlessRunner.h"

#include <cstdio>

#include <QFileInfo>
#include <QStandardPaths>

#include <windows.h>

#include "AscLogWriter.h"
#include "CsvLogWriter.h"
#include "RuneFile.h"

namespace cantrip {

namespace {

// Must match can2pcap.cpp's / MainWindow.cpp's own copies exactly - see
// MainWindow.cpp's comment on why this is duplicated rather than shared
// (it's how the app tells any backend consumer which vendor+channel an
// interfaceId string refers to).
std::string sanitizeId(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

QString interfaceIdFor(const IAvlabsCanBackend& backend, const CanChannelInfo& channel) {
    return QString::fromStdString(backend.id() + "_" + sanitizeId(channel.name));
}

constexpr const char* kTestInterfaceId = "cantrip_test";

// Ctrl+C handling: Windows delivers console control events (Ctrl+C,
// Ctrl+Break, console close) to a handler running on its own separate
// thread, which must not touch Qt/GUI-thread state directly - it just
// sets this flag, and HeadlessRunner polls it from a normal QTimer on the
// main thread (see checkInterrupted()) rather than trying to marshal a
// cross-thread Qt call out of the handler itself.
volatile LONG g_interrupted = 0;

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE; // handled - don't let the default handler terminate us immediately
    }
    return FALSE;
}

} // namespace

HeadlessRunner::HeadlessRunner(QObject* parent) : QObject(parent) {
    connect(&capture_, &TsharkCapture::frameReceived, this, &HeadlessRunner::onFrameReceived);
    connect(&capture_, &TsharkCapture::errorOccurred, this, &HeadlessRunner::onCaptureError);
    connect(&capture_, &TsharkCapture::stopped, this, &HeadlessRunner::onCaptureStopped);
    connect(&messageSender_, &MessageSender::frameSent, this, &HeadlessRunner::onFrameReceived);

    durationTimer_.setSingleShot(true);
    connect(&durationTimer_, &QTimer::timeout, this, &HeadlessRunner::onDurationElapsed);

    connect(&interruptPollTimer_, &QTimer::timeout, this, &HeadlessRunner::checkInterrupted);
    interruptPollTimer_.start(200);
}

QString HeadlessRunner::findTsharkExe() {
    QString found = QStandardPaths::findExecutable("tshark");
    if (!found.isEmpty()) return found;
    const QString defaultInstall = "C:/Program Files/Wireshark/tshark.exe";
    if (QFileInfo::exists(defaultInstall)) return defaultInstall;
    return "tshark";
}

bool HeadlessRunner::resolveChannel(const QString& displayName, QString* interfaceId, QString* error) const {
    if (displayName == "CANtrip synthetic test source (no hardware needed)") {
        *interfaceId = kTestInterfaceId;
        return true;
    }
    for (auto& backend : probeAvailableBackends()) {
        for (const CanChannelInfo& ch : backend->enumerateChannels()) {
            const QString name = QString::fromStdString(backend->displayName()) + " " + QString::fromStdString(ch.name);
            if (name == displayName) {
                *interfaceId = interfaceIdFor(*backend, ch);
                return true;
            }
        }
    }
    if (error) *error = "Channel not found: " + displayName;
    return false;
}

bool HeadlessRunner::start(const QStringList& args, QString* error) {
    QString runePath, channelName, dbcPath, logPath;
    uint32_t bitrate = 500000, dataBitrate = 2000000;
    bool fd = false, send = false;
    bool bitrateGiven = false, fdGiven = false, dataBitrateGiven = false;
    bool haveDuration = false;
    int durationSec = 0;

    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        auto nextArg = [&]() -> QString { return (i + 1 < args.size()) ? args[++i] : QString(); };

        if (a == "--rune") runePath = nextArg();
        else if (a == "--channel") channelName = nextArg();
        else if (a == "--bitrate") { bitrate = nextArg().toUInt(); bitrateGiven = true; }
        else if (a == "--fd") { fd = true; fdGiven = true; }
        else if (a == "--data-bitrate") { dataBitrate = nextArg().toUInt(); dataBitrateGiven = true; }
        else if (a == "--dbc") dbcPath = nextArg();
        else if (a == "--log") logPath = nextArg();
        else if (a == "--send") send = true;
        else if (a == "--duration") { durationSec = nextArg().toInt(); haveDuration = true; }
        else {
            if (error) *error = "Unknown argument: " + a + "\n\nUsage: cantrip --headless "
                "[--rune <path>] [--channel <name>] [--bitrate <bps>] [--fd] "
                "[--data-bitrate <bps>] [--dbc <path>] [--log <path>] [--send] "
                "[--duration <seconds>]";
            return false;
        }
    }

    CanBitrateConfig busConfig;
    busConfig.nominalBitrateBps = bitrate;
    busConfig.dataBitrateBps = dataBitrate;
    busConfig.fd = fd;
    std::vector<TransmitMessage> transmitMessages;

    if (!runePath.isEmpty()) {
        QString runeError;
        std::optional<RuneConfig> config = loadRuneFile(runePath, &runeError);
        if (!config) {
            if (error) *error = "Could not load rune '" + runePath + "': " + runeError;
            return false;
        }
        if (channelName.isEmpty()) channelName = config->channelDisplayName;
        busConfig = config->busConfig;
        if (dbcPath.isEmpty()) dbcPath = config->dbcPath;
        transmitMessages = config->transmitMessages;

        // Explicit flags still override a loaded rune's own values.
        if (bitrateGiven) busConfig.nominalBitrateBps = bitrate;
        if (fdGiven) busConfig.fd = fd;
        if (dataBitrateGiven) busConfig.dataBitrateBps = dataBitrate;
    }

    if (channelName.isEmpty()) {
        if (error) *error = "No channel specified - pass --channel <name> or --rune <path>.";
        return false;
    }
    QString interfaceId;
    if (!resolveChannel(channelName, &interfaceId, error)) return false;

    if (send && runePath.isEmpty()) {
        if (error) *error = "--send requires --rune (there's nothing configured to send otherwise).";
        return false;
    }

    if (!dbcPath.isEmpty()) {
        QString dbcError;
        if (!dbcDecoder_.loadFile(dbcPath, &dbcError)) {
            if (error) *error = "Could not load DBC '" + dbcPath + "': " + dbcError;
            return false;
        }
    }

    if (!logPath.isEmpty()) {
        auto resolver = [this](const DecodedCanFrame& frame) { return dbcDecoder_.resolveMessageName(frame); };
        const bool csv = logPath.endsWith(".csv", Qt::CaseInsensitive);
        logWriter_ = csv ? std::unique_ptr<ILogWriter>(std::make_unique<CsvLogWriter>(resolver))
                          : std::unique_ptr<ILogWriter>(std::make_unique<AscLogWriter>(resolver));
        QString logError;
        if (!logWriter_->open(logPath, &logError)) {
            if (error) *error = "Could not open log file '" + logPath + "': " + logError;
            return false;
        }
    }

    for (const TransmitMessage& msg : transmitMessages) messageSender_.addMessage(msg);

    TsharkCapture::Config config;
    config.tsharkPath = findTsharkExe();
    config.interfaceId = interfaceId;
    config.nominalBitrateBps = busConfig.nominalBitrateBps;
    config.dataBitrateBps = busConfig.dataBitrateBps;
    config.fd = busConfig.fd;
    config.nomBrp = busConfig.nominalTiming.brp;
    config.nomTseg1 = busConfig.nominalTiming.tseg1;
    config.nomTseg2 = busConfig.nominalTiming.tseg2;
    config.nomSjw = busConfig.nominalTiming.sjw;
    config.dataBrp = busConfig.dataTiming.brp;
    config.dataTseg1 = busConfig.dataTiming.tseg1;
    config.dataTseg2 = busConfig.dataTiming.tseg2;
    config.dataSjw = busConfig.dataTiming.sjw;
    config.listenOnly = false;

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    runClock_.start();
    capture_.start(config);
    fprintf(stdout, "cantrip: capturing on '%s'%s%s\n", qUtf8Printable(channelName),
            logWriter_ ? ", logging to " : "", logWriter_ ? qUtf8Printable(logPath) : "");
    fflush(stdout);

    if (send) {
        // Lazy-open, same reasoning as MainWindow::ensureSenderPortOpen():
        // opening immediately would race against can2pcap.exe actually
        // finishing its own startup. A short delay after capture_.start()
        // is close enough for a scripted/CI run - there's no GUI click to
        // naturally provide the same delay headless has here for free.
        QTimer::singleShot(2000, this, [this, interfaceId, busConfig]() {
            if (stopping_) return;
            QString sendError;
            if (!messageSender_.openPort(interfaceId, busConfig, &sendError)) {
                // Best-effort: report and continue capturing/logging even if
                // sending failed to start - matches MainWindow's own
                // graceful-degradation spirit (a failed send doesn't kill
                // an otherwise-working capture).
                fprintf(stderr, "cantrip: could not start sending: %s\n", qUtf8Printable(sendError));
                fflush(stderr);
                return;
            }
            sending_ = true;
            messageSender_.start();
        });
    }

    if (haveDuration) durationTimer_.start(durationSec * 1000);

    return true;
}

void HeadlessRunner::onFrameReceived(const DecodedCanFrame& frame) {
    ++frameCount_;
    if (logWriter_) logWriter_->writeFrame(1, frame);
}

void HeadlessRunner::onCaptureError(const QString& message) {
    fprintf(stderr, "cantrip: %s\n", qUtf8Printable(message));
    fflush(stderr);
}

void HeadlessRunner::onCaptureStopped() {
    if (stopping_) return; // already tearing down via stopAndFinish()
    // The capture ended on its own (e.g. a fatal backend error) rather
    // than via our own requested stop - treat as a failure.
    stopAndFinish(1);
}

void HeadlessRunner::onDurationElapsed() {
    stopAndFinish(0);
}

void HeadlessRunner::checkInterrupted() {
    if (g_interrupted) stopAndFinish(0);
}

void HeadlessRunner::stopAndFinish(int exitCode) {
    if (stopping_) return;
    stopping_ = true;

    interruptPollTimer_.stop();
    durationTimer_.stop();
    if (sending_) messageSender_.closePort();
    if (capture_.isRunning()) capture_.stop();
    if (logWriter_) logWriter_->close();

    const double elapsedSec = runClock_.elapsed() / 1000.0;
    fprintf(stdout, "cantrip: captured %llu frame(s) in %.1fs\n",
            static_cast<unsigned long long>(frameCount_), elapsedSec);
    fflush(stdout);

    emit finished(exitCode);
}

} // namespace cantrip
