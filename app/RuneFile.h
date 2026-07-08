// .rune files save/restore everything needed to pick up a CANtrip session
// where it left off: which channel, bus timing, capture display mode/rate,
// which DBC, and the Graph view's axis/signal layout - not sample data,
// which is always live/session-only. Named after CANalyzer's own .cfg
// files, which this is the equivalent of.
#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "../common/AVlabsCanBackend.h"
#include "GraphView.h"

namespace cantrip {

// Standalone rather than reusing MainWindow::DisplayMode - that enum is a
// private implementation detail of MainWindow, and this header shouldn't
// need to depend on MainWindow.h just to describe a file format.
enum class RuneDisplayMode { Waterfall, Periodic };

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
    QString dbcPath;
    std::vector<GraphView::AxisLayout> graphLayout;
};

// Returns false (with *error set to a human-readable message) on any
// failure. Never partially applies a broken/malformed file.
bool saveRuneFile(const QString& path, const RuneConfig& config, QString* error);
std::optional<RuneConfig> loadRuneFile(const QString& path, QString* error);

} // namespace cantrip
