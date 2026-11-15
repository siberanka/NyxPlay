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
        Undecryptable   // protected value exists but cannot be decrypted here
    };

    struct Result {
        Status status = Status::Missing;
        QByteArray key;
    };

    // Returns an empty array on failure or on platforms without protection
    static QByteArray protect(const QByteArray& plain);
    static QByteArray unprotect(const QByteArray& blob);

    static Result load(QSettings& settings);

    // Stores the key (protected where supported). Returns false if the key
    // could only be stored in plain form or not at all.
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
