#include "LogFilenameTemplate.h"

#include <QDateTime>

namespace cantrip {

namespace {

// Filenames built from this should contain only letters, digits, and '_'
// as a separator - every run of anything else (spaces, punctuation,
// parentheses - channel display names like "CANtrip synthetic test source
// (no hardware needed)" have plenty) collapses to a single underscore.
QString sanitizeForFilename(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) {
            out += c;
        } else if (!out.isEmpty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.isEmpty() && out.back() == '_') out.chop(1);
    return out;
}

// Channel display names can be long (vendor + device + port, or the
// synthetic source's descriptive name) - cap how much of that lands in the
// filename so [bus] doesn't dominate the whole name.
constexpr int kMaxBusNameLength = 20;

} // namespace

QString expandLogFilenameTemplate(const QString& tmpl, const QString& busName) {
    const QDateTime now = QDateTime::currentDateTime();

    QString result = tmpl;
    result.replace("[date]", now.toString("yyyy-MM-dd"));
    result.replace("[time]", now.toString("HHmmss"));
    result.replace("[user]", sanitizeForFilename(qEnvironmentVariable("USERNAME")));
    result.replace("[bus]", sanitizeForFilename(busName).left(kMaxBusNameLength));
    return result;
}

} // namespace cantrip
