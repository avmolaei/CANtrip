#include "HeadlessRunner.h"

#include <cstdio>

#include <QFileInfo>
#include <QStandardPaths>

#include <windows.h>

#include "AscLogWriter.h"
#include "CsvLogWriter.h"
#include "GatewayText.h"
#include "RuneFile.h"

namespace cantrip {

namespace {

constexpr const char* kTestInterfaceId = kSyntheticInterfaceId;

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
    connect(&gatewayTimer_, &QTimer::timeout, this, &HeadlessRunner::pumpGateway);
    messageSender_.setBusTransmitter(gateway_);
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
                *interfaceId = QString::fromStdString(interfaceIdFor(*backend, ch));
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

    // --bus N switches which bus the channel and bitrate flags after it
    // apply to. Without it everything is bus 1, as it always was.
    struct ExtraBus {
        int bus = 0;
        QString channelName;
        uint32_t bitrate = 500000, dataBitrate = 2000000;
        bool fd = false;
    };
    std::vector<ExtraBus> extraBuses;
    // Each --dbc, with the bus it was given under (all buses before any --bus).
    std::vector<RuneDbcFile> dbcFiles;
    std::vector<GatewayRule> gatewayRules;
    bool gatewayGiven = false;
    ExtraBus* current = nullptr; // null while the flags apply to bus 1
    bool afterBusOne = false;    // an explicit --bus 1 was given

    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        auto nextArg = [&]() -> QString { return (i + 1 < args.size()) ? args[++i] : QString(); };

