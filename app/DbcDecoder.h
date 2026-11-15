// DBC load + signal decode, extracted out of MainWindow so it can be used
// without a GUI - see docs/headless-mode.md. Plain class, no Qt widget
// dependency, same pattern as TsharkCapture/MessageSender.
//
// Holds any number of DBC files. Each one applies to every bus or to a single
// bus, and a frame is decoded by the first file, in the order they were
// added, that applies to the frame's bus and defines its ID.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QString>
#include <QStringList>

#include <dbcppp/Network.h>

#include "TsharkCapture.h"

namespace cantrip {

class DbcDecoder {
public:
    // A file applies to this bus number, or to every bus.
    static constexpr int kAllBuses = 0;

    struct FileInfo {
        QString path;
        int bus = kAllBuses;
        int messageCount = 0;
    };

    // Parses the file at path and makes it the only one loaded, for every
    // bus. Leaves what was loaded intact on failure: a bad DBC never wipes
    // out a working one. *error gets a human-readable message, including
    // dbcppp's own parse diagnostic when available (it only writes that to
    // stderr, redirected here for the duration of the parse).
    bool loadFile(const QString& path, QString* error);
    // Parses the file at path and adds it after the ones already loaded, for
    // `bus` (1 to 4) or kAllBuses. Changes nothing on failure.
    bool addFile(const QString& path, int bus, QString* error);
    void removeFile(int index);
    void clear();

    bool isLoaded() const { return !files_.empty(); }
    // The first loaded file's path, empty if there is none.
    QString path() const { return files_.empty() ? QString() : files_.front()->info.path; }
    std::vector<FileInfo> files() const;
    // One line per ID that more than one file defines for the same bus,
    // naming which file wins. Empty when no two files overlap.
    QStringList conflicts() const;

    // Empty string if no DBC is loaded or the frame's ID doesn't match any
    // message in the files that apply to its bus.
    QString resolveMessageName(const DecodedCanFrame& frame) const;

    struct DecodedSignal {
        QString name;
        QString qualifiedName; // "MessageName.SignalName"
        QString unit;
        double physicalValue = 0.0;
    };

    // Empty vector if no DBC is loaded or the frame's ID doesn't match any
    // message - never partial, a frame either fully decodes against its
    // matched message's signals or contributes nothing.
    std::vector<DecodedSignal> decodeSignals(const DecodedCanFrame& frame) const;

    // How many signals decodeSignals() would return, without decoding.
    int signalCount(const DecodedCanFrame& frame) const;
    // Names of the frame's signals, null when no message matches it. Kept
    // from load time, so checking every frame against them stays cheap.
    const QStringList* signalNames(const DecodedCanFrame& frame) const;

private:
    struct File {
        FileInfo info;
        std::unique_ptr<dbcppp::INetwork> network;
        // Built once per load rather than linearly scanning the network's
        // messages on every single received frame - a real, measurable cost
        // on a large real-world DBC at real bus rates.
        std::unordered_map<uint32_t, const dbcppp::IMessage*> messageById;
    };

    static std::unique_ptr<File> parse(const QString& path, int bus, QString* error);
    const dbcppp::IMessage* messageFor(const DecodedCanFrame& frame) const;

    std::vector<std::unique_ptr<File>> files_;
    std::unordered_map<const dbcppp::IMessage*, QStringList> signalNames_;
};

} // namespace cantrip
