#include "BusAutoDetector.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QtEndian>
#include <QUuid>

namespace cantrip {

namespace {

// Matches extcap/can2pcap.cpp's serializeFrame() exactly: 24-byte global
// pcap header, then one 16-byte (classic) or 72-byte (FD) record per frame -
// this probe only ever requests classic bitrates, so every record here is
// 16 bytes. The CAN ID occupies the first 4 bytes of each record's payload,
// in network (big-endian) byte order, with bit 0x20000000 marking an error
// frame - same constant can2pcap.cpp calls kCanErrFlag.
constexpr int kGlobalHeaderSize = 24;
constexpr int kRecordHeaderSize = 16;
constexpr int kClassicPayloadSize = 16;
constexpr uint32_t kCanErrFlag = 0x20000000u;

const uint32_t kCandidateBitrates[] = {125000, 250000, 500000, 1000000};

} // namespace

bool BusAutoDetector::probeBitrate(const QString& can2pcapPath, const QString& interfaceId,
                                    uint32_t bitrateBps, int probeMs) {
    const QString tempPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath("cantrip_probe_" + QUuid::createUuid().toString(QUuid::Id128) + ".pcap");

    QProcess process;
    process.start(can2pcapPath, {
        "--capture", "--fifo", tempPath,
        "--extcap-interface", interfaceId,
        "--bitrate", QString::number(bitrateBps),
    });
    if (!process.waitForStarted(1000)) {
        QFile::remove(tempPath);
        return false;
    }

    QThread::msleep(static_cast<unsigned long>(probeMs));

    process.terminate();
    if (!process.waitForFinished(500)) {
        process.kill();
        process.waitForFinished(500);
    }

    QFile file(tempPath);
    const bool opened = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = opened ? file.readAll() : QByteArray();
    file.close();
    QFile::remove(tempPath);

    if (bytes.size() <= kGlobalHeaderSize) return false; // no records at all

    int offset = kGlobalHeaderSize;
    int dataRecords = 0;
    int errorRecords = 0;
    while (offset + kRecordHeaderSize + kClassicPayloadSize <= bytes.size()) {
        const uint8_t* record = reinterpret_cast<const uint8_t*>(bytes.constData()) + offset + kRecordHeaderSize;
        const uint32_t canId = qFromBigEndian<uint32_t>(record);
        if (canId & kCanErrFlag) {
            ++errorRecords;
        } else {
            ++dataRecords;
        }
        offset += kRecordHeaderSize + kClassicPayloadSize;
    }

    return dataRecords > 0 && errorRecords == 0;
}

std::optional<uint32_t> BusAutoDetector::detectClassicBitrate(
    const QString& can2pcapPath, const QString& interfaceId) {
    constexpr int kProbeMs = 300;
    for (uint32_t bitrate : kCandidateBitrates) {
        if (probeBitrate(can2pcapPath, interfaceId, bitrate, kProbeMs)) {
            return bitrate;
        }
    }
    return std::nullopt;
}

} // namespace cantrip
