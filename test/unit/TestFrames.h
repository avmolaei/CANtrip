// Frame builders shared by the unit tests.
#pragma once

#include <QByteArray>
#include <QString>

#include "TsharkCapture.h"

namespace cantrip::testing {

// 0x100 with this payload decodes, against test/sample.dbc, to
// EngineSpeed = 1418 rpm and EngineTemp = 90 degC.
inline QByteArray engineData() { return QByteArray::fromHex("502C820000000000"); }

inline DecodedCanFrame frame(uint32_t id, double timeSec, const QByteArray& data = engineData()) {
    DecodedCanFrame f;
    f.id = id;
    f.dlc = static_cast<uint8_t>(data.size());
    f.data = data;
    f.timestamp = QString::number(timeSec, 'f', 6);
    return f;
}

inline DecodedCanFrame errorFrame(double timeSec, const QString& description) {
    DecodedCanFrame f;
    f.error = true;
    f.errorDescription = description;
    f.timestamp = QString::number(timeSec, 'f', 6);
    return f;
}

inline QString sampleDbcPath() { return QStringLiteral(CANTRIP_TEST_DATA_DIR "/sample.dbc"); }

} // namespace cantrip::testing
