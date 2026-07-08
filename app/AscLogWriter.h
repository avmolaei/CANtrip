// Writes CANtrip captures in Vector's .asc format, targeting the "baseline"
// variant of that format (no per-frame hardware-timing telemetry) - see
// common/CanBitTiming.h-adjacent project notes for why: that telemetry
// (Length=/BitCount=/ID= on classic lines, several trailing fields on CAN
// FD lines) isn't exposed by any vendor API CANtrip talks to, verified by
// checking PCANBasic.h's TPCANMsg/TPCANMsgFD and vxlapi.h's
// s_xl_can_msg/XL_CAN_EV_RX_MSG directly - and was confirmed, empirically,
// to open identically in real CANalyzer with or without that suffix.
//
// Field widths/alignment are based on python-can's real, community-verified
// ASCWriter (can/io/asc.py), cross-checked against - and in a few places
// corrected against - a real CANalyzer trace's actual structure (see git
// history/PR description for this file, not reproduced here since that
// trace was customer-confidential): CANalyzer's date header is 12-hour with
// am/pm and a non-zero-padded day (python-can's own default writer uses
// 24-hour), "Begin TriggerBlock" capitalizes Block (python-can writes
// "Triggerblock"), the timestamp field is 11 characters wide (python-can
// uses 9), and the CAN FD line's symbolic-name field is left-justified in
// real output (python-can right-justifies, indistinguishable in its own
// tests since it never populates a real name).
#pragma once

#include <functional>

#include <QFile>
#include <QTextStream>

#include "LogWriter.h"

namespace cantrip {

class AscLogWriter : public ILogWriter {
public:
    // Returns the DBC message name for a frame, or an empty string if none
    // matches / no DBC is loaded - lets this class embed message names in
    // CAN FD lines (matching real CANalyzer's behavior when a database is
    // loaded during capture) without needing to know about dbcppp types
    // itself. Mirrors MainWindow::populateDecodedChildren's messageById_
    // lookup (app/MainWindow.cpp).
    using MessageNameResolver = std::function<QString(const DecodedCanFrame&)>;

    explicit AscLogWriter(MessageNameResolver resolver = {});

    bool open(const QString& path, QString* error) override;
    void writeFrame(int channel, const DecodedCanFrame& frame) override;
    void close() override;
    qint64 bytesWritten() const override { return file_.pos(); }

private:
    QString formatClassicLine(int channel, const DecodedCanFrame& frame) const;
    QString formatFdLine(int channel, const DecodedCanFrame& frame) const;

    QFile file_;
    QTextStream stream_;
    MessageNameResolver messageNameResolver_;
};

} // namespace cantrip
