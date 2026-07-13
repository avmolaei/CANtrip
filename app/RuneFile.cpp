#include "RuneFile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cantrip {

namespace {

QJsonObject timingToJson(const CanTimingValues& t) {
    QJsonObject obj;
    obj["brp"] = static_cast<int>(t.brp);
    obj["tseg1"] = static_cast<int>(t.tseg1);
    obj["tseg2"] = static_cast<int>(t.tseg2);
    obj["sjw"] = static_cast<int>(t.sjw);
    return obj;
}

CanTimingValues timingFromJson(const QJsonObject& obj) {
    CanTimingValues t;
    t.brp = static_cast<uint32_t>(obj["brp"].toInt());
    t.tseg1 = static_cast<uint32_t>(obj["tseg1"].toInt());
    t.tseg2 = static_cast<uint32_t>(obj["tseg2"].toInt());
    t.sjw = static_cast<uint32_t>(obj["sjw"].toInt());
    return t;
}

const char* lineStyleToString(GraphLineStyle style) {
    switch (style) {
        case GraphLineStyle::Dashed: return "Dashed";
        case GraphLineStyle::Dotted: return "Dotted";
        case GraphLineStyle::Scatter: return "Scatter";
        default: return "Solid";
    }
}

GraphLineStyle lineStyleFromString(const QString& s) {
    if (s == "Dashed") return GraphLineStyle::Dashed;
    if (s == "Dotted") return GraphLineStyle::Dotted;
    if (s == "Scatter") return GraphLineStyle::Scatter;
    return GraphLineStyle::Solid;
}

QJsonObject axisLayoutToJson(const GraphView::AxisLayout& axis) {
    QJsonObject obj;
    obj["name"] = axis.name;
    obj["autoScale"] = axis.autoScale;
    obj["min"] = axis.min;
    obj["max"] = axis.max;
    obj["hidden"] = axis.hidden;

    QJsonArray signalsArray;
    for (const auto& sig : axis.plottedSignals) {
        QJsonObject sigObj;
        sigObj["qualifiedName"] = sig.qualifiedName;
        sigObj["color"] = sig.color.name(QColor::HexRgb);
        sigObj["style"] = lineStyleToString(sig.style);
        signalsArray.append(sigObj);
    }
    obj["signals"] = signalsArray;
    return obj;
}

GraphView::AxisLayout axisLayoutFromJson(const QJsonObject& obj) {
    GraphView::AxisLayout axis;
    axis.name = obj["name"].toString();
    axis.autoScale = obj["autoScale"].toBool(true);
    axis.min = obj["min"].toDouble();
    axis.max = obj["max"].toDouble(100.0);
    axis.hidden = obj["hidden"].toBool(false);

    for (const QJsonValue& v : obj["signals"].toArray()) {
        const QJsonObject sigObj = v.toObject();
        GraphView::AxisLayoutSignal sig;
        sig.qualifiedName = sigObj["qualifiedName"].toString();
        sig.color = QColor(sigObj["color"].toString());
        sig.style = lineStyleFromString(sigObj["style"].toString());
        axis.plottedSignals.push_back(sig);
    }
    return axis;
}

QJsonObject transmitMessageToJson(const TransmitMessage& m) {
    QJsonObject obj;
    obj["id"] = static_cast<int>(m.id);
    obj["extended"] = m.extended;
    obj["fd"] = m.fd;
    obj["brs"] = m.brs;
    obj["rtr"] = m.rtr;
    obj["dlc"] = static_cast<int>(m.dlc);
    obj["data"] = QString::fromLatin1(m.data.toHex());
    obj["cycleTimeMs"] = m.cycleTimeMs;
    obj["paused"] = m.paused;
    obj["comment"] = m.comment;
    return obj;
}

TransmitMessage transmitMessageFromJson(const QJsonObject& obj) {
    TransmitMessage m;
    m.id = static_cast<uint32_t>(obj["id"].toInt());
    m.extended = obj["extended"].toBool();
    m.fd = obj["fd"].toBool();
    m.brs = obj["brs"].toBool();
    m.rtr = obj["rtr"].toBool();
    m.dlc = static_cast<uint8_t>(obj["dlc"].toInt());
    m.data = QByteArray::fromHex(obj["data"].toString().toLatin1());
    m.cycleTimeMs = obj["cycleTimeMs"].toInt();
    m.paused = obj["paused"].toBool();
    m.comment = obj["comment"].toString();
    return m;
}

} // namespace

