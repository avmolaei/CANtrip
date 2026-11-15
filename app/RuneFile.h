// .rune files save/restore everything needed to pick up a CANtrip session
// where it left off: which channel, bus timing, capture display mode/rate,
// which DBC, and the Graph view's axis/signal layout - not sample data,
// which is always live/session-only. Named after CANalyzer's own .cfg
// files, which this is the equivalent of.
#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "../common/AVlabsCanBackend.h"
#include "../common/GatewayRouter.h"
#include "GraphView.h"
#include "TransmitMessage.h"

namespace cantrip {

// Standalone rather than reusing MainWindow::DisplayMode - that enum is a
// private implementation detail of MainWindow, and this header shouldn't
// need to depend on MainWindow.h just to describe a file format.
enum class RuneDisplayMode { Waterfall, Periodic };

// How many buses one session can hold: CAN 1 plus the ones added in the
// Hardware tab's bus list.
constexpr int kMaxBuses = 16;

// One loaded DBC file: for every bus (0) or for one bus (1 to 4).
struct RuneDbcFile {
    QString path;
    int bus = 0;
};

// A bus beyond the first, with its own channel and bus configuration.
struct RuneExtraBus {
    int bus = 2;
    QString channelDisplayName;
    CanBitrateConfig busConfig;
    bool listenOnly = false;
};

struct RuneConfig {
    // Matched back to a channel by display name (not interfaceId, which is
    // backend-namespaced and can shift if hardware changes) - empty/not-found
    // is a valid outcome the loader has to handle, not an error.
    QString channelDisplayName;
    CanBitrateConfig busConfig;
    RuneDisplayMode displayMode = RuneDisplayMode::Waterfall;
    int displayRateMs = 33;
    // Path reference, not an embedded DBC - re-loaded via the same
    // dbcppp::INetwork::LoadDBCFromIs() path MainWindow::importDbc() uses.
    // Empty means no DBC was loaded when the rune was saved.
    // The first DBC file, which is all that versions before multi-DBC read.
    QString dbcPath;
    // Every DBC file, in lookup order. Empty in runes saved before
    // multi-DBC, where dbcPath says it all.
    std::vector<RuneDbcFile> dbcFiles;
    // One entry per graph window (see GraphWindowContainer) - always at
    // least one entry for a rune saved by the current version; loadRuneFile()
    // also accepts the older single-window file shape (see RuneFile.cpp).
    std::vector<std::vector<GraphView::AxisLayout>> graphWindows;
    // Configured Stimulation-tab messages (see MessageSender) - empty for a
    // rune saved before this field existed, loadRuneFile() tolerates its
    // absence with no version field needed (see RuneFile.cpp).
    std::vector<TransmitMessage> transmitMessages;
    // Trace view's column order/widths (QHeaderView::saveState()'s own
    // opaque format) - empty for a rune saved before this field existed,
    // in which case the header is just left as whatever it currently is.
    QByteArray traceHeaderState;
    // CAN Controller dialog's "Request bus configuration" checkbox (see
    // MainWindow::listenOnlyMode_) - false (request ownership, today's
    // longstanding default) for a rune saved before this field existed.
    bool listenOnly = false;
    std::vector<RuneExtraBus> extraBuses;
    // Gateway rules, and whether the gateway is switched on. Rules are kept
    // while it is off.
    std::vector<GatewayRule> gatewayRules;
    bool gatewayEnabled = false;
};

// Returns false (with *error set to a human-readable message) on any
// failure. Never partially applies a broken/malformed file.
bool saveRuneFile(const QString& path, const RuneConfig& config, QString* error);
std::optional<RuneConfig> loadRuneFile(const QString& path, QString* error);

} // namespace cantrip
