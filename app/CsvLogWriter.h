// Writes CANtrip captures as CSV - no external format to match (unlike
// AscLogWriter), so this is CANtrip's own schema: one row per frame,
// including bus-error frames (no fidelity constraint against a real tool
// to violate here, unlike ASC).
#pragma once

#include <functional>

#include <QFile>
#include <QTextStream>

#include "LogWriter.h"

namespace cantrip {

class CsvLogWriter : public ILogWriter {
public:
    // Same resolver contract as AscLogWriter::MessageNameResolver - see
    // AscLogWriter.h for why this is a callback rather than a dbcppp
    // dependency.
    using MessageNameResolver = std::function<QString(const DecodedCanFrame&)>;

    explicit CsvLogWriter(MessageNameResolver resolver = {});

    bool open(const QString& path, QString* error) override;
    void writeFrame(int channel, const DecodedCanFrame& frame) override;
    void close() override;
    qint64 bytesWritten() const override { return file_.pos(); }

private:
    QFile file_;
    QTextStream stream_;
    MessageNameResolver messageNameResolver_;
};

} // namespace cantrip
