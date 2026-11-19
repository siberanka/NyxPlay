/**
 * @file tests/unit/test_pairing_name.cpp
 * @brief Test the sanitising of the device name a client sends when it pairs (src/nvhttp.*).
 */
#include <string>

#include <nlohmann/json.hpp>

#include <src/nvhttp.h>

#include "../tests_common.h"

namespace {
  const std::string replacement = "\xEF\xBF\xBD";
}

TEST(PairingName, ValidTextIsKept) {
  EXPECT_EQ(nvhttp::sanitize_pairing_name("Pixel 8"), "Pixel 8");
  EXPECT_EQ(nvhttp::sanitize_pairing_name("Ömer'in Telefonu \xE2\x82\xAC \xF0\x9F\x93\xB1"), "Ömer'in Telefonu \xE2\x82\xAC \xF0\x9F\x93\xB1");
  EXPECT_EQ(nvhttp::sanitize_pairing_name(""), "");
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a;b|c&d$(e)`f`\"g\" <h>"), "a;b|c&d$(e)`f`\"g\" <h>");
}

TEST(PairingName, InvalidUtf8IsReplaced) {
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\xFF"), replacement);
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\xFF" "b"), "a" + replacement + "b");
  // Overlong encoding of NUL and of a slash
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\xC0\x80"), replacement + replacement);
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\xE0\x80\xAF"), replacement + replacement + replacement);
  // UTF-16 surrogate and a value beyond U+10FFFF
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\xED\xA0\x80"), replacement + replacement + replacement);
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\xF4\x90\x80\x80"), replacement + replacement + replacement + replacement);
  // A stray continuation byte
  EXPECT_EQ(nvhttp::sanitize_pairing_name("\x80"), replacement);
}

TEST(PairingName, TruncatedMultibyteSequenceIsReplaced) {
  EXPECT_EQ(nvhttp::sanitize_pairing_name("abc\xE2\x82"), "abc" + replacement + replacement);
  EXPECT_EQ(nvhttp::sanitize_pairing_name("abc\xF0\x9F\x93"), "abc" + replacement + replacement + replacement);
  EXPECT_EQ(nvhttp::sanitize_pairing_name("abc\xC3"), "abc" + replacement);
}

TEST(PairingName, ControlCharactersAreDropped) {
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\r\nb"), "ab");
  EXPECT_EQ(nvhttp::sanitize_pairing_name(std::string("a\0b", 3)), "ab");
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\tb\x1B[31mc\x7F"), "ab[31mc");
  // C1 controls (NEL), line/paragraph separators and text direction overrides
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\xC2\x85" "b"), "ab");
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c"), "abc");
  EXPECT_EQ(nvhttp::sanitize_pairing_name("a\xE2\x80\xAE" "b\xE2\x81\xA6" "c"), "abc");
}

TEST(PairingName, CutAtACodePointBoundaryWithinTheLimit) {
  const auto limit = nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE;

  EXPECT_EQ(nvhttp::sanitize_pairing_name(std::string(limit, 'a')), std::string(limit, 'a'));
  EXPECT_EQ(nvhttp::sanitize_pairing_name(std::string(limit + 50, 'a')), std::string(limit, 'a'));

  // Two byte characters: 64 fit
  std::string two_bytes;
  for (int i = 0; i < 100; ++i) {
    two_bytes += "\xC3\xB6";
  }
  EXPECT_EQ(nvhttp::sanitize_pairing_name(two_bytes).size(), limit);

  // Three byte characters: 42 fit (126 bytes), a 43rd would cross the limit
  std::string three_bytes;
  for (int i = 0; i < 100; ++i) {
    three_bytes += "\xE2\x82\xAC";
  }
  const auto cut_three = nvhttp::sanitize_pairing_name(three_bytes);
  EXPECT_EQ(cut_three.size(), 126U);
  EXPECT_EQ(nvhttp::sanitize_pairing_name(cut_three), cut_three);

  // Four byte characters: 32 fit
  std::string four_bytes;
  for (int i = 0; i < 100; ++i) {
    four_bytes += "\xF0\x9F\x93\xB1";
  }
  EXPECT_EQ(nvhttp::sanitize_pairing_name(four_bytes).size(), limit);

  // Replacement characters count against the limit too
  EXPECT_EQ(nvhttp::sanitize_pairing_name(std::string(200, '\xFF')).size(), 126U);
}

TEST(PairingName, ResultCanBeWrittenAsJson) {
  for (const std::string &input : {std::string("\xFF\xFE"), std::string("ab\xE2\x82"), std::string("\xED\xA0\x80"), std::string("a\r\nb"), std::string(300, '\xC3')}) {
    const auto name = nvhttp::sanitize_pairing_name(input);
    nlohmann::json json {{"name", name}};
    EXPECT_NO_THROW(json.dump());
    EXPECT_EQ(nlohmann::json::parse(json.dump()).at("name").get<std::string>(), name);
  }
}

TEST(PairingName, JsonDumpNeedsTheReplaceHandler) {
  nlohmann::json json {{"name", std::string("Phone \xFF")}};
  EXPECT_THROW(json.dump(), nlohmann::json::type_error);
  EXPECT_NO_THROW(json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}
