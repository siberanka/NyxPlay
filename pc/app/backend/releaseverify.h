#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

// Verification of signed release checksums (SHA256SUMS.txt plus a detached Ed25519 signature).
// Nothing in here throws or blocks on the network.
namespace ReleaseVerify {

enum class Result {
    Ok,             // everything checked out
    BadSignature,   // well formed signature that no trusted key accepts
    Malformed,      // signature text or checksum list not in the strict format
    NotListed,      // the file name has no entry in the checksum list
    HashMismatch,   // the file does not match the signed hash
    IoError         // the file could not be read
};

constexpr int SignatureSize = 64;
constexpr int PublicKeySize = 32;
constexpr qsizetype MaxSumsSize = 64 * 1024;

struct SumsEntry {
    QString name;
    QString sha256Hex;   // 64 lowercase hex characters
};

// Short stable name of a result, for logs.
const char* resultName(Result result);

// The release signing keys embedded in this build (raw 32 byte keys). The list lets a
// new key ship one release before it is used.
QList<QByteArray> trustedKeys();

// Lowercase hex SHA-256 of a raw public key. Safe to log.
QString keyFingerprint(const QByteArray& rawKey);

// Strict Base64: 88 characters ending in "==", canonical, 64 bytes. One trailing LF is accepted.
bool decodeSignature(const QByteArray& text, QByteArray* out);

// Verifies the signature over the exact bytes of the checksum list.
Result verifySignature(const QByteArray& sums, const QByteArray& signatureBase64, const QList<QByteArray>& keys);
Result verifySignature(const QByteArray& sums, const QByteArray& signatureBase64);

// Lines of "<64 lowercase hex><two spaces><name>", names only [A-Za-z0-9._-] (not "." or ".."),
// no duplicate names.
Result parseSums(const QByteArray& sums, QVector<SumsEntry>* entries);

// Exact name lookup. NotListed when there is no entry.
Result findHash(const QByteArray& sums, const QString& fileName, QString* sha256Hex);

// Streaming SHA-256 of a local file as lowercase hex.
Result hashFile(const QString& path, QString* sha256Hex);

bool constantTimeEqual(const QByteArray& a, const QByteArray& b);

// Checks a local file against its entry in the (already signature checked) checksum list.
Result verifyFile(const QByteArray& sums, const QString& fileName, const QString& path);

}
