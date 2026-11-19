#include "releaseverify.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSet>

#include <openssl/evp.h>

#include <memory>

namespace ReleaseVerify {

namespace {

// SHA-256 fingerprint of this key:
// db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1
const unsigned char kEmbeddedKey[PublicKeySize] = {
    0x94, 0x2b, 0xa3, 0x46, 0x02, 0x0e, 0xd3, 0x3f, 0xcf, 0x76, 0xfe, 0xb1, 0x70, 0xe0, 0x56, 0xc1,
    0x6a, 0xf2, 0xa2, 0x86, 0x4b, 0xff, 0xaa, 0x6f, 0xd6, 0xff, 0x42, 0x24, 0xaf, 0x8e, 0x93, 0x26
};

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool isNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

bool isLowerHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool verifyOne(const QByteArray& key, const QByteArray& sums, const QByteArray& sig)
{
    if (key.size() != PublicKeySize) {
        return false;
    }
    std::unique_ptr<EVP_PKEY, PkeyDeleter> pkey(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(key.constData()), key.size()));
    if (!pkey) {
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(ctx.get(),
                            reinterpret_cast<const unsigned char*>(sig.constData()), sig.size(),
                            reinterpret_cast<const unsigned char*>(sums.constData()), sums.size()) == 1;
}

}

const char* resultName(Result result)
{
    switch (result) {
    case Result::Ok: return "ok";
    case Result::BadSignature: return "bad signature";
    case Result::Malformed: return "malformed";
    case Result::NotListed: return "not listed";
    case Result::HashMismatch: return "hash mismatch";
    case Result::IoError: return "I/O error";
    }
    return "unknown";
}

QList<QByteArray> trustedKeys()
{
    return { QByteArray(reinterpret_cast<const char*>(kEmbeddedKey), PublicKeySize) };
}

QString keyFingerprint(const QByteArray& rawKey)
{
    return QString::fromLatin1(QCryptographicHash::hash(rawKey, QCryptographicHash::Sha256).toHex());
}

bool decodeSignature(const QByteArray& textIn, QByteArray* out)
{
    QByteArray text = textIn;
    if (text.endsWith('\n')) {
        text.chop(1);
    }
    // 64 bytes = 21 groups of 4 characters + one group with two characters and "=="
    if (text.size() != 88 || text[86] != '=' || text[87] != '=') {
        return false;
    }
    QByteArray result;
    result.reserve(SignatureSize);
    for (int group = 0; group < 22; group++) {
        int v[4] = { 0, 0, 0, 0 };
        const int chars = group == 21 ? 2 : 4;
        for (int i = 0; i < chars; i++) {
            v[i] = base64Value(text[group * 4 + i]);
            if (v[i] < 0) {
                return false;
            }
        }
        result.append(static_cast<char>((v[0] << 2) | (v[1] >> 4)));
        if (group == 21) {
            // the unused low four bits of the last character must be zero
            if ((v[1] & 0x0F) != 0) {
                return false;
            }
        }
        else {
            result.append(static_cast<char>(((v[1] & 0x0F) << 4) | (v[2] >> 2)));
            result.append(static_cast<char>(((v[2] & 0x03) << 6) | v[3]));
        }
    }
    if (result.size() != SignatureSize) {
        return false;
    }
    if (out) {
        *out = result;
    }
    return true;
}

Result verifySignature(const QByteArray& sums, const QByteArray& signatureBase64, const QList<QByteArray>& keys)
{
    QByteArray sig;
    if (sums.size() > MaxSumsSize || !decodeSignature(signatureBase64, &sig)) {
        return Result::Malformed;
    }
    for (const QByteArray& key : keys) {
        if (verifyOne(key, sums, sig)) {
            return Result::Ok;
        }
    }
    return Result::BadSignature;
}

Result verifySignature(const QByteArray& sums, const QByteArray& signatureBase64)
{
    return verifySignature(sums, signatureBase64, trustedKeys());
}

Result parseSums(const QByteArray& sums, QVector<SumsEntry>* entries)
{
    QVector<SumsEntry> parsed;
    QSet<QString> seen;
    if (entries) {
        entries->clear();
    }
    if (sums.isEmpty() || sums.size() > MaxSumsSize) {
        return Result::Malformed;
    }
    qsizetype pos = 0;
    while (pos < sums.size()) {
        const qsizetype end = sums.indexOf('\n', pos);
        const QByteArray line = sums.mid(pos, end < 0 ? -1 : end - pos);
        pos = end < 0 ? sums.size() : end + 1;

        // 64 hex + two spaces + at least one name character
        if (line.size() < 67 || line.size() > 66 + 255 || line[64] != ' ' || line[65] != ' ') {
            return Result::Malformed;
        }
        for (int i = 0; i < 64; i++) {
            if (!isLowerHex(line[i])) {
                return Result::Malformed;
            }
        }
        const QByteArray name = line.mid(66);
        for (char c : name) {
            if (!isNameChar(c)) {
                return Result::Malformed;
            }
        }
        if (name == "." || name == "..") {
            return Result::Malformed;
        }
        const QString nameString = QString::fromLatin1(name);
        if (seen.contains(nameString)) {
            return Result::Malformed;
        }
        seen.insert(nameString);
        parsed.append({ nameString, QString::fromLatin1(line.left(64)) });
    }
    if (parsed.isEmpty()) {
        return Result::Malformed;
    }
    if (entries) {
        *entries = parsed;
    }
    return Result::Ok;
}

Result findHash(const QByteArray& sums, const QString& fileName, QString* sha256Hex)
{
    if (sha256Hex) {
        sha256Hex->clear();
    }
    QVector<SumsEntry> entries;
    Result parsed = parseSums(sums, &entries);
    if (parsed != Result::Ok) {
        return parsed;
    }
    for (const SumsEntry& entry : entries) {
        if (entry.name == fileName) {
            if (sha256Hex) {
                *sha256Hex = entry.sha256Hex;
            }
            return Result::Ok;
        }
    }
    return Result::NotListed;
}

Result hashFile(const QString& path, QString* sha256Hex)
{
    if (sha256Hex) {
        sha256Hex->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result::IoError;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 got = file.read(buffer.data(), buffer.size());
        if (got < 0) {
            return Result::IoError;
        }
        if (got == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), got));
    }
    if (file.error() != QFileDevice::NoError) {
        return Result::IoError;
    }
    if (sha256Hex) {
        *sha256Hex = QString::fromLatin1(hash.result().toHex());
    }
    return Result::Ok;
}

bool constantTimeEqual(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); i++) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

Result verifyFile(const QByteArray& sums, const QString& fileName, const QString& path)
{
    QString expected;
    Result found = findHash(sums, fileName, &expected);
    if (found != Result::Ok) {
        return found;
    }
    QString actual;
    Result hashed = hashFile(path, &actual);
    if (hashed != Result::Ok) {
        return hashed;
    }
    return constantTimeEqual(expected.toLatin1(), actual.toLatin1()) ? Result::Ok : Result::HashMismatch;
}

}
