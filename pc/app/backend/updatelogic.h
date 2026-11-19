#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "updateurl.h"

// Pure update manifest logic (no I/O) so that it can be unit tested.
namespace UpdateLogic {

// Upper bound for the downloaded manifest body.
constexpr qint64 MaxManifestBytes = 256 * 1024;

// Upper bound for the number of manifest entries that are inspected.
constexpr int MaxManifestEntries = 64;

inline QString fallbackReleasesUrl()
{
    return QStringLiteral("https://github.com/siberanka/NyxPlay/releases");
}

// Parses "2026.11.19.1" style versions. In strict mode every component must be a plain
// decimal number (1 to 9 digits); in lenient mode (kernel versions such as "6.5.0-generic")
// the leading digits of each component are used. Returns false on unusable input.
inline bool parseVersion(const QString& string, QVector<int>& version, bool lenient = false)
{
    version.clear();
    const QString trimmed = string.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 64) {
        return false;
    }

    const QStringList list = trimmed.split(QLatin1Char('.'));
    if (list.size() > 8) {
        return false;
    }

    for (const QString& component : list) {
        QString digits;
        for (const QChar ch : component) {
            if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) {
                digits.append(ch);
            }
            else if (lenient) {
                break;
            }
            else {
                version.clear();
                return false;
            }
        }

        if (digits.isEmpty() || digits.size() > 9) {
            if (!lenient || !digits.isEmpty()) {
                version.clear();
                return false;
            }
            digits = QStringLiteral("0");
        }
        version.append(digits.toInt());
    }

    return !version.isEmpty();
}

// Missing trailing components count as 0. Returns -1, 0 or 1.
inline int compareVersions(const QVector<int>& version1, const QVector<int>& version2)
{
    const int count = qMax(version1.count(), version2.count());
    for (int i = 0; i < count; i++) {
        const int v1Val = i < version1.count() ? version1[i] : 0;
        const int v2Val = i < version2.count() ? version2[i] : 0;
        if (v1Val < v2Val) {
            return -1;
        }
        if (v1Val > v2Val) {
            return 1;
        }
    }
    return 0;
}

enum class Status {
    Malformed,       // not JSON, not an array, empty, or too large
    NoMatchingEntry, // valid manifest without a usable entry for this platform
    UpToDate,        // the matching entry is not newer than the running version
    UpdateAvailable
};

struct Result {
    Status status = Status::Malformed;
    QString version;
    QString url;
};

// A failed or unusable primary manifest should be retried against the mirror.
inline bool shouldTryFallback(Status status)
{
    return status == Status::Malformed || status == Status::NoMatchingEntry;
}

inline Result evaluateManifest(const QByteArray& body,
                               const QString& currentVersion,
                               const QString& arch,
                               const QString& platform,
                               const QString& kernelVersion)
{
    Result result;

    QVector<int> current;
    if (body.isEmpty() || body.size() > MaxManifestBytes || !parseVersion(currentVersion, current)) {
        return result;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        return result;
    }

    const QJsonArray array = doc.array();
    result.status = Status::NoMatchingEntry;

    int inspected = 0;
    for (const QJsonValue& entry : array) {
        if (++inspected > MaxManifestEntries) {
            break;
        }
        if (!entry.isObject()) {
            continue;
        }

        const QJsonObject obj = entry.toObject();
        const QJsonValue platformValue = obj.value(QLatin1String("platform"));
        const QJsonValue archValue = obj.value(QLatin1String("arch"));
        const QJsonValue versionValue = obj.value(QLatin1String("version"));
        const QJsonValue urlValue = obj.value(QLatin1String("browser_url"));
        if (!platformValue.isString() || !archValue.isString() ||
                !versionValue.isString() || !urlValue.isString()) {
            continue;
        }

        if (archValue.toString() != arch || platformValue.toString() != platform) {
            continue;
        }

        const QJsonValue kernelValue = obj.value(QLatin1String("kernel_version_at_least"));
        if (kernelValue.isString()) {
            QVector<int> required;
            QVector<int> actual;
            if (parseVersion(kernelValue.toString(), required, true) &&
                    parseVersion(kernelVersion, actual, true) &&
                    compareVersions(actual, required) < 0) {
                continue;
            }
        }

        QVector<int> latest;
        if (!parseVersion(versionValue.toString(), latest)) {
            continue;
        }

        if (compareVersions(current, latest) >= 0) {
            result.status = Status::UpToDate;
            return result;
        }

        QString trustedUrl;
        if (!normalizeTrustedUpdateUrl(urlValue.toString(), &trustedUrl)) {
            trustedUrl = fallbackReleasesUrl();
        }

        result.status = Status::UpdateAvailable;
        result.version = versionValue.toString().trimmed();
        result.url = trustedUrl;
        return result;
    }

    return result;
}

} // namespace UpdateLogic
