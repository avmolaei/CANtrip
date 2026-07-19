// DBC load + signal decode, extracted out of MainWindow so it can be used
// without a GUI - see docs/future/cli-and-headless-mode.md. Plain class,
// no Qt widget dependency, same pattern as TsharkCapture/MessageSender.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>

#include <dbcppp/Network.h>

#include "TsharkCapture.h"

namespace cantrip {

class DbcDecoder {
public:
    // Parses the file at path, replacing any previously loaded network on
    // success. Leaves the previous network intact on failure (matches
    // MainWindow::loadDbcFile()'s original behavior: a bad DBC never wipes
    // out a working one). *error gets a human-readable message, including
    // dbcppp's own parse diagnostic when available (it only writes that to
    // stderr, redirected here for the duration of the parse).
    bool loadFile(const QString& path, QString* error);

    bool isLoaded() const { return network_ != nullptr; }
    QString path() const { return path_; }

    // Empty string if no DBC is loaded or the frame's ID doesn't match any
    // message in it.
    QString resolveMessageName(const DecodedCanFrame& frame) const;

    struct DecodedSignal {
        QString name;
        QString qualifiedName; // "MessageName.SignalName"
        QString unit;
        double physicalValue = 0.0;
    };

    // Empty vector if no DBC is loaded or the frame's ID doesn't match any
    // message in it - never partial, a frame either fully decodes against
    // its matched message's signals or contributes nothing.
    std::vector<DecodedSignal> decodeSignals(const DecodedCanFrame& frame) const;

private:
    const dbcppp::IMessage* messageFor(const DecodedCanFrame& frame) const;

    std::unique_ptr<dbcppp::INetwork> network_;
    // Built once per loadFile() call rather than linearly scanning
    // network_->Messages() on every single received frame - a real,
    // measurable cost on a large real-world DBC at real bus rates.
    std::unordered_map<uint32_t, const dbcppp::IMessage*> messageById_;
    QString path_;
};

} // namespace cantrip
