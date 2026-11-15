#include "moonlightimport.h"
#include "identitystore.h"
#include "nvcomputer.h"

#include <QDebug>
#include <QSet>

#define SER_HOSTS "hosts"
#define MAX_IMPORTED_HOSTS 100

QList<QVariantMap> QSettingsImportSource::hosts() const
{
    QList<QVariantMap> result;

    int count = m_Settings.beginReadArray(SER_HOSTS);
    for (int i = 0; i < count; i++) {
        m_Settings.setArrayIndex(i);
        QVariantMap record;
        const QStringList keys = m_Settings.childKeys();
        for (const QString& key : keys) {
            record.insert(key, m_Settings.value(key));
        }
        result.append(record);
    }
    m_Settings.endArray();

    return result;
}

QVariantMap QSettingsImportSource::preferences() const
{
    QVariantMap result;
    const QStringList keys = m_Settings.childKeys();
    for (const QString& key : keys) {
        result.insert(key, m_Settings.value(key));
    }
    return result;
}

namespace {
    bool isPlainText(const QString& s, int maxLen)
    {
        if (s.isEmpty() || s.size() > maxLen) {
            return false;
        }
        for (QChar c : s) {
            if (c.unicode() < 0x20 || c.unicode() == 0x7F) {
                return false;
            }
        }
        return true;
    }

    bool isAddress(const QString& s)
    {
        if (!isPlainText(s, 255)) {
            return false;
        }
        for (QChar c : s) {
            if (c.isSpace() || c == '/' || c == '\\' || c == '@' || c == '?' || c == '#') {
                return false;
            }
        }
        return true;
    }

    bool toIntInRange(const QVariant& v, int min, int max, int* out)
    {
        bool ok = false;
        int value = v.toString().toInt(&ok);
        if (!ok || value < min || value > max) {
            return false;
        }
        *out = value;
        return true;
    }

    bool toBool(const QVariant& v, bool* out)
    {
        if (v.metaType().id() == QMetaType::Bool) {
            *out = v.toBool();
            return true;
        }
        QString s = v.toString().toLower();
        if (s == "true" || s == "1") {
            *out = true;
            return true;
        }
        if (s == "false" || s == "0") {
            *out = false;
            return true;
        }
        return false;
    }

    struct IntPref { const char* key; int min; int max; };

    const IntPref kIntPrefs[] = {
        { "width", 320, 16384 },
        { "height", 240, 16384 },
        { "fps", 1, 1000 },
        { "bitrate", 500, 1000000 },
        { "packetsize", 0, 65535 },
        { "audiocfg", 0, 2 },
        { "videocfg", 0, 4 },
        { "videodec", 0, 2 },
        { "windowmode", 0, 2 },
        { "uidisplaymode", 0, 2 },
        { "capturesyskeys", 0, 2 },
    };

    const char* const kBoolPrefs[] = {
        "unlockbitrate", "autoadjustbitrate", "vsync", "gameopts",
        "hostaudio", "multicontroller", "hdr", "yuv444", "mdns",
        "quitAppAfter", "mouseacceleration", "abstouchmode", "framepacing",
        "connwarnings", "confwarnings", "richpresence", "gamepadmouse",
        "detectnetblocking", "showperfoverlay", "swapmousebuttons",
        "muteonfocusloss", "backgroundgamepad", "reversescroll",
        "swapfacebuttons", "keepawake",
    };

    const char* const kPortKeys[] = {
        "localport", "remoteport", "ipv6port", "manualport"
    };
    const char* const kAddressKeys[] = {
        "localaddress", "remoteaddress", "ipv6address", "manualaddress"
    };
}

