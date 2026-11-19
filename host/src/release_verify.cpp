/**
 * @file src/release_verify.cpp
 * @brief Verification of signed release checksums (SHA256SUMS.txt + detached Ed25519 signature).
 */
// standard includes
#include <algorithm>
#include <fstream>
#include <memory>

// lib includes
#include <openssl/evp.h>

// local includes
#include "release_verify.h"

namespace release_verify {

  namespace {
    // SHA-256 fingerprint of this key:
    // db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1
    constexpr std::array<public_key_t, 1> embedded_keys {{
      {0x94, 0x2b, 0xa3, 0x46, 0x02, 0x0e, 0xd3, 0x3f, 0xcf, 0x76, 0xfe, 0xb1, 0x70, 0xe0, 0x56, 0xc1,
       0x6a, 0xf2, 0xa2, 0x86, 0x4b, 0xff, 0xaa, 0x6f, 0xd6, 0xff, 0x42, 0x24, 0xaf, 0x8e, 0x93, 0x26},
    }};

    struct md_ctx_deleter_t {
      void operator()(EVP_MD_CTX *ctx) const {
        EVP_MD_CTX_free(ctx);
      }
    };
    using md_ctx_t = std::unique_ptr<EVP_MD_CTX, md_ctx_deleter_t>;

    struct pkey_deleter_t {
      void operator()(EVP_PKEY *key) const {
        EVP_PKEY_free(key);
      }
    };
    using pkey_t = std::unique_ptr<EVP_PKEY, pkey_deleter_t>;

    constexpr char hex_digits[] = "0123456789abcdef";

    std::string to_hex(const unsigned char *data, std::size_t size) {
      std::string out;
      out.reserve(size * 2);
      for (std::size_t i = 0; i < size; ++i) {
        out.push_back(hex_digits[data[i] >> 4]);
        out.push_back(hex_digits[data[i] & 0x0F]);
      }
      return out;
    }

    int base64_value(char c) {
      if (c >= 'A' && c <= 'Z') {
        return c - 'A';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
      }
      if (c >= '0' && c <= '9') {
        return c - '0' + 52;
      }
      if (c == '+') {
        return 62;
      }
      if (c == '/') {
        return 63;
      }
      return -1;
    }

    bool is_name_char(char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    }

    bool is_lower_hex(char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }

