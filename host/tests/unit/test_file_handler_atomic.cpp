/**
 * @file tests/unit/test_file_handler_atomic.cpp
 * @brief Test file_handler::write_file_atomic and the credentials file written through it.
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <src/config.h>
#include <src/file_handler.h>
#include <src/httpcommon.h>

#include "../tests_common.h"

namespace fs = std::filesystem;

namespace {
  std::string slurp(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
}  // namespace

class AtomicWriteTest: public ::testing::Test {
protected:
  void SetUp() override {
    dir = fs::temp_directory_path() / ("nyx-atomic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  fs::path dir;
};

TEST_F(AtomicWriteTest, CreatesAndReplaces) {
  const auto file = (dir / "data.txt").string();
  ASSERT_EQ(file_handler::write_file_atomic(file.c_str(), "first"), 0);
  EXPECT_EQ(slurp(file), "first");
  ASSERT_EQ(file_handler::write_file_atomic(file.c_str(), "second, longer"), 0);
  EXPECT_EQ(slurp(file), "second, longer");
  EXPECT_FALSE(fs::exists(file + ".tmp"));
}

TEST_F(AtomicWriteTest, KeepsBytesExactly) {
  const auto file = (dir / "bytes.bin").string();
  const std::string data {"a\r\nb\0c\n\r", 8};
  ASSERT_EQ(file_handler::write_file_atomic(file.c_str(), data), 0);
  EXPECT_EQ(slurp(file), data);
}

TEST_F(AtomicWriteTest, FailureLeavesTheOldFileAlone) {
  const auto file = (dir / "keep.txt").string();
  ASSERT_EQ(file_handler::write_file_atomic(file.c_str(), "old"), 0);

  // A directory in the way of the temporary file makes the write fail
  fs::create_directory(file + ".tmp");
  EXPECT_NE(file_handler::write_file_atomic(file.c_str(), "new"), 0);
  EXPECT_EQ(slurp(file), "old");
}

TEST_F(AtomicWriteTest, MissingDirectoryFails) {
  const auto file = (dir / "missing" / "data.txt").string();
  EXPECT_NE(file_handler::write_file_atomic(file.c_str(), "x"), 0);
}

TEST_F(AtomicWriteTest, ReadersNeverSeeAPartialFile) {
  const auto file = (dir / "big.txt").string();
  const std::string small(1000, 'a');
  const std::string big(3 * 1024 * 1024, 'b');
  ASSERT_EQ(file_handler::write_file_atomic(file.c_str(), small), 0);

  std::atomic<bool> done {false};
  std::thread writer([&] {
    for (int i = 0; i < 20; ++i) {
      file_handler::write_file_atomic(file.c_str(), i % 2 ? small : big);
    }
    done = true;
  });

  int partial = 0;
  while (!done) {
    std::error_code ec;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      continue;  // replaced at this very moment (Windows)
    }
    const std::string content {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (content != small && content != big) {
      ++partial;
    }
  }
  writer.join();
  EXPECT_EQ(partial, 0);
}

TEST_F(AtomicWriteTest, CredentialsRoundTrip) {
  const auto file = (dir / "credentials.json").string();
  const auto saved_user = config::sunshine.username;
  const auto saved_pass = config::sunshine.password;
  const auto saved_salt = config::sunshine.salt;

  ASSERT_EQ(http::save_user_creds(file, "admin", "first-password"), 0);
  ASSERT_EQ(http::reload_user_creds(file), 0);
  EXPECT_EQ(config::sunshine.username, "admin");
  const auto first_salt = config::sunshine.salt;
  const auto first_hash = config::sunshine.password;

  ASSERT_EQ(http::save_user_creds(file, "admin", "second-password"), 0);
  ASSERT_EQ(http::reload_user_creds(file), 0);
  EXPECT_NE(config::sunshine.salt, first_salt);
  EXPECT_NE(config::sunshine.password, first_hash);
  EXPECT_FALSE(fs::exists(file + ".tmp"));

  // A failing write reports it and keeps the stored credentials
  fs::create_directory(file + ".tmp");
  const auto before = slurp(file);
  EXPECT_NE(http::save_user_creds(file, "admin", "third-password"), 0);
  EXPECT_EQ(slurp(file), before);

  config::sunshine.username = saved_user;
  config::sunshine.password = saved_pass;
  config::sunshine.salt = saved_salt;
}
