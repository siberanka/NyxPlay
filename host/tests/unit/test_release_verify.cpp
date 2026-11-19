/**
 * @file tests/unit/test_release_verify.cpp
 * @brief Test the verification of signed release checksums (src/release_verify.*).
 */
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <src/release_verify.h>

#include "../tests_common.h"

namespace rv = release_verify;

namespace {
  const std::string installer_name = "NyxPlayHost-2026.11.19.2-windows-x64.exe";
  const std::string large_name = "NyxPlay-2026.11.19.2-arm64-v8a-release.apk";
  const std::string empty_name = "NyxPlay-empty.bin";

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  class ReleaseVerify: public ::testing::Test {
  protected:
    void SetUp() override {
      const std::filesystem::path fixtures = std::filesystem::path(SUNSHINE_SOURCE_DIR) / "tests" / "fixtures" / "release";
      sums = read_file(fixtures / "SHA256SUMS.txt");
      sig = read_file(fixtures / "SHA256SUMS.txt.sig");
      ASSERT_FALSE(sums.empty());
      ASSERT_FALSE(sig.empty());
      work = std::filesystem::path(SUNSHINE_TEST_BIN_DIR) / "release_verify_work";
      std::filesystem::create_directories(work);
    }

    void TearDown() override {
      std::error_code ec;
      std::filesystem::remove_all(work, ec);
    }

    void write(const std::string &name, const std::string &content) {
      std::ofstream out(work / name, std::ios::binary);
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    static std::string large_content() {
      std::string data(5 * 1024 * 1024 + 17, '\0');
      for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>((i * 31 + 7) & 0xFF);
      }
      return data;
    }

    std::string sums;
    std::string sig;
    std::filesystem::path work;
  };

  // An unrelated public key (RFC 8032 test vector 1)
  const rv::public_key_t other_key {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
}  // namespace

TEST_F(ReleaseVerify, EmbeddedKeyFingerprint) {
  ASSERT_EQ(rv::trusted_keys().size(), 1u);
  EXPECT_EQ(rv::key_fingerprint(rv::trusted_keys()[0]), "db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1");
}

TEST_F(ReleaseVerify, RealSignatureVerifies) {
  EXPECT_EQ(rv::verify_signature(sums, sig), rv::result_e::ok);
  // The signature text is accepted with or without its trailing line feed
  EXPECT_EQ(rv::verify_signature(sums, sig.substr(0, sig.size() - 1)), rv::result_e::ok);
}

TEST_F(ReleaseVerify, TamperedSumsAreRejected) {
  auto changed = sums;
  changed[0] = changed[0] == 'a' ? 'b' : 'a';
  EXPECT_EQ(rv::verify_signature(changed, sig), rv::result_e::bad_signature);
  EXPECT_EQ(rv::verify_signature(sums + "\n", sig), rv::result_e::bad_signature);
  EXPECT_EQ(rv::verify_signature(std::string_view(sums).substr(1), sig), rv::result_e::bad_signature);
}

TEST_F(ReleaseVerify, TamperedSignatureIsRejected) {
  auto changed = sig;
  changed[0] = changed[0] == 'A' ? 'B' : 'A';
  EXPECT_EQ(rv::verify_signature(sums, changed), rv::result_e::bad_signature);
  changed = sig;
  changed[40] = changed[40] == 'A' ? 'B' : 'A';
  EXPECT_EQ(rv::verify_signature(sums, changed), rv::result_e::bad_signature);
}

TEST_F(ReleaseVerify, WrongKeyIsRejected) {
  const std::array<rv::public_key_t, 1> keys {other_key};
  EXPECT_EQ(rv::verify_signature(sums, sig, keys), rv::result_e::bad_signature);
  // A key that is not a valid point never verifies and never throws
  const std::array<rv::public_key_t, 1> zero {};
  EXPECT_NE(rv::verify_signature(sums, sig, zero), rv::result_e::ok);
  // A key list accepts the signature when any key matches (key rotation)
  const std::array<rv::public_key_t, 2> rotated {other_key, rv::trusted_keys()[0]};
  EXPECT_EQ(rv::verify_signature(sums, sig, rotated), rv::result_e::ok);
  EXPECT_EQ(rv::verify_signature(sums, sig, std::span<const rv::public_key_t> {}), rv::result_e::bad_signature);
}

TEST_F(ReleaseVerify, MalformedSignatureText) {
  const auto bare = sig.substr(0, 88);
  EXPECT_EQ(rv::verify_signature(sums, ""), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, "\n"), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, bare.substr(0, 87)), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, bare + "A"), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, bare + "\n\n"), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, bare + "\r\n"), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, " " + bare), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, bare.substr(0, 86)), rv::result_e::malformed);

  auto bad_char = bare;
  bad_char[10] = '*';
  EXPECT_EQ(rv::verify_signature(sums, bad_char), rv::result_e::malformed);
  bad_char[10] = '-';  // URL-safe alphabet is not accepted
  EXPECT_EQ(rv::verify_signature(sums, bad_char), rv::result_e::malformed);

  // Non-zero padding bits in the last character
  auto bits = bare;
  bits[85] = 'B';
  EXPECT_EQ(rv::verify_signature(sums, bits), rv::result_e::malformed);

  // 63 and 65 bytes of valid Base64
  EXPECT_EQ(rv::verify_signature(sums, std::string(84, 'A') + "A="), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, std::string(86, 'A')), rv::result_e::malformed);
  EXPECT_EQ(rv::verify_signature(sums, std::string(88, 'A')), rv::result_e::malformed);
}

