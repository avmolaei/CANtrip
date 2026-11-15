// The gateway's small conversions, shared by the window and headless mode:
// the ID range a rule is typed as, and a forwarded frame as a trace row.
#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>

#include <QString>
#include <QStringList>

#include "../common/GatewayRouter.h"
#include "BusTimeAligner.h"
#include "TsharkCapture.h"

namespace cantrip {

constexpr uint32_t kGatewayMaxId = 0x1FFFFFFFu;

// "" or "all" is every ID, "0x100" one ID, "0x100-0x1FF" a range. Numbers
// are hex with 0x in front, decimal without.
inline bool parseGatewayIdRange(const QString& text, uint32_t* low, uint32_t* high) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t.compare("all", Qt::CaseInsensitive) == 0) {
        *low = 0;
        *high = kGatewayMaxId;
        return true;
    }
    const QStringList parts = t.split('-');
    if (parts.size() > 2) return false;
    bool ok = false;
    const uint32_t first = parts[0].trimmed().toUInt(&ok, 0);
    if (!ok) return false;
    uint32_t second = first;
    if (parts.size() == 2) {
        second = parts[1].trimmed().toUInt(&ok, 0);
        if (!ok) return false;
    }
    if (first > second || second > kGatewayMaxId) return false;
    *low = first;
    *high = second;
    return true;
}

inline QString gatewayIdRangeText(const GatewayRule& rule) {
    if (rule.idLow == 0 && rule.idHigh >= kGatewayMaxId) return "all";
    const QString low = "0x" + QString::number(rule.idLow, 16).toUpper();
    if (rule.idLow == rule.idHigh) return low;
    return low + "-0x" + QString::number(rule.idHigh, 16).toUpper();
}

// A frame the gateway transmitted, as the Tx row it shows up as on its
// destination bus, at the given time on the session's timeline.
inline DecodedCanFrame decodedFromGatewaySent(const GatewaySent& sent, double timelineSec) {
    static const int kFdLength[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    DecodedCanFrame frame;
    frame.direction = FrameDirection::Tx;
    frame.bus = sent.bus;
    frame.timestamp = QString::number(std::max(0.0, timelineSec), 'f', 6);
    frame.id = sent.frame.id;
    frame.extended = sent.frame.extended;
    frame.rtr = sent.frame.rtr;
    frame.fd = sent.frame.fd;
    frame.brs = sent.frame.brs;
    frame.esi = sent.frame.esi;
    const int length = sent.frame.fd ? kFdLength[sent.frame.dlc & 0x0F] : std::min<int>(sent.frame.dlc, 8);
    frame.dlc = static_cast<uint8_t>(length); // a length in bytes, as tshark reports it
    frame.data = QByteArray(reinterpret_cast<const char*>(sent.frame.data), sent.frame.rtr ? 0 : length);
    return frame;
}

// Turns what the router transmitted into Tx rows with the right time.
//
// A transmitted frame is known at once, but received frames reach the window
// through tshark, which at the start of a capture can be a second or two
// behind. Stamping a forwarded frame with "now" would put it long before the
// received frame it is a copy of. So each one is timed on a bus's own clock
// (see GatewaySent::clockBus) and held back until that bus has shown its
// first frame, which is when its place on the shared timeline is known.
class GatewaySentPump {
public:
    void reset() { pending_.clear(); }

    // sessionNowSec is the session clock read now.
    std::vector<DecodedCanFrame> pump(GatewayRouter& router, const BusTimeAligner& aligner, double sessionNowSec) {
        for (GatewaySent& sent : router.drainSent()) pending_.push_back(std::move(sent));
        std::vector<DecodedCanFrame> rows;
        const auto now = std::chrono::steady_clock::now();
        while (!pending_.empty()) {
            const GatewaySent& sent = pending_.front();
            const double age = std::chrono::duration<double>(now - sent.when).count();
            if (sent.clockBus != 0 && aligner.hasBus(sent.clockBus)) {
                rows.push_back(decodedFromGatewaySent(sent, aligner.timeOn(sent.clockBus, sent.clockSec)));
            } else if (sent.clockBus == 0 || age > kMaxHeldSec) {
                // No bus clock to go by: the session clock is the best there is.
                rows.push_back(decodedFromGatewaySent(sent, aligner.fromSession(sessionNowSec - age)));
            } else {
                break; // its bus has not shown a frame yet, and order is kept
            }
            pending_.pop_front();
        }
        return rows;
    }

private:
    static constexpr double kMaxHeldSec = 5.0;
    std::deque<GatewaySent> pending_;
};

} // namespace cantrip
