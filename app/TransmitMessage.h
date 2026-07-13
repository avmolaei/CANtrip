// A single configured message in the Stimulation tab's Transmit window -
// plain data, no GUI/backend dependency, owned by MainWindow and scheduled
// by MessageSender.
#pragma once

#include <cstdint>

#include <QByteArray>
#include <QString>

namespace cantrip {

struct TransmitMessage {
    uint32_t id = 0;
    bool extended = false;
    bool fd = false;
    bool brs = false;
    bool rtr = false;
    uint8_t dlc = 0;
    QByteArray data;

    // 0 = one-shot (fires once when sending starts, or via Send Now).
    // >0 = cyclic interval in milliseconds.
    int cycleTimeMs = 0;

    // Excluded from the cyclic scheduler while true - Send Now still fires
    // it regardless, matching the dialog's own "Paused" checkbox.
    bool paused = false;

    QString comment;
};

} // namespace cantrip
