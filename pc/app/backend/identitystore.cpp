#include "identitystore.h"

#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>

namespace {
    // Application-specific entropy mixed into the encryption
    const char kEntropy[] = "NyxPlay client identity v1";

    QByteArray runCrypt(const QByteArray& input, bool encrypt, DWORD* errorCode = nullptr)
    {
        if (errorCode != nullptr) {
            *errorCode = 0;
        }
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
            if (errorCode != nullptr) {
                *errorCode = GetLastError();
            }
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

bool IdentityStore::isPermanentDecryptError(unsigned long errorCode)
{
    switch (errorCode) {
    case 13:          // ERROR_INVALID_DATA: the blob is damaged
    case 87:          // ERROR_INVALID_PARAMETER: the blob is not a protected value
    case 0x80090003:  // NTE_BAD_KEY
    case 0x80090004:  // NTE_BAD_LEN
    case 0x80090005:  // NTE_BAD_DATA
    case 0x80090006:  // NTE_BAD_SIGNATURE: integrity check failed
    case 0x80090007:  // NTE_BAD_VER
    case 0x8009000B:  // NTE_BAD_KEY_STATE: the master key belongs to another account
    case 0x8009000D:  // NTE_NO_KEY
        return true;
    default:
        return false;
    }
}

QByteArray IdentityStore::unprotect(const QByteArray& blob, bool* permanent)
{
    if (permanent != nullptr) {
        *permanent = false;
    }
#ifdef Q_OS_WIN
    DWORD errorCode = 0;
    QByteArray result = runCrypt(blob, false, &errorCode);
    if (result.isEmpty() && permanent != nullptr) {
        *permanent = isPermanentDecryptError(errorCode);
        if (!*permanent) {
            qWarning() << "Decrypting the client identity failed with a possibly temporary error" << errorCode;
        }
    }
    return result;
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

    // Encryption is unavailable: the key is never written in plain form
    qWarning() << "Unable to protect the client identity, it was not saved";
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
        bool permanent = false;
        result.key = unprotect(blob, &permanent);
        if (result.key.isEmpty()) {
            // Only a value that can never be decrypted here is reported as such; for any other
            // failure the stored value is left untouched so a later start can read it.
            result.status = permanent ? Status::Undecryptable : Status::Unavailable;
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