bool saveRuneFile(const QString& path, const RuneConfig& config, QString* error) {
    QJsonObject root;
    root["channelDisplayName"] = config.channelDisplayName;

    QJsonObject busConfig;
    busConfig["nominalBitrateBps"] = static_cast<int>(config.busConfig.nominalBitrateBps);
    busConfig["dataBitrateBps"] = static_cast<int>(config.busConfig.dataBitrateBps);
    busConfig["fd"] = config.busConfig.fd;
    busConfig["nominalTiming"] = timingToJson(config.busConfig.nominalTiming);
    busConfig["dataTiming"] = timingToJson(config.busConfig.dataTiming);
    root["busConfig"] = busConfig;

    root["displayMode"] = config.displayMode == RuneDisplayMode::Periodic ? "Periodic" : "Waterfall";
    root["displayRateMs"] = config.displayRateMs;
    root["dbcPath"] = config.dbcPath;

    QJsonArray windowsArray;
    for (const auto& window : config.graphWindows) {
        QJsonArray axesArray;
        for (const auto& axis : window) axesArray.append(axisLayoutToJson(axis));
        QJsonObject windowObj;
        windowObj["axes"] = axesArray;
        windowsArray.append(windowObj);
    }
    QJsonObject graph;
    graph["windows"] = windowsArray;
    root["graph"] = graph;

    QJsonArray transmitArray;
    for (const auto& message : config.transmitMessages) transmitArray.append(transmitMessageToJson(message));
    root["transmitMessages"] = transmitArray;

    root["traceHeaderState"] = QString::fromLatin1(config.traceHeaderState.toHex());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = "Could not open file for writing:\n" + path;
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

std::optional<RuneConfig> loadRuneFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = "Could not open file:\n" + path;
        return std::nullopt;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "Not a valid .rune file:\n" + parseError.errorString();
        return std::nullopt;
    }

    const QJsonObject root = doc.object();
    RuneConfig config;
    config.channelDisplayName = root["channelDisplayName"].toString();

    const QJsonObject busConfig = root["busConfig"].toObject();
    config.busConfig.nominalBitrateBps = static_cast<uint32_t>(busConfig["nominalBitrateBps"].toInt(500000));
    config.busConfig.dataBitrateBps = static_cast<uint32_t>(busConfig["dataBitrateBps"].toInt(2000000));
    config.busConfig.fd = busConfig["fd"].toBool();
    config.busConfig.nominalTiming = timingFromJson(busConfig["nominalTiming"].toObject());
    config.busConfig.dataTiming = timingFromJson(busConfig["dataTiming"].toObject());

    config.displayMode = root["displayMode"].toString() == "Periodic" ? RuneDisplayMode::Periodic : RuneDisplayMode::Waterfall;
    config.displayRateMs = root["displayRateMs"].toInt(33);
    config.dbcPath = root["dbcPath"].toString();

    const QJsonObject graph = root["graph"].toObject();
    if (graph.contains("windows")) {
        for (const QJsonValue& windowVal : graph["windows"].toArray()) {
            std::vector<GraphView::AxisLayout> window;
            for (const QJsonValue& v : windowVal.toObject()["axes"].toArray()) {
                window.push_back(axisLayoutFromJson(v.toObject()));
            }
            config.graphWindows.push_back(std::move(window));
        }
    } else if (graph.contains("axes")) {
        // Pre-multi-window rune file: a single flat "axes" array directly
        // under "graph" - load it as one window rather than rejecting the
        // file or silently dropping the graph layout.
        std::vector<GraphView::AxisLayout> window;
        for (const QJsonValue& v : graph["axes"].toArray()) {
            window.push_back(axisLayoutFromJson(v.toObject()));
        }
        config.graphWindows.push_back(std::move(window));
    }

    // Absent entirely in a rune saved before this field existed -
    // QJsonObject::operator[] on a missing key yields an empty array, no
    // explicit version field needed (same convention as graph/windows above).
    for (const QJsonValue& v : root["transmitMessages"].toArray()) {
        config.transmitMessages.push_back(transmitMessageFromJson(v.toObject()));
    }

    config.traceHeaderState = QByteArray::fromHex(root["traceHeaderState"].toString().toLatin1());

    return config;
}

} // namespace cantrip
