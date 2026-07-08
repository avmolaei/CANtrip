// Expands a bracketed-token filename template (e.g. "[user]_[bus]_[date]")
// into a real filename, for the Logging tab's "Output file..." default
// name and LoggingOptionsDialog's template editor.
#pragma once

#include <QString>

namespace cantrip {

// Recognized tokens: [date] (yyyy-MM-dd), [time] (HHmmss), [user] (the
// Windows account name), [bus] (busName, sanitized for filesystem safety).
// Unrecognized bracketed text is left as-is rather than treated as an
// error, so a typo just shows up literally in the filename instead of
// silently dropping the whole log.
QString expandLogFilenameTemplate(const QString& tmpl, const QString& busName);

} // namespace cantrip