QVariantMap MoonlightImport::sanitizeHost(const QVariantMap& raw)
{
    QVariantMap out;

    // The uuid is used to build cache paths, so it must pass the same check
    // as for hosts that are loaded normally
    QString uuid = raw.value("uuid").toString();
    if (!NvComputer::isValidUuid(uuid)) {
        return out;
    }
    out.insert("uuid", uuid);

    QString name = raw.value("hostname").toString();
    bool nameOk = isPlainText(name, 255);
    out.insert("hostname", nameOk ? name : QString());
    bool custom;
    if (nameOk && toBool(raw.value("customname"), &custom) && custom) {
        out.insert("customname", true);
    }

    QByteArray mac = raw.value("mac").toByteArray();
    if (mac.size() == 6) {
        out.insert("mac", mac);
    }

    bool hasAddress = false;
    for (const char* key : kAddressKeys) {
        QString address = raw.value(key).toString();
        if (isAddress(address)) {
            out.insert(key, address);
            hasAddress = true;
        }
    }

    // A host needs at least one address to be reachable
    if (!hasAddress) {
        return QVariantMap();
    }

    for (const char* key : kPortKeys) {
        int port;
        if (raw.contains(key) && toIntInRange(raw.value(key), 1, 65535, &port)) {
            out.insert(key, port);
        }
    }

    bool nvidia;
    if (toBool(raw.value("nvidiasw"), &nvidia)) {
        out.insert("nvidiasw", nvidia);
    }

    return out;
}

QVariantMap MoonlightImport::sanitizePreferences(const QVariantMap& raw)
{
    QVariantMap out;

    for (const IntPref& pref : kIntPrefs) {
        int value;
        if (raw.contains(pref.key) && toIntInRange(raw.value(pref.key), pref.min, pref.max, &value)) {
            out.insert(pref.key, value);
        }
    }

    for (const char* key : kBoolPrefs) {
        bool value;
        if (raw.contains(key) && toBool(raw.value(key), &value)) {
            out.insert(key, value);
        }
    }

    return out;
}

bool MoonlightImport::isNeeded(QSettings& dest)
{
    if (dest.value(KEY_DONE).toBool()) {
        return false;
    }

    int hosts = dest.beginReadArray(SER_HOSTS);
    dest.endArray();
    return hosts == 0;
}

MoonlightImport::Result MoonlightImport::run(QSettings& dest, const ImportSource& source)
{
    Result result;

    // Mark first so that an interrupted import is not retried on every start
    dest.setValue(KEY_DONE, true);

    QList<QVariantMap> accepted;
    QSet<QString> seen;
    const QList<QVariantMap> hosts = source.hosts();
    for (const QVariantMap& raw : hosts) {
        QVariantMap host = sanitizeHost(raw);
        QString uuid = host.value("uuid").toString();
        if (host.isEmpty() || seen.contains(uuid) || accepted.size() >= MAX_IMPORTED_HOSTS) {
            result.hostsSkipped++;
            continue;
        }
        seen.insert(uuid);
        accepted.append(host);
    }

    if (!accepted.isEmpty()) {
        dest.beginWriteArray(SER_HOSTS, accepted.size());
        for (int i = 0; i < accepted.size(); i++) {
            dest.setArrayIndex(i);
            const QVariantMap& host = accepted.at(i);
            for (auto it = host.constBegin(); it != host.constEnd(); ++it) {
                dest.setValue(it.key(), it.value());
            }
        }
        dest.endArray();
        result.hostsImported = accepted.size();
    }

    const QVariantMap prefs = sanitizePreferences(source.preferences());
    for (auto it = prefs.constBegin(); it != prefs.constEnd(); ++it) {
        if (!dest.contains(it.key())) {
            dest.setValue(it.key(), it.value());
            result.preferencesImported++;
        }
    }

    dest.sync();
    return result;
}

void MoonlightImport::runAtStartup()
{
#ifdef Q_OS_WIN
    QSettings dest;
    if (!isNeeded(dest)) {
        return;
    }

    // Opening a missing registry key for reading does not create it
    QSettings other(QSettings::NativeFormat, QSettings::UserScope,
                    "Moonlight Game Streaming Project", "Moonlight");
    if (other.childKeys().isEmpty() && other.childGroups().isEmpty()) {
        dest.setValue(KEY_DONE, true);
        return;
    }

    QSettingsImportSource source(other);
    Result result = run(dest, source);
    qInfo() << "Imported" << result.hostsImported << "host(s) and"
            << result.preferencesImported << "setting(s) from an existing installation;"
            << result.hostsSkipped << "entries skipped";

    if (result.hostsImported > 0) {
        IdentityStore::setStartupNotice(dest, "import");
    }
#endif
}
