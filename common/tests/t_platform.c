// Platform helper fixes: IPv6 subnet matching (dc09c9f), PKCS7 padded length (2b45ee9)

#include "testutil.h"

#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformCrypto.h"

#include <openssl/evp.h>

bool isInSubnetV6(struct sockaddr_in6* sin6, unsigned char* subnet, int prefixLength);

static void makeAddr(struct sockaddr_in6* sin6, const char* hex16) {
    size_t n;
    memset(sin6, 0, sizeof(*sin6));
    sin6->sin6_family = AF_INET6;
    hexToBytes(hex16, sin6->sin6_addr.s6_addr, &n);
    CHECK_EQ_INT(n, 16);
}

static void testSubnetV6(void) {
    unsigned char linkLocal[16], uniqueLocal[16], siteLocal[16], allZero[16] = { 0 };
    struct sockaddr_in6 a;
    size_t n;

    hexToBytes("fe800000000000000000000000000000", linkLocal, &n);
    hexToBytes("fc000000000000000000000000000000", uniqueLocal, &n);
    hexToBytes("fec00000000000000000000000000000", siteLocal, &n);

    // fe80::/10 covers fe80 to febf
    makeAddr(&a, "fe800000000000000000000000000001");
    CHECK(isInSubnetV6(&a, linkLocal, 10));
    makeAddr(&a, "febf0000000000000000000000000001");
    CHECK(isInSubnetV6(&a, linkLocal, 10));
    makeAddr(&a, "fec00000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, linkLocal, 10));
    makeAddr(&a, "fe400000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, linkLocal, 10));
    makeAddr(&a, "ff800000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, linkLocal, 10));

    // fc00::/7 covers fc00 to fdff
    makeAddr(&a, "fd123456789a0000000000000000abcd");
    CHECK(isInSubnetV6(&a, uniqueLocal, 7));
    makeAddr(&a, "fc000000000000000000000000000001");
    CHECK(isInSubnetV6(&a, uniqueLocal, 7));
    makeAddr(&a, "fe000000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, uniqueLocal, 7));
    makeAddr(&a, "f8000000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, uniqueLocal, 7));

    // fec0::/10 site local
    makeAddr(&a, "fedf0000000000000000000000000001");
    CHECK(isInSubnetV6(&a, siteLocal, 10));
    makeAddr(&a, "fe800000000000000000000000000001");
    CHECK(!isInSubnetV6(&a, siteLocal, 10));

    // Every bit of a /128 prefix must be compared in most significant bit first order
    for (int bit = 0; bit < 128; bit++) {
        struct sockaddr_in6 b;
        makeAddr(&b, "00000000000000000000000000000000");
        b.sin6_addr.s6_addr[bit / 8] = (unsigned char)(0x80 >> (bit % 8));
        CHECK(!isInSubnetV6(&b, allZero, 128));
        CHECK(isInSubnetV6(&b, allZero, bit));      // differs only after the prefix
        CHECK(!isInSubnetV6(&b, allZero, bit + 1)); // differs inside the prefix
    }
}

static void testPkcs7Length(void) {
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(0), 16);
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(1), 16);
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(15), 16);
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(16), 32); // a full padding block is appended
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(17), 32);
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(1392), 1408);
    CHECK_EQ_INT(ROUND_TO_PKCS7_PADDED_LEN(1408), 1424);
}

// A CBC message whose plaintext is a multiple of 16 bytes carries a whole padding block, so
// the ciphertext is 16 bytes longer than the plaintext. The decrypt buffer sized with
// ROUND_TO_PKCS7_PADDED_LEN must hold everything OpenSSL may write: under AddressSanitizer
// a too small macro result is reported as a heap buffer overflow.
static void testCbcAlignedInput(void) {
    unsigned char key[16], iv[16], plain[1392], cipher[1408 + 16];
    int cipherLen = 0, tailLen = 0, decLen = 0;
    EVP_CIPHER_CTX* ec = EVP_CIPHER_CTX_new();
    PPLT_CRYPTO_CONTEXT d = PltCreateCryptoContext(0);
    unsigned char* dec;

    for (int i = 0; i < 16; i++) {
        key[i] = (unsigned char)(i + 1);
        iv[i] = (unsigned char)(0x80 + i);
    }
    for (int i = 0; i < (int)sizeof(plain); i++) {
        plain[i] = (unsigned char)(i * 31);
    }

    CHECK(ec != NULL && d != NULL);
    CHECK(EVP_EncryptInit_ex(ec, EVP_aes_128_cbc(), NULL, key, iv) == 1);
    CHECK(EVP_EncryptUpdate(ec, cipher, &cipherLen, plain, sizeof(plain)) == 1);
    CHECK(EVP_EncryptFinal_ex(ec, cipher + cipherLen, &tailLen) == 1);
    cipherLen += tailLen;
    CHECK_EQ_INT(cipherLen, 1408);

    dec = malloc(ROUND_TO_PKCS7_PADDED_LEN(cipherLen));
    CHECK(dec != NULL);
    if (dec != NULL) {
        CHECK(PltDecryptMessage(d, ALGORITHM_AES_CBC, CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH, 0, 0,
                                key, sizeof(key), iv, sizeof(iv), NULL, 0, cipher, cipherLen, dec, &decLen));
        CHECK_EQ_INT(decLen, (int)sizeof(plain));
        CHECK(decLen == (int)sizeof(plain) && memcmp(dec, plain, sizeof(plain)) == 0);
        free(dec);
    }

    EVP_CIPHER_CTX_free(ec);
    PltDestroyCryptoContext(d);
}

int main(void) {
    RUN(testSubnetV6);
    RUN(testPkcs7Length);
    RUN(testCbcAlignedInput);
    return testFinish("platform");
}
