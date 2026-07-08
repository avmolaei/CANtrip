// Vendor-neutral "write a decoded frame to disk" interface - one
// implementation per log format (ASC, CSV, later MF4), same spirit as
// AVlabsCanBackend.h's one-interface-many-vendors pattern.
#pragma once

#include <QString>

#include "TsharkCapture.h"

namespace cantrip {

class ILogWriter {
public:
    virtual ~ILogWriter() = default;

    // Opens the file at `path` and writes any format header. Returns false
    // (with *error set) on failure; never partially opens a broken file.
    virtual bool open(const QString& path, QString* error) = 0;

    // `channel` is a logical bus/channel number (1-based, matching how
    // CANalyzer's own .asc numbers channels) - CANtrip only ever passes 1
    // today (single-bus), but the parameter exists from day one so a
    // future multi-bus capture manager can feed several channels into one
    // writer/file without an interface change.
    virtual void writeFrame(int channel, const DecodedCanFrame& frame) = 0;

    virtual void close() = 0;

    // Current file size - used by MainWindow to implement max-file-size
    // auto-splitting (LoggingOptionsDialog's Tab 2).
    virtual qint64 bytesWritten() const = 0;
};

} // namespace cantrip
