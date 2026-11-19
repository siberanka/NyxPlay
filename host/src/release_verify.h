/**
 * @file src/release_verify.h
 * @brief Verification of signed release checksums (SHA256SUMS.txt + detached Ed25519 signature).
 *
 * The functions never throw. Every failure is reported through release_verify::result_e.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace release_verify {

  enum class result_e {
    ok,  ///< Everything checked out.
    bad_signature,  ///< The signature is well formed but does not match any trusted key.
    malformed,  ///< The signature text or the checksum list is not in the strict format.
    not_listed,  ///< The file name has no entry in the checksum list.
    hash_mismatch,  ///< The file content does not match the signed hash.
    io_error,  ///< The file could not be read.
  };

  /// A raw 32 byte Ed25519 public key.
  using public_key_t = std::array<std::uint8_t, 32>;

  constexpr std::size_t signature_size = 64;
  constexpr std::size_t max_sums_size = 64 * 1024;

  struct sums_entry_t {
    std::string name;
    std::string sha256_hex;  ///< 64 lowercase hex characters
  };

  /// Short stable name of a result, for logs.
  const char *to_string(result_e result);

  /// The release signing keys embedded in this build. The list allows a new key to ship one release before it is used.
  std::span<const public_key_t> trusted_keys();

  /// Lowercase hex SHA-256 of the raw public key. Safe to log.
  std::string key_fingerprint(const public_key_t &key);

  /**
   * @brief Decode a strict Base64 signature: 88 characters with "==" padding, canonical, 64 bytes.
   *        One trailing LF is accepted so that a signature file can be passed as it is.
   * @return false if the text is not in that exact form.
   */
  bool decode_signature(std::string_view text, std::array<std::uint8_t, signature_size> &out);

  /// Verify the Ed25519 signature over the exact bytes of the checksum list against any key in the list.
  result_e verify_signature(std::string_view sums, std::string_view signature_base64, std::span<const public_key_t> keys);

  /// Verify the signature using the embedded keys.
  result_e verify_signature(std::string_view sums, std::string_view signature_base64);

  /**
   * @brief Parse the checksum list strictly: `<64 lowercase hex><two spaces><name>` per line,
   *        names only [A-Za-z0-9._-] (not "." or ".."), no duplicate names, at most max_sums_size bytes.
   */
  result_e parse_sums(std::string_view sums, std::vector<sums_entry_t> &entries);

  /// Look up the exact file name. Returns not_listed when it has no entry.
  result_e find_hash(std::string_view sums, std::string_view file_name, std::string &sha256_hex);

  /// Streaming SHA-256 of a local file as lowercase hex.
  result_e hash_file(const std::filesystem::path &path, std::string &sha256_hex);

  /// Compare two hex strings without an early exit on the first difference.
  bool constant_time_equal(std::string_view a, std::string_view b);

  /// Check a local file against its entry in the (already signature checked) checksum list.
  result_e verify_file(std::string_view sums, std::string_view file_name, const std::filesystem::path &path);
}  // namespace release_verify
