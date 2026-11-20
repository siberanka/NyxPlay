/**
 * @file tests/unit/test_update_install.cpp
 * @brief Test the one-click update installation (src/update_install.*).
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <src/update_install.h>
#include <tools/svc_logic.h>

#include "../tests_common.h"

using namespace std::literals;
namespace ui = update_install;
namespace fs = std::filesystem;

namespace {
  const std::string tag = "v2026.11.19.2";
  const std::string version = "2026.11.19.2";
  const std::string installer_file = "NyxPlayHost-2026.11.19.2-windows-x64.exe";

  std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void write_file(const fs::path &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  // The content whose hash is listed for the installer in the signed fixture
  std::string installer_content() {
    return "nyxplay fixture installer\n";
  }

  update_check::release_t make_release() {
    update_check::release_t release;
    release.tag = tag;
    release.url = "https://github.com/siberanka/NyxPlay/releases/tag/" + tag;
    release.version = *update_check::parse_version(tag, true);
    return release;
  }

  class UpdateInstall: public ::testing::Test {
  protected:
    void SetUp() override {
      fixtures = fs::path(SUNSHINE_SOURCE_DIR) / "tests" / "fixtures" / "release";
      dir = fs::path(SUNSHINE_TEST_BIN_DIR) / "update_install_work" / "updates";
      fs::remove_all(dir.parent_path());

      env.updates_dir = dir;
      env.current_version = "2026.11.19.1";
      env.poll_interval = 20ms;
      env.install_timeout = 3s;
      env.prepare_dir = [](const fs::path &path) {
        std::error_code ec;
        fs::create_directories(path, ec);
        return !ec;
      };
      env.can_install = [] {
        return true;
      };
      env.stream_active = [this] {
        return streaming.load();
      };
      env.download = [this](std::string_view url, const fs::path &dest, std::uint64_t, const std::atomic<bool> &cancel, const ui::progress_fn &progress) {
        {
          std::lock_guard lock {fake_mutex};
          ++downloads;
          urls.emplace_back(url);
        }
        if (offline) {
          return ui::transfer_e::network;
        }
        const auto name = dest.filename().string();
        if (name == "SHA256SUMS.txt") {
          write_file(dest, read_file(fixtures / "SHA256SUMS.txt"));
        } else if (name == "SHA256SUMS.txt.sig") {
          auto sig = read_file(fixtures / "SHA256SUMS.txt.sig");
          if (tamper_signature) {
            sig[10] = sig[10] == 'A' ? 'B' : 'A';
          }
          write_file(dest, sig);
        } else {
          if (block_download) {
            for (int i = 0; i < 500 && !cancel.load(); ++i) {
              std::this_thread::sleep_for(10ms);
            }
            if (cancel.load()) {
              write_file(dest.string() + ".part", "partial");
              return ui::transfer_e::cancelled;
            }
          }
          auto content = installer_content();
          if (tamper_installer) {
            content[3] ^= 0x55;
          }
          write_file(dest, content);
          if (progress) {
            progress(1, 2);
            progress(2, 2);
          }
        }
        return ui::transfer_e::ok;
      };
      env.launch = [this](const fs::path &path, const std::string &hash) {
        launched_path = path;
        launched_hash = hash;
        launched = true;
        return launch_result;
      };
      env.install_result = [this]() -> std::optional<int> {
        if (installer_exit >= 0) {
          return installer_exit.load();
        }
        return std::nullopt;
      };
      env.store_marker = [this](std::string_view marker) {
        std::lock_guard lock {fake_mutex};
        markers.emplace_back(marker);
      };
      env.publish = [this](const ui::status_t &status, bool notify) {
        std::lock_guard lock {fake_mutex};
        states.push_back(status.state);
        if (notify) {
          ++notifications;
        }
      };
    }

    void TearDown() override {
      controller.reset();
      std::error_code ec;
      fs::remove_all(dir.parent_path(), ec);
    }

    ui::controller_t &make() {
      controller = std::make_unique<ui::controller_t>(env);
      return *controller;
    }

    bool wait_for(ui::state_e state, std::chrono::milliseconds limit = 5s) {
      const auto end = std::chrono::steady_clock::now() + limit;
      while (std::chrono::steady_clock::now() < end) {
        if (controller->status().state == state) {
          return true;
        }
        std::this_thread::sleep_for(10ms);
      }
      return false;
    }

    bool no_download_files() const {
      for (const auto &name : {installer_file, installer_file + ".part", "SHA256SUMS.txt"s, "SHA256SUMS.txt.sig"s}) {
        if (fs::exists(dir / name)) {
          return false;
        }
      }
      return true;
    }

    fs::path fixtures;
    fs::path dir;
    ui::environment_t env;
    std::unique_ptr<ui::controller_t> controller;
    std::mutex fake_mutex;
    std::vector<std::string> urls;
    std::vector<std::string> markers;
    std::vector<ui::state_e> states;
    int downloads = 0;
    int notifications = 0;
    bool offline = false;
    bool tamper_installer = false;
    bool tamper_signature = false;
    bool block_download = false;
    std::atomic<bool> streaming = false;
    std::atomic<bool> launched = false;
    std::atomic<int> installer_exit = -1;
    ui::launch_e launch_result = ui::launch_e::ok;
    fs::path launched_path;
    std::string launched_hash;
  };
}  // namespace

TEST(UpdateInstallHelpers, VersionTextAndNames) {
  EXPECT_EQ(ui::version_text("v2026.11.19.2"), "2026.11.19.2");
  EXPECT_EQ(ui::version_text("host-v2026.11.19.2"), "2026.11.19.2");
  EXPECT_EQ(ui::version_text("v2026.11.19.2-beta.1"), "2026.11.19.2-beta.1");
  EXPECT_FALSE(ui::version_text("v2026.11.19.2/../x"));
  EXPECT_FALSE(ui::version_text("latest"));
  EXPECT_EQ(ui::installer_name("2026.11.19.2"), installer_file);
  EXPECT_TRUE(svc::is_installer_name(ui::installer_name("2026.11.19.2")));
  EXPECT_TRUE(svc::is_installer_name(ui::installer_name("2026.11.19.2-beta.1")));
}

TEST(UpdateInstallHelpers, AssetUrlsAreAlwaysAllowed) {
  const auto urls = ui::asset_urls(tag, installer_file);
  ASSERT_EQ(urls.size(), 2u);
  EXPECT_EQ(urls[0], "https://github.com/siberanka/NyxPlay/releases/download/v2026.11.19.2/" + installer_file);
  for (const auto &url : urls) {
    EXPECT_TRUE(ui::is_allowed_url(url)) << url;
  }
  EXPECT_TRUE(ui::asset_urls("v2026.11.19.2/../../evil", installer_file).empty());
  EXPECT_TRUE(ui::asset_urls(tag, "..\\evil.exe").empty());
  EXPECT_TRUE(ui::asset_urls(tag, "").empty());
}

TEST(UpdateInstallHelpers, AllowListOfDownloadLocations) {
  EXPECT_TRUE(ui::is_allowed_url("https://objects.githubusercontent.com/github-production-release-asset/1/2?X-Amz-Signature=abc"));
  EXPECT_TRUE(ui::is_allowed_url("https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r"));
  EXPECT_TRUE(ui::is_allowed_url("https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.11.19.2/downloads/x.exe"));

  EXPECT_FALSE(ui::is_allowed_url("http://github.com/siberanka/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com/siberanka/NyxPlay/archive/main.zip"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com/other/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com.evil.example/siberanka/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://user@github.com/siberanka/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com:444/siberanka/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com/siberanka/NyxPlay/releases/download/../../../x"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com/siberanka/NyxPlay/releases/download/%2e%2e/x"));
  EXPECT_FALSE(ui::is_allowed_url("https://evilgithubusercontent.com/x"));
  EXPECT_FALSE(ui::is_allowed_url("https://objects.githubusercontent.com.evil.example/x"));
  EXPECT_FALSE(ui::is_allowed_url("https://127.0.0.1/siberanka/NyxPlay/releases/download/v1/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("file:///C:/x.exe"));
  EXPECT_FALSE(ui::is_allowed_url("https://github.com/siberanka/NyxPlay/releases/download/v1/x y.exe"));
  EXPECT_FALSE(ui::is_allowed_url(""));
}

TEST(UpdateInstallHelpers, RequestBodyNeverCarriesAnAddressOrPath) {
  EXPECT_EQ(ui::parse_request(""), std::optional<bool> {false});
  EXPECT_EQ(ui::parse_request("  \n"), std::optional<bool> {false});
  EXPECT_EQ(ui::parse_request("{}"), std::optional<bool> {false});
  EXPECT_EQ(ui::parse_request("{\"now\":true}"), std::optional<bool> {true});
  EXPECT_EQ(ui::parse_request("{\"now\":false}"), std::optional<bool> {false});

  EXPECT_FALSE(ui::parse_request("{\"url\":\"https://github.com/siberanka/NyxPlay/releases/download/v1/x.exe\"}"));
  EXPECT_FALSE(ui::parse_request("{\"path\":\"C:\\\\x.exe\"}"));
  EXPECT_FALSE(ui::parse_request("{\"now\":true,\"url\":\"https://example.com\"}"));
  EXPECT_FALSE(ui::parse_request("{\"now\":1}"));
  EXPECT_FALSE(ui::parse_request("{\"now\":\"true\"}"));
  EXPECT_FALSE(ui::parse_request("[true]"));
  EXPECT_FALSE(ui::parse_request("not json"));
  EXPECT_FALSE(ui::parse_request(std::string(2000, ' ') + "{}"));
}

TEST(UpdateInstallHelpers, UpdatedNotice) {
  EXPECT_EQ(ui::updated_notice("2026.11.19.1 2026.11.19.2", "2026.11.19.2"), std::optional<std::string> {"2026.11.19.2"});
  // The installer did not change anything, or the marker is unusable
  EXPECT_FALSE(ui::updated_notice("2026.11.19.1 2026.11.19.2", "2026.11.19.1"));
  EXPECT_FALSE(ui::updated_notice("2026.11.19.2 2026.11.19.1", "2026.11.19.1"));
  EXPECT_FALSE(ui::updated_notice("", "2026.11.19.2"));
  EXPECT_FALSE(ui::updated_notice("garbage", "2026.11.19.2"));
  EXPECT_FALSE(ui::updated_notice("2026.11.19.1 2026.11.19.2", "garbage"));
}

TEST(UpdateInstallHelpers, UpdatesFolderIsRestrictedToSystemAndAdministrators) {
  const std::string sddl = ui::updates_dir_sddl();
  EXPECT_TRUE(sddl.starts_with("D:P"));  // protected: nothing is inherited from the parent
  EXPECT_NE(sddl.find(";;;SY)"), std::string::npos);
  EXPECT_NE(sddl.find(";;;BA)"), std::string::npos);
  EXPECT_EQ(sddl.find(";;;WD)"), std::string::npos);  // not Everyone
  EXPECT_EQ(sddl.find(";;;BU)"), std::string::npos);  // not Users
  EXPECT_EQ(sddl.find(";;;AU)"), std::string::npos);  // not Authenticated Users
}

TEST(UpdateInstallHelpers, ServiceRequestFormat) {
  const std::string hash(64, 'a');
  svc::update_request_t request;
  EXPECT_TRUE(svc::parse_update_request(installer_file + "\n" + hash + "\n", request));
  EXPECT_EQ(request.name, installer_file);
  EXPECT_EQ(request.sha256, hash);

  EXPECT_FALSE(svc::parse_update_request(installer_file + "\n" + hash, request));
  EXPECT_FALSE(svc::parse_update_request(installer_file + "\r\n" + hash + "\r\n", request));
  EXPECT_FALSE(svc::parse_update_request("..\\" + installer_file + "\n" + hash + "\n", request));
  EXPECT_FALSE(svc::parse_update_request("C:\\x\\" + installer_file + "\n" + hash + "\n", request));
  EXPECT_FALSE(svc::parse_update_request("evil.exe\n" + hash + "\n", request));
  EXPECT_FALSE(svc::parse_update_request(installer_file + "\n" + std::string(63, 'a') + "\n", request));
  EXPECT_FALSE(svc::parse_update_request(installer_file + "\n" + std::string(64, 'A') + "\n", request));
  EXPECT_FALSE(svc::parse_update_request(installer_file + "\n" + hash + "\nextra\n", request));
  EXPECT_FALSE(svc::is_installer_name("NyxPlayHost-1.0-windows-x64.exe:stream"));
  EXPECT_FALSE(svc::is_installer_name("NyxPlayHost-..\\x-windows-x64.exe"));
}

TEST_F(UpdateInstall, NoCandidateMeansNothingToInstall) {
  auto &c = make();
  EXPECT_EQ(c.status().state, ui::state_e::idle);
  EXPECT_EQ(c.request_install(false), ui::request_e::no_update);
  EXPECT_FALSE(c.cancel());
  EXPECT_EQ(downloads, 0);
}

TEST_F(UpdateInstall, CandidateMakesTheUpdateAvailable) {
  auto &c = make();
  c.set_candidate(make_release());
  const auto status = c.status();
  EXPECT_EQ(status.state, ui::state_e::available);
  EXPECT_EQ(status.version, version);
  EXPECT_TRUE(status.can_install);
  EXPECT_EQ(downloads, 0);  // nothing is fetched until the user asks
}

TEST_F(UpdateInstall, InstallsTheVerifiedInstaller) {
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::installing));
  ASSERT_TRUE(launched);

  EXPECT_EQ(launched_path.filename().string(), installer_file);
  EXPECT_EQ(launched_hash, "fb5c85338c609e0c9284185f942c7683275e2248086e5824611ef30f5ecd6a58");
  ASSERT_FALSE(markers.empty());
  EXPECT_EQ(markers.front(), "2026.11.19.1 " + version);
  EXPECT_GE(notifications, 3);  // start, 50 %, verifying, installing: never one per percent
  EXPECT_LE(notifications, 6);

  // Only the release's own GitHub location is asked first
  ASSERT_GE(urls.size(), 3u);
  for (const auto &url : urls) {
    EXPECT_TRUE(ui::is_allowed_url(url)) << url;
  }
  EXPECT_EQ(c.request_install(false), ui::request_e::busy_installing);
  EXPECT_FALSE(c.cancel());  // the installer cannot be recalled
}

TEST_F(UpdateInstall, TamperedInstallerIsRejectedAndDeleted) {
  tamper_installer = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::verify_failed);
  EXPECT_FALSE(launched);
  EXPECT_TRUE(no_download_files());
}

TEST_F(UpdateInstall, TamperedSignatureIsRejectedAndDeleted) {
  tamper_signature = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::verify_failed);
  EXPECT_FALSE(launched);
  EXPECT_TRUE(no_download_files());
  // The installer itself is never fetched once the list is not trusted
  for (const auto &url : urls) {
    EXPECT_EQ(url.find(".exe"), std::string::npos) << url;
  }
}

TEST_F(UpdateInstall, FailureCanBeRetried) {
  offline = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::no_connection);

  offline = false;
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::installing));
  EXPECT_EQ(c.status().error, ui::error_e::none);
}

TEST_F(UpdateInstall, CancelDuringDownload) {
  block_download = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  while (true) {
    std::lock_guard lock {fake_mutex};
    if (urls.size() >= 3) {
      break;
    }
  }
  EXPECT_EQ(c.request_install(false), ui::request_e::already_running);  // one installation at a time
  EXPECT_TRUE(c.cancel());
  ASSERT_TRUE(wait_for(ui::state_e::available));
  EXPECT_EQ(c.status().error, ui::error_e::none);
  EXPECT_FALSE(launched);
  EXPECT_TRUE(no_download_files());

  block_download = false;
  EXPECT_EQ(c.request_install(false), ui::request_e::started);  // and it can be started again
}

TEST_F(UpdateInstall, ShutdownDuringDownloadCleansUp) {
  block_download = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  while (true) {
    std::lock_guard lock {fake_mutex};
    if (urls.size() >= 3) {
      break;
    }
  }
  c.shutdown();
  EXPECT_FALSE(launched);
  EXPECT_TRUE(no_download_files());
  EXPECT_EQ(c.request_install(false), ui::request_e::no_update);
}

TEST_F(UpdateInstall, WaitsForTheStreamToEnd) {
  streaming = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::waiting_for_stream));
  std::this_thread::sleep_for(150ms);
  EXPECT_FALSE(launched);
  EXPECT_TRUE(c.status().stream_active);

  streaming = false;
  ASSERT_TRUE(wait_for(ui::state_e::installing));
  EXPECT_TRUE(launched);
}

TEST_F(UpdateInstall, InstallNowDoesNotWait) {
  streaming = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::waiting_for_stream));
  EXPECT_EQ(c.request_install(true), ui::request_e::already_running);
  ASSERT_TRUE(wait_for(ui::state_e::installing));
  EXPECT_TRUE(launched);
}

TEST_F(UpdateInstall, WaitingCanBeCancelled) {
  streaming = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::waiting_for_stream));
  EXPECT_TRUE(c.cancel());
  ASSERT_TRUE(wait_for(ui::state_e::available));
  EXPECT_FALSE(launched);
  EXPECT_TRUE(no_download_files());
}

TEST_F(UpdateInstall, CannotInstallWithoutTheRights) {
  env.can_install = [] {
    return false;
  };
  auto &c = make();
  c.set_candidate(make_release());
  EXPECT_FALSE(c.status().can_install);
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::not_supported);
  EXPECT_EQ(downloads, 0);  // nothing is downloaded for an installation that cannot run
}

TEST_F(UpdateInstall, ServiceNotAvailable) {
  launch_result = ui::launch_e::not_supported;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::not_supported);
  EXPECT_TRUE(no_download_files());
  ASSERT_GE(markers.size(), 2u);
  EXPECT_TRUE(markers.back().empty());  // the "updated" notice must not appear after a failed start
}

TEST_F(UpdateInstall, InstallerFailureIsReported) {
  installer_exit = 5;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::failed));
  EXPECT_EQ(c.status().error, ui::error_e::installer_failed);
  EXPECT_TRUE(markers.back().empty());
}

TEST_F(UpdateInstall, NewCandidateWhileRunningIsIgnored) {
  streaming = true;
  auto &c = make();
  c.set_candidate(make_release());
  ASSERT_EQ(c.request_install(false), ui::request_e::started);
  ASSERT_TRUE(wait_for(ui::state_e::waiting_for_stream));
  auto newer = make_release();
  newer.tag = "v2026.11.20.1";
  c.set_candidate(newer);
  EXPECT_EQ(c.status().version, version);
  EXPECT_EQ(c.status().state, ui::state_e::waiting_for_stream);
}