    bool verify_one(const public_key_t &key, std::string_view sums, const std::array<std::uint8_t, signature_size> &sig) {
      pkey_t pkey {EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size())};
      if (!pkey) {
        return false;
      }
      md_ctx_t ctx {EVP_MD_CTX_new()};
      if (!ctx) {
        return false;
      }
      if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
        return false;
      }
      return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), reinterpret_cast<const unsigned char *>(sums.data()), sums.size()) == 1;
    }
  }  // namespace

  const char *to_string(result_e result) {
    switch (result) {
      case result_e::ok:
        return "ok";
      case result_e::bad_signature:
        return "bad signature";
      case result_e::malformed:
        return "malformed";
      case result_e::not_listed:
        return "not listed";
      case result_e::hash_mismatch:
        return "hash mismatch";
      case result_e::io_error:
        return "I/O error";
    }
    return "unknown";
  }

  std::span<const public_key_t> trusted_keys() {
    return embedded_keys;
  }

  std::string key_fingerprint(const public_key_t &key) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_Digest(key.data(), key.size(), digest, &size, EVP_sha256(), nullptr) != 1) {
      return {};
    }
    return to_hex(digest, size);
  }

  bool decode_signature(std::string_view text, std::array<std::uint8_t, signature_size> &out) {
    if (!text.empty() && text.back() == '\n') {
      text.remove_suffix(1);
    }
    // 64 bytes = 21 groups of 4 characters + one group with two characters and "=="
    if (text.size() != 88 || text[86] != '=' || text[87] != '=') {
      return false;
    }
    std::size_t pos = 0;
    for (std::size_t group = 0; group < 22; ++group) {
      int v[4] {0, 0, 0, 0};
      const std::size_t chars = group == 21 ? 2 : 4;
      for (std::size_t i = 0; i < chars; ++i) {
        v[i] = base64_value(text[group * 4 + i]);
        if (v[i] < 0) {
          return false;
        }
      }
      out[pos++] = static_cast<std::uint8_t>((v[0] << 2) | (v[1] >> 4));
      if (group == 21) {
        // the unused low four bits of the last character must be zero
        if ((v[1] & 0x0F) != 0) {
          return false;
        }
      } else {
        out[pos++] = static_cast<std::uint8_t>(((v[1] & 0x0F) << 4) | (v[2] >> 2));
        out[pos++] = static_cast<std::uint8_t>(((v[2] & 0x03) << 6) | v[3]);
      }
    }
    return pos == signature_size;
  }

  result_e verify_signature(std::string_view sums, std::string_view signature_base64, std::span<const public_key_t> keys) {
    std::array<std::uint8_t, signature_size> sig {};
    if (sums.size() > max_sums_size || !decode_signature(signature_base64, sig)) {
      return result_e::malformed;
    }
    for (const auto &key : keys) {
      if (verify_one(key, sums, sig)) {
        return result_e::ok;
      }
    }
    return result_e::bad_signature;
  }

  result_e verify_signature(std::string_view sums, std::string_view signature_base64) {
    return verify_signature(sums, signature_base64, trusted_keys());
  }

  result_e parse_sums(std::string_view sums, std::vector<sums_entry_t> &entries) {
    entries.clear();
    if (sums.empty() || sums.size() > max_sums_size) {
      return result_e::malformed;
    }
    std::size_t pos = 0;
    while (pos < sums.size()) {
      const auto end = sums.find('\n', pos);
      const auto line = sums.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
      pos = end == std::string_view::npos ? sums.size() : end + 1;

      // 64 hex + two spaces + at least one name character
      const auto name = line.size() > 66 ? line.substr(66) : std::string_view {};
      if (line.size() < 67 || name.size() > 255 || line[64] != ' ' || line[65] != ' ' ||
          !std::all_of(line.begin(), line.begin() + 64, is_lower_hex) ||
          !std::all_of(name.begin(), name.end(), is_name_char) || name == "." || name == ".." ||
          std::any_of(entries.begin(), entries.end(), [&](const sums_entry_t &e) {
            return e.name == name;
          })) {
        entries.clear();
        return result_e::malformed;
      }
      entries.push_back({std::string(name), std::string(line.substr(0, 64))});
    }
    return entries.empty() ? result_e::malformed : result_e::ok;
  }

  result_e find_hash(std::string_view sums, std::string_view file_name, std::string &sha256_hex) {
    sha256_hex.clear();
    std::vector<sums_entry_t> entries;
    if (auto r = parse_sums(sums, entries); r != result_e::ok) {
      return r;
    }
    for (const auto &entry : entries) {
      if (entry.name == file_name) {
        sha256_hex = entry.sha256_hex;
        return result_e::ok;
      }
    }
    return result_e::not_listed;
  }

  result_e hash_file(const std::filesystem::path &path, std::string &sha256_hex) {
    sha256_hex.clear();
    try {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return result_e::io_error;
      }
      md_ctx_t ctx {EVP_MD_CTX_new()};
      if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return result_e::io_error;
      }
      std::vector<char> buffer(64 * 1024);
      while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0 && EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(got)) != 1) {
          return result_e::io_error;
        }
      }
      if (!in.eof()) {
        return result_e::io_error;
      }
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int size = 0;
      if (EVP_DigestFinal_ex(ctx.get(), digest, &size) != 1) {
        return result_e::io_error;
      }
      sha256_hex = to_hex(digest, size);
      return result_e::ok;
    } catch (...) {
      return result_e::io_error;
    }
  }

  bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
      return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
  }

  result_e verify_file(std::string_view sums, std::string_view file_name, const std::filesystem::path &path) {
    std::string expected;
    if (auto r = find_hash(sums, file_name, expected); r != result_e::ok) {
      return r;
    }
    std::string actual;
    if (auto r = hash_file(path, actual); r != result_e::ok) {
      return r;
    }
    return constant_time_equal(expected, actual) ? result_e::ok : result_e::hash_mismatch;
  }
}  // namespace release_verify
