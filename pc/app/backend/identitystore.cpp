#include "identitystore.h"

#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>

namespace {
    // Application-specific entropy mixed into the encryption
    const char kEntropy[] = "NyxPlay client identity v1";

    QByteArray runCrypt(const QByteArray& input, bool encrypt)
    {
        if (input.isEmpty()) {
            return QByteArray();
        }

        DATA_BLOB in;
        in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(input.constData()));
        in.cbData = static_cast<DWORD>(input.size());

        DATA_BLOB entropy;
        entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
        entropy.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);

        DATA_BLOB out = {};
        BOOL ok;
        if (encrypt) {
            ok = CryptProtectData(&in, L"NyxPlay", &entropy, nullptr, nullptr,
                                  CRYPTPROTECT_UI_FORBIDDEN, &out);
        }
        else {
            ok = CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN, &out);
        }
        if (!ok) {
            return QByteArray();
        }

        QByteArray result(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return result;
    }
}
#endif

QByteArray IdentityStore::protect(const QByteArray& plain)
{
#ifdef Q_OS_WIN
    return runCrypt(plain, true);
#else
    Q_UNUSED(plain);
    return QByteArray();
#endif
}

QByteArray IdentityStore::unprotect(const QByteArray& blob)
{
#ifdef Q_OS_WIN
    return runCrypt(blob, false);
#else
    Q_UNUSED(blob);
    return QByteArray();
#endif
}

bool IdentityStore::save(QSettings& settings, const QByteArray& pem)
{
#ifdef Q_OS_WIN
    QByteArray blob = protect(pem);
    if (!blob.isEmpty()) {
        settings.setValue(KEY_PROTECTED, blob);
        settings.remove(KEY_PLAIN);
        settings.sync();
        return true;
    }

    // Encryption is unavailable; keep the identity usable rather than lose it
    qWarning() << "Unable to protect the client identity, storing it unprotected";
    settings.setValue(KEY_PLAIN, pem);
    return false;
#else
    settings.setValue(KEY_PLAIN, pem);
    return true;
#endif
}

IdentityStore::Result IdentityStore::load(QSettings& settings)
{
    Result result;

#ifdef Q_OS_WIN
    QByteArray blob = settings.value(KEY_PROTECTED).toByteArray();
    if (!blob.isEmpty()) {
        result.key = unprotect(blob);
        if (result.key.isEmpty()) {
            result.status = Status::Undecryptable;
            return result;
        }

        // A protected value wins; drop any stale plain copy
        if (settings.contains(KEY_PLAIN)) {
            settings.remove(KEY_PLAIN);
        }
        result.status = Status::Loaded;
        return result;
    }
#endif

    QByteArray plain = settings.value(KEY_PLAIN).toByteArray();
    if (plain.isEmpty()) {
        return result;
    }

    result.key = plain;
    result.status = Status::Loaded;
#ifdef Q_OS_WIN
    if (save(settings, plain)) {
        qInfo() << "Moved the client identity to protected storage";
        result.status = Status::Migrated;
    }
#endif
    return result;
}

int IdentityStore::clearHostPairings(QSettings& settings)
{
    static const char* const arrays[] = { "hosts", "hostsbackup" };
    int changed = 0;

    for (const char* name : arrays) {
        int count = settings.beginReadArray(name);
        for (int i = 0; i < count; i++) {
            settings.setArrayIndex(i);
            if (!settings.value("srvcert").toByteArray().isEmpty()) {
                settings.setValue("srvcert", QByteArray());
                if (name[5] == '\0') { // only count the primary list
                    changed++;
                }
            }
        }
        settings.endArray();
    }

    return changed;
}

void IdentityStore::setStartupNotice(QSettings& settings, const QString& code)
{
    settings.setValue(KEY_NOTICE, code);
}

QString IdentityStore::takeStartupNotice(QSettings& settings)
{
    QString code = settings.value(KEY_NOTICE).toString();
    if (!code.isEmpty()) {
        settings.remove(KEY_NOTICE);
    }
    return code;
}
