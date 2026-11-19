/**
 * @file tests/unit/test_process_apps.cpp
 * @brief Test the loading of apps.json in src/process.*.
 */
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include <src/process.h>

#include "../tests_common.h"

namespace fs = std::filesystem;
using nlohmann::json;

class AppsLoaderTest: public ::testing::Test {
protected:
  void SetUp() override {
    dir = fs::temp_directory_path() / ("nyx-apps-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    file = (dir / "apps.json").string();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  void write(const json &tree) const {
    std::ofstream out(file, std::ios::binary);
    out << tree.dump(2);
  }

  static bool has_app(const proc::proc_t &proc, const std::string &name) {
    const auto &apps = proc.get_apps();
    return std::ranges::any_of(apps, [&](const proc::ctx_t &app) {
      return app.name == name;
    });
  }

  fs::path dir;
  std::string file;
};

TEST_F(AppsLoaderTest, LoadsValidApps) {
  write({{"env", json::object()}, {"apps", json::array({{{"name", "One"}, {"uuid", "u1"}}, {{"name", "Two"}, {"uuid", "u2"}}})}});
  auto loaded = proc::parse(file);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(has_app(*loaded, "One"));
  EXPECT_TRUE(has_app(*loaded, "Two"));
}

TEST_F(AppsLoaderTest, OneBrokenEntryDoesNotHideTheOthers) {
  write({{"env", json::object()},
         {"apps", json::array({{{"name", "One"}, {"uuid", "u1"}},
                               {{"name", 5}, {"uuid", "u2"}},
                               {{"name", "Three"}, {"uuid", "u3"}, {"cmd", 7}},
                               {{"name", "Four"}, {"uuid", "u4"}}})}});
  auto loaded = proc::parse(file);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(has_app(*loaded, "One"));
  EXPECT_TRUE(has_app(*loaded, "Four"));
  EXPECT_FALSE(has_app(*loaded, "Three"));
  EXPECT_FALSE(has_app(*loaded, "Desktop (fallback)"));
}

TEST_F(AppsLoaderTest, OnlyBrokenEntriesFallBackToDesktop) {
  write({{"env", json::object()}, {"apps", json::array({{{"name", 1}, {"uuid", "u1"}}, {{"name", 2}, {"uuid", "u2"}}})}});
  auto loaded = proc::parse(file);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(has_app(*loaded, "Desktop (fallback)"));
}

TEST_F(AppsLoaderTest, AppListCopyIsIndependentOfAReload) {
  write({{"env", json::object()}, {"apps", json::array({{{"name", "One"}, {"uuid", "u1"}}, {{"name", "Two"}, {"uuid", "u2"}}})}});
  auto loaded = proc::parse(file);
  ASSERT_TRUE(loaded);

  const auto copy = loaded->get_apps_copy();
  ASSERT_EQ(copy.size(), loaded->get_apps().size());
  ASSERT_FALSE(copy.empty());

  write({{"env", json::object()}, {"apps", json::array({{{"name", "Other"}, {"uuid", "u3"}}})}});
  auto reloaded = proc::parse(file);
  ASSERT_TRUE(reloaded);
  loaded->replace_configuration(std::move(*reloaded));

  EXPECT_TRUE(has_app(*loaded, "Other"));
  EXPECT_FALSE(has_app(*loaded, "One"));
  // The copy taken before still shows the old list
  EXPECT_TRUE(std::ranges::any_of(copy, [](const proc::ctx_t &app) {
    return app.name == "One";
  }));
  EXPECT_FALSE(std::ranges::any_of(copy, [](const proc::ctx_t &app) {
    return app.name == "Other";
  }));
}
