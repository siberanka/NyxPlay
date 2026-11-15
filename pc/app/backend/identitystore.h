#pragma once

#include <QByteArray>
#include <QSettings>
#include <QString>

// Persistence of the client private key. On Windows the key is stored
// encrypted for the current user (DPAPI) and the legacy plain value is
// migrated on first load. Other platforms keep the plain value.
class IdentityStore
{
public:
    enum class Status {
        Missing,        // nothing stored
        Loaded,         // key read as stored
        Migrated,       // plain key was re-saved in protected form
        Undecryptable,  // protected value exists and can never be decrypted by this user
        Unavailable     // protected value exists but could not be read right now; it is kept as is
    };

    struct Result {
        Status status = Status::Missing;
        QByteArray key;
    };

    // Returns an empty array on failure or on platforms without protection.
    // When unprotect() fails, *permanent (if given) tells whether the value is definitely
    // not decryptable for this user (damaged data or another account's key) as opposed to
    // a failure that may go away (profile not ready, out of resources, service unavailable).
    static QByteArray protect(const QByteArray& plain);
    static QByteArray unprotect(const QByteArray& blob, bool* permanent = nullptr);

    // Classifies a Windows error code reported by the decryption call
    static bool isPermanentDecryptError(unsigned long errorCode);

    static Result load(QSettings& settings);

    // Stores the key (protected where supported). A key that cannot be protected is never
    // written in plain form; false means nothing was stored.
    static bool save(QSettings& settings, const QByteArray& pem);

    // Forgets the pinned server certificate of every saved host so that the
    // hosts are treated as unpaired. Returns the number of hosts changed.
    static int clearHostPairings(QSettings& settings);

    // One-shot message for the UI ("import", "repair")
    static void setStartupNotice(QSettings& settings, const QString& code);
    static QString takeStartupNotice(QSettings& settings);

    static constexpr const char* KEY_PLAIN = "key";
    static constexpr const char* KEY_PROTECTED = "keyprotected";
    static constexpr const char* KEY_NOTICE = "startupnotice";
};