        if (a == "--rune") runePath = nextArg();
        else if (a == "--bus") {
            const int bus = nextArg().toInt();
            if (bus < 1 || bus > kMaxBuses) {
                if (error) *error = QString("--bus takes a bus number from 1 to %1.").arg(kMaxBuses);
                return false;
            }
            current = nullptr;
            afterBusOne = bus == 1;
            if (bus != 1) {
                for (ExtraBus& b : extraBuses) if (b.bus == bus) current = &b;
                if (!current) {
                    extraBuses.reserve(kMaxBuses); // keeps `current` valid across push_back
                    extraBuses.push_back(ExtraBus{bus});
                    current = &extraBuses.back();
                }
            }
        }
        else if (a == "--channel") { if (current) current->channelName = nextArg(); else channelName = nextArg(); }
        else if (a == "--bitrate") {
            if (current) current->bitrate = nextArg().toUInt();
            else { bitrate = nextArg().toUInt(); bitrateGiven = true; }
        }
        else if (a == "--fd") { if (current) current->fd = true; else { fd = true; fdGiven = true; } }
        else if (a == "--data-bitrate") {
            if (current) current->dataBitrate = nextArg().toUInt();
            else { dataBitrate = nextArg().toUInt(); dataBitrateGiven = true; }
        }
        else if (a == "--dbc") {
            dbcPath = nextArg();
            dbcFiles.push_back({dbcPath, current ? current->bus : (afterBusOne ? 1 : DbcDecoder::kAllBuses)});
        }
        else if (a == "--gateway") {
            // <from>:<to>, or <from>:<to>:<ids> with ids as in the Gateway dialog.
            const QStringList parts = nextArg().split(':');
            GatewayRule rule;
            bool ok = parts.size() == 2 || parts.size() == 3;
            if (ok) {
                rule.fromBus = parts[0].toInt();
                rule.toBus = parts[1].toInt();
                ok = rule.fromBus >= 1 && rule.fromBus <= kMaxBuses && rule.toBus >= 1 && rule.toBus <= kMaxBuses &&
                     rule.fromBus != rule.toBus;
            }
            if (ok && parts.size() == 3) ok = parseGatewayIdRange(parts[2], &rule.idLow, &rule.idHigh);
            if (!ok) {
                if (error) *error = "--gateway takes <from bus>:<to bus>, optionally followed by :<id> or :<id>-<id>, e.g. 1:2:0x100-0x1FF.";
                return false;
            }
            gatewayRules.push_back(rule);
            gatewayGiven = true;
        }
        else if (a == "--log") logPath = nextArg();
        else if (a == "--send") send = true;
        else if (a == "--duration") { durationSec = nextArg().toInt(); haveDuration = true; }
        else {
            if (error) *error = "Unknown argument: " + a + "\n\nUsage: cantrip --headless "
                "[--rune <path>] [--channel <name>] [--bitrate <bps>] [--fd] "
                "[--data-bitrate <bps>] [--dbc <path>] [--log <path>] [--send] "
                "[--duration <seconds>] "
                "[--bus <n> --channel <name> [--bitrate <bps>] [--fd] [--data-bitrate <bps>]]... "
                "[--gateway <from>:<to>[:<ids>]]...";
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
        if (!gatewayGiven && config->gatewayEnabled) gatewayRules = config->gatewayRules;
        if (dbcFiles.empty()) {
            dbcFiles = config->dbcFiles;
            if (dbcFiles.empty() && !config->dbcPath.isEmpty()) dbcFiles.push_back({config->dbcPath, DbcDecoder::kAllBuses});
        }
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

    for (const RuneDbcFile& file : dbcFiles) {
        QString dbcError;
        if (!dbcDecoder_.addFile(file.path, file.bus, &dbcError)) {
            if (error) *error = "Could not load DBC '" + file.path + "': " + dbcError;
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
    config.gateway = gateway_;

    // Everything about the extra buses is checked before any capture starts,
    // so a typo in bus 3's channel doesn't leave buses 1 and 2 running.
    std::vector<TsharkCapture::Config> extraConfigs;
    for (const ExtraBus& extra : extraBuses) {
        if (extra.channelName.isEmpty()) {
            if (error) *error = QString("--bus %1 has no --channel.").arg(extra.bus);
            return false;
        }
        TsharkCapture::Config extraConfig;
        extraConfig.tsharkPath = config.tsharkPath;
        extraConfig.bus = extra.bus;
        extraConfig.nominalBitrateBps = extra.bitrate;
        extraConfig.dataBitrateBps = extra.dataBitrate;
        extraConfig.fd = extra.fd;
        extraConfig.gateway = gateway_;
        if (!resolveChannel(extra.channelName, &extraConfig.interfaceId, error)) return false;
        if (extraConfig.interfaceId == interfaceId && interfaceId != kTestInterfaceId) {
            if (error) *error = QString("Bus %1 uses the same channel as bus 1.").arg(extra.bus);
            return false;
        }
        extraConfigs.push_back(extraConfig);
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    // A rule naming a bus this run does not capture would never do anything.
    for (const GatewayRule& rule : gatewayRules) {
        for (int bus : {rule.fromBus, rule.toBus}) {
            bool captured = bus == 1;
            for (const ExtraBus& extra : extraBuses) captured = captured || extra.bus == bus;
            if (!captured) {
                if (error) *error = QString("The gateway rule %1 to %2 names bus %3, which this run does not capture.")
                                        .arg(rule.fromBus).arg(rule.toBus).arg(bus);
                return false;
            }
        }
    }
    gateway_->reset();
    gateway_->setRules(gatewayRules);
    // Also running without rules: --send messages for buses beyond the first
    // come back through the same router.
    gatewayTimer_.start(20);
    if (!gatewayRules.empty()) {
        fprintf(stdout, "cantrip: gateway on, %d rule(s)\n", static_cast<int>(gatewayRules.size()));
    }

    runClock_.start();
    timeAligner_.reset();
    capture_.start(config);
    fprintf(stdout, "cantrip: capturing on '%s'%s%s\n", qUtf8Printable(channelName),
            logWriter_ ? ", logging to " : "", logWriter_ ? qUtf8Printable(logPath) : "");
    for (size_t i = 0; i < extraConfigs.size(); ++i) {
        auto capture = std::make_unique<TsharkCapture>();
        connect(capture.get(), &TsharkCapture::frameReceived, this, &HeadlessRunner::onFrameReceived);
        connect(capture.get(), &TsharkCapture::errorOccurred, this, &HeadlessRunner::onCaptureError);
        connect(capture.get(), &TsharkCapture::stopped, this, &HeadlessRunner::onCaptureStopped);
        capture->start(extraConfigs[i]);
        extraCaptures_.push_back(std::move(capture));
        fprintf(stdout, "cantrip: bus %d capturing on '%s'\n", extraBuses[i].bus, qUtf8Printable(extraBuses[i].channelName));
    }
    fflush(stdout);

    if (send) {
        // Lazy-open, same reasoning as MainWindow::ensureSenderPortOpen():
        // opening immediately would race against the capture thread actually
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
    if (!logWriter_) return;
    if (extraCaptures_.empty()) {
        logWriter_->writeFrame(frame.bus, frame);
        return;
    }
    // Several buses: each capture counts time from its own first frame, so
    // put them on one clock before they share a log (see BusTimeAligner.h).
    DecodedCanFrame aligned = frame;
    const double t = timeAligner_.align(frame.bus, frame.timestamp.toDouble(), runClock_.nsecsElapsed() / 1e9);
    aligned.timestamp = QString::number(t, 'f', 6);
    logWriter_->writeFrame(aligned.bus, aligned);
}

void HeadlessRunner::pumpGateway() {
    const std::vector<DecodedCanFrame> rows = gatewayPump_.pump(*gateway_, timeAligner_, runClock_.nsecsElapsed() / 1e9);
    if (!logWriter_) return;
    for (const DecodedCanFrame& frame : rows) logWriter_->writeFrame(frame.bus, frame);
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
    gatewayTimer_.stop();
    pumpGateway();
    if (gateway_->active()) {
        uint64_t forwarded = 0;
        for (uint64_t n : gateway_->forwardedCounts()) forwarded += n;
        fprintf(stdout, "cantrip: gateway forwarded %llu frame(s), %llu dropped, %llu failed%s%s\n",
                static_cast<unsigned long long>(forwarded), static_cast<unsigned long long>(gateway_->dropped()),
                static_cast<unsigned long long>(gateway_->failed()), gateway_->failed() ? ": " : "",
                gateway_->failed() ? gateway_->lastError().c_str() : "");
    }
    if (sending_) messageSender_.closePort();
    if (capture_.isRunning()) capture_.stop();
    for (auto& capture : extraCaptures_) {
        if (capture->isRunning()) capture->stop();
    }
    if (logWriter_) logWriter_->close();

    const double elapsedSec = runClock_.elapsed() / 1000.0;
    fprintf(stdout, "cantrip: captured %llu frame(s) in %.1fs\n",
            static_cast<unsigned long long>(frameCount_), elapsedSec);
    fflush(stdout);

    emit finished(exitCode);
}

} // namespace cantrip
