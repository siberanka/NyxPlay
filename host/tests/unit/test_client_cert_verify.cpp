/**
 * @file tests/unit/test_client_cert_verify.cpp
 * @brief Test crypto::cert_chain_t::verify() accepts only the exact paired client certificate.
 */
#include <memory>
#include <string>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <src/crypto.h>

#include "../tests_common.h"

namespace {
  /**
   * @brief Generate a self-signed certificate that OpenSSL may use as a CA.
   */
  crypto::creds_t make_ca(const std::string_view cn) {
    auto creds = crypto::gen_creds(cn, 2048);
    auto cert = crypto::x509(creds.x509);
    auto key = crypto::pkey(creds.pkey);

    util::safe_ptr<BASIC_CONSTRAINTS, &BASIC_CONSTRAINTS_free> constraints {BASIC_CONSTRAINTS_new()};
    constraints->ca = 1;
    X509_add1_ext_i2d(cert.get(), NID_basic_constraints, constraints.get(), 1, X509V3_ADD_DEFAULT);
    X509_sign(cert.get(), key.get(), EVP_sha256());
    creds.x509 = crypto::pem(cert);
    return creds;
  }

  /**
   * @brief Generate a certificate with its own key whose issuer is the given CA, signed by the CA key.
   */
  crypto::creds_t make_derived(const crypto::creds_t &issuer) {
    auto creds = crypto::gen_creds("Derived Client", 2048);
    auto cert = crypto::x509(creds.x509);
    auto issuer_cert = crypto::x509(issuer.x509);
    auto issuer_key = crypto::pkey(issuer.pkey);

    X509_set_issuer_name(cert.get(), X509_get_subject_name(issuer_cert.get()));
    X509_sign(cert.get(), issuer_key.get(), EVP_sha256());
    creds.x509 = crypto::pem(cert);
    return creds;
  }

  crypto::p_named_cert_t make_named(const std::string &pem, const std::string &name) {
    auto named = std::make_shared<crypto::named_cert_t>();
    named->name = name;
    named->uuid = name + "-uuid";
    named->cert = pem;
    return named;
  }
}  // namespace

class ClientCertVerifyTest: public ::testing::Test {
protected:
  crypto::cert_chain_t chain;

  void pair(const crypto::p_named_cert_t &named) {
    auto copy = named;
    chain.add(copy);
  }

  const char *verify(const std::string &pem, crypto::p_named_cert_t &out) {
    auto cert = crypto::x509(pem);
    EXPECT_TRUE(static_cast<bool>(cert));
    return chain.verify(cert.get(), out);
  }
};

TEST_F(ClientCertVerifyTest, ExactPairedCertificateIsAccepted) {
  auto ca = make_ca("Paired Client");
  auto named = make_named(ca.x509, "paired");
  pair(named);

  crypto::p_named_cert_t out;
  EXPECT_EQ(verify(ca.x509, out), nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->name, "paired");
}

TEST_F(ClientCertVerifyTest, CertificateIssuedByPairedCertificateIsRejected) {
  auto ca = make_ca("Paired Client");
  auto derived = make_derived(ca);
  auto named = make_named(ca.x509, "paired");
  pair(named);

  crypto::p_named_cert_t out;
  EXPECT_NE(verify(derived.x509, out), nullptr);
  EXPECT_EQ(out, nullptr);
}

TEST_F(ClientCertVerifyTest, UnrelatedSelfSignedCertificateIsRejected) {
  auto ca = make_ca("Paired Client");
  auto other = make_ca("Other Client");
  auto named = make_named(ca.x509, "paired");
  pair(named);

  crypto::p_named_cert_t out;
  EXPECT_NE(verify(other.x509, out), nullptr);
  EXPECT_EQ(out, nullptr);
}

TEST_F(ClientCertVerifyTest, EmptyChainRejectsEverything) {
  auto ca = make_ca("Anyone");
  crypto::p_named_cert_t out;
  EXPECT_NE(verify(ca.x509, out), nullptr);
  EXPECT_EQ(out, nullptr);
}

TEST_F(ClientCertVerifyTest, CorrectCertificateIsFoundAmongSeveralPaired) {
  auto first = make_ca("First");
  auto second = make_ca("Second");
  auto first_named = make_named(first.x509, "first");
  auto second_named = make_named(second.x509, "second");
  pair(first_named);
  pair(second_named);

  crypto::p_named_cert_t out;
  EXPECT_EQ(verify(second.x509, out), nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->name, "second");
}
