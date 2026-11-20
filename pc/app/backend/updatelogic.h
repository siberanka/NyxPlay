#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>
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

// Largest portable package the client will download.
constexpr qint64 MaxPackageBytes = 300LL * 1024 * 1024;

// Everything needed to install an update in place. Only filled when the manifest entry carries
// valid "tag", "asset_url" and "asset_size" fields; older clients ignore these fields.
struct Package {
    QString tag;        // "v2026.11.19.1"
    QString assetName;  // "NyxPlayPC-2026.11.19.1-windows-x64-portable.zip"
    QUrl assetUrl;
    qint64 assetSize = 0;
    bool valid() const { return !assetName.isEmpty(); }
};

struct Result {
    Status status = Status::Malformed;
    QString version;
    QString url;
    Package package;
};

// Package architecture label used in the asset name.
inline QString packageArch(const QString& cpuArch)
{
    if (cpuArch == QLatin1String("x86_64")) {
        return QStringLiteral("x64");
    }
    if (cpuArch == QLatin1String("arm64")) {
        return QStringLiteral("arm64");
    }
    return QString();
}

// Name of the portable package of a release.
inline QString packageAssetName(const QString& version, const QString& cpuArch)
{
    const QString arch = packageArch(cpuArch);
    return arch.isEmpty() ? QString()
                          : QStringLiteral("NyxPlayPC-%1-windows-%2-portable.zip").arg(version, arch);
}

// Accepts only the two release asset locations of the project: GitHub
// ".../releases/download/<tag>/<name>" and the GitLab mirror ".../-/releases/<tag>/downloads/<name>".
inline bool isTrustedAssetUrl(const QUrl& url, const QString& tag, const QString& assetName)
{
    QString normalized;
    if (!normalizeTrustedUpdateUrl(url.toString(QUrl::FullyEncoded), &normalized)) {
        return false;
    }
    if (!url.query().isEmpty() || url.hasFragment()) {
        return false;
    }
    const QString path = QUrl(normalized).path();
    const QString github = QStringLiteral("/siberanka/NyxPlay/releases/download/%1/%2").arg(tag, assetName);
    const QString gitlab = QStringLiteral("/siberanka/NyxPlay/-/releases/%1/downloads/%2").arg(tag, assetName);
    return (url.host() == QLatin1String("github.com") && path == github) ||
           (url.host() == QLatin1String("gitlab.com") && path == gitlab);
}

// Location of a sibling file of the same release (SHA256SUMS.txt, SHA256SUMS.txt.sig).
inline QUrl releaseFileUrl(const Package& package, const QString& fileName)
{
    QUrl url = package.assetUrl;
    QString path = url.path();
    path.truncate(path.lastIndexOf(QLatin1Char('/')) + 1);
    url.setPath(path + fileName);
    return url;
}

// Validates the install fields of a manifest entry; an invalid or absent set only disables
// the in-place install, the notice and the release page link keep working.
inline Package parsePackage(const QJsonObject& obj, const QString& version, const QString& cpuArch)
{
    Package package;
    const QJsonValue tagValue = obj.value(QLatin1String("tag"));
    const QJsonValue urlValue = obj.value(QLatin1String("asset_url"));
    const QJsonValue sizeValue = obj.value(QLatin1String("asset_size"));
    if (!tagValue.isString() || !urlValue.isString() || !sizeValue.isDouble()) {
        return package;
    }

    const QString trimmedVersion = version.trimmed();
    const QString tag = tagValue.toString();
    const QString name = packageAssetName(trimmedVersion, cpuArch);
    const double size = sizeValue.toDouble();
    if (name.isEmpty() || tag != QLatin1Char('v') + trimmedVersion ||
            !(size >= 1 && size <= double(MaxPackageBytes)) || size != double(qint64(size))) {
        return package;
    }

    QUrl url(urlValue.toString(), QUrl::StrictMode);
    if (!url.isValid() || !isTrustedAssetUrl(url, tag, name)) {
        return package;
    }

    package.tag = tag;
    package.assetName = name;
    package.assetUrl = url;
    package.assetSize = qint64(size);
    return package;
}

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
        if (platform == QLatin1String("windows")) {
            result.package = parsePackage(obj, result.version, arch);
        }
        return result;
    }

    return result;
}

} // namespace UpdateLogic