TEST_F(ReleaseVerify, Rfc8032VectorThroughTheKeyList) {
  // Empty message, RFC 8032 section 7.1 test 1; the checksum list parser is not involved
  const std::string signature = "5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw==";
  const std::array<rv::public_key_t, 1> keys {other_key};
  EXPECT_EQ(rv::verify_signature("", signature, keys), rv::result_e::ok);
}

TEST_F(ReleaseVerify, ParsesTheFixture) {
  std::vector<rv::sums_entry_t> entries;
  ASSERT_EQ(rv::parse_sums(sums, entries), rv::result_e::ok);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[1].name, empty_name);
  EXPECT_EQ(entries[1].sha256_hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(ReleaseVerify, StrictSumsFormat) {
  const std::string hash(64, 'a');
  const std::string good = hash + "  file.bin\n";
  std::vector<rv::sums_entry_t> entries;
  EXPECT_EQ(rv::parse_sums(good, entries), rv::result_e::ok);
  EXPECT_EQ(rv::parse_sums(hash + "  file.bin", entries), rv::result_e::ok);  // missing final LF is tolerated
  EXPECT_EQ(rv::parse_sums("", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums("\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(good + "\n", entries), rv::result_e::malformed);  // empty line
  EXPECT_EQ(rv::parse_sums(hash + " *file.bin\n", entries), rv::result_e::malformed);  // binary mode marker
  EXPECT_EQ(rv::parse_sums(hash + " file.bin\n", entries), rv::result_e::malformed);  // one space
  EXPECT_EQ(rv::parse_sums(hash + "   file.bin\n", entries), rv::result_e::malformed);  // three spaces
  EXPECT_EQ(rv::parse_sums(std::string(64, 'A') + "  file.bin\n", entries), rv::result_e::malformed);  // upper case hex
  EXPECT_EQ(rv::parse_sums(std::string(63, 'a') + "  file.bin\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(std::string(65, 'a') + "  file.bin\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(std::string(63, 'a') + "g  file.bin\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(hash + "  file.bin\r\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(hash + "  \n", entries), rv::result_e::malformed);  // no name
  EXPECT_EQ(rv::parse_sums(hash + "  file name.bin\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(hash + "  " + std::string(256, 'x') + "\n", entries), rv::result_e::malformed);
  EXPECT_EQ(rv::parse_sums(hash + "  " + std::string(255, 'x') + "\n", entries), rv::result_e::ok);
  EXPECT_EQ(rv::parse_sums(std::string(rv::max_sums_size + 1, 'a'), entries), rv::result_e::malformed);
  EXPECT_TRUE(entries.empty());
}

TEST_F(ReleaseVerify, PathLikeNamesAreMalformed) {
  const std::string hash(64, 'b');
  std::vector<rv::sums_entry_t> entries;
  for (const char *name : {"../evil.exe", "dir/file.bin", "dir\\file.bin", "/abs.bin", "C:file.bin", "..", ".", "a%2Fb", "file.bin:stream", "na\xC3\xAFve"}) {
    EXPECT_EQ(rv::parse_sums(hash + "  " + name + "\n", entries), rv::result_e::malformed) << name;
  }
  EXPECT_EQ(rv::parse_sums(hash + "  ...\n", entries), rv::result_e::ok);
  std::string hex;
  EXPECT_EQ(rv::find_hash(sums, "../" + installer_name, hex), rv::result_e::not_listed);
  EXPECT_EQ(rv::find_hash(sums, "x/" + installer_name, hex), rv::result_e::not_listed);
}

TEST_F(ReleaseVerify, DuplicateAndMissingEntries) {
  const std::string a(64, 'a');
  const std::string b(64, 'b');
  std::string hex;
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n" + b + "  one.bin\n", "one.bin", hex), rv::result_e::malformed);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n" + a + "  one.bin\n", "one.bin", hex), rv::result_e::malformed);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n" + b + "  two.bin\n", "two.bin", hex), rv::result_e::ok);
  EXPECT_EQ(hex, b);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n" + b + "  two.bin\n", "three.bin", hex), rv::result_e::not_listed);
  EXPECT_TRUE(hex.empty());
  // Exact match only: no case folding, no prefix or suffix match
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n", "One.bin", hex), rv::result_e::not_listed);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n", "one.bi", hex), rv::result_e::not_listed);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n", "one.bin.", hex), rv::result_e::not_listed);
  EXPECT_EQ(rv::find_hash(a + "  one.bin\n", "", hex), rv::result_e::not_listed);
  EXPECT_EQ(rv::find_hash(sums, installer_name, hex), rv::result_e::ok);
  EXPECT_EQ(hex, "fb5c85338c609e0c9284185f942c7683275e2248086e5824611ef30f5ecd6a58");
}

TEST_F(ReleaseVerify, FileMatchesSignedHash) {
  write(installer_name, "nyxplay fixture installer\n");
  EXPECT_EQ(rv::verify_file(sums, installer_name, work / installer_name), rv::result_e::ok);
  write(empty_name, "");
  EXPECT_EQ(rv::verify_file(sums, empty_name, work / empty_name), rv::result_e::ok);
}

TEST_F(ReleaseVerify, HashMismatch) {
  write(installer_name, "nyxplay fixture installer\n\n");
  EXPECT_EQ(rv::verify_file(sums, installer_name, work / installer_name), rv::result_e::hash_mismatch);
  write(installer_name, "Nyxplay fixture installer\n");
  EXPECT_EQ(rv::verify_file(sums, installer_name, work / installer_name), rv::result_e::hash_mismatch);
  write(installer_name, "");
  EXPECT_EQ(rv::verify_file(sums, installer_name, work / installer_name), rv::result_e::hash_mismatch);
}

TEST_F(ReleaseVerify, NotListedAndIoError) {
  write("other.bin", "x");
  EXPECT_EQ(rv::verify_file(sums, "other.bin", work / "other.bin"), rv::result_e::not_listed);
  EXPECT_EQ(rv::verify_file(sums, installer_name, work / "does-not-exist.bin"), rv::result_e::io_error);
  EXPECT_EQ(rv::verify_file(sums, installer_name, work), rv::result_e::io_error);  // a directory
  EXPECT_EQ(rv::verify_file("garbage", installer_name, work / "x"), rv::result_e::malformed);
  std::string hex;
  EXPECT_EQ(rv::hash_file(work / "does-not-exist.bin", hex), rv::result_e::io_error);
  EXPECT_TRUE(hex.empty());
}

TEST_F(ReleaseVerify, LargeFileIsStreamedInChunks) {
  write(large_name, large_content());
  EXPECT_EQ(rv::verify_file(sums, large_name, work / large_name), rv::result_e::ok);
  // One changed byte near the end is detected
  auto data = large_content();
  data[data.size() - 5] ^= 0x01;
  write(large_name, data);
  EXPECT_EQ(rv::verify_file(sums, large_name, work / large_name), rv::result_e::hash_mismatch);
}

TEST_F(ReleaseVerify, ConstantTimeCompare) {
  EXPECT_TRUE(rv::constant_time_equal("abc", "abc"));
  EXPECT_TRUE(rv::constant_time_equal("", ""));
  EXPECT_FALSE(rv::constant_time_equal("abc", "abd"));
  EXPECT_FALSE(rv::constant_time_equal("abc", "ab"));
  EXPECT_FALSE(rv::constant_time_equal("abc", "abcd"));
}

TEST_F(ReleaseVerify, ResultNames) {
  EXPECT_STREQ(rv::to_string(rv::result_e::ok), "ok");
  EXPECT_STREQ(rv::to_string(rv::result_e::bad_signature), "bad signature");
  EXPECT_STREQ(rv::to_string(rv::result_e::io_error), "I/O error");
}
