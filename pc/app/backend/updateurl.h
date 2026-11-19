#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

// Only HTTPS links into the project's own repository (GitHub or its GitLab mirror) may be opened from the update manifest.
// Dot segments (plain or percent-encoded) and encoded separators are rejected, the path is
// normalised, and the prefix test is applied to the normalised path.
inline bool normalizeTrustedUpdateUrl(const QString& urlString, QString* normalized = nullptr)
{
    QUrl url(urlString, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        return false;
    }
    if ((url.host() != QLatin1String("github.com") && url.host() != QLatin1String("gitlab.com")) ||
            url.port() != -1 || !url.userInfo().isEmpty()) {
        return false;
    }

    const QString rawPath = url.path(QUrl::FullyEncoded);
    const QString lowerPath = rawPath.toLower();
    if (lowerPath.contains(QLatin1String("%2e")) ||
            lowerPath.contains(QLatin1String("%2f")) ||
            lowerPath.contains(QLatin1String("%5c")) ||
            lowerPath.contains(QLatin1String("%00")) ||
            rawPath.contains(QLatin1Char('\\')) ||
            rawPath.contains(QLatin1String("//"))) {
        return false;
    }
    const QStringList segments = rawPath.split(QLatin1Char('/'));
    for (const QString& segment : segments) {
        if (segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return false;
        }
    }

    url = url.adjusted(QUrl::NormalizePathSegments);
    const QString path = url.path();
    if (path != QLatin1String("/siberanka/NyxPlay") &&
            !path.startsWith(QLatin1String("/siberanka/NyxPlay/"))) {
        return false;
    }

    if (normalized != nullptr) {
        *normalized = url.toString(QUrl::FullyEncoded);
    }
    return true;
}
