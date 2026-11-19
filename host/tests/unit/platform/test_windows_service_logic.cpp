/**
 * @file tests/unit/platform/test_windows_service_logic.cpp
 * @brief Test the platform independent helpers of the Windows service.
 */
#include "tools/svc_logic.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
  std::vector<std::uint32_t> run_failures(svc::restart_backoff_t &backoff, int count, std::uint64_t ran_ms = 0) {
    std::vector<std::uint32_t> delays;
    for (int i = 0; i < count; i++) {
      delays.push_back(backoff.on_stopped(ran_ms));
    }
    return delays;
  }
}  // namespace

TEST(ServiceBackoff, Sequence) {
  svc::restart_backoff_t backoff;
  const std::vector<std::uint32_t> expected {3000, 6000, 12000, 24000, 48000, 60000, 60000};
  EXPECT_EQ(run_failures(backoff, 7), expected);
}

TEST(ServiceBackoff, ResetsAfterHealthyRun) {
  svc::restart_backoff_t backoff;
  run_failures(backoff, 5);
  // A run that stayed up long enough starts over at the initial delay
  EXPECT_EQ(backoff.on_stopped(svc::backoff_healthy_run_ms), 3000u);
  EXPECT_EQ(backoff.on_stopped(1000), 6000u);
}

TEST(ServiceBackoff, ShortRunDoesNotReset) {
  svc::restart_backoff_t backoff;
  run_failures(backoff, 3);
  EXPECT_EQ(backoff.on_stopped(svc::backoff_healthy_run_ms - 1), 24000u);
}

TEST(ServiceBackoff, NoOverflow) {
  svc::restart_backoff_t backoff;
  auto delays = run_failures(backoff, 1000);
  EXPECT_EQ(delays.back(), svc::backoff_max_ms);
  for (auto delay : delays) {
    EXPECT_LE(delay, svc::backoff_max_ms);
  }
}

TEST(ServiceBackoff, ExplicitReset) {
  svc::restart_backoff_t backoff;
  run_failures(backoff, 4);
  backoff.reset();
  EXPECT_EQ(backoff.on_stopped(0), 3000u);
}

TEST(ServiceLogRotate, Threshold) {
  EXPECT_FALSE(svc::should_rotate_log(0));
  EXPECT_FALSE(svc::should_rotate_log(svc::log_rotate_bytes));
  EXPECT_TRUE(svc::should_rotate_log(svc::log_rotate_bytes + 1));
  EXPECT_EQ(svc::log_rotate_bytes, 10ull * 1024 * 1024);
}

TEST(ServiceModulePath, ShortPath) {
  const std::wstring path = L"C:\\Program Files\\NyxPlay Host\\tools\\nyxplay-host-svc.exe";
  auto result = svc::read_growing([&](wchar_t *buffer, unsigned long size) -> unsigned long {
    if (path.size() >= size) {
      // GetModuleFileName contract: truncated, returns the buffer size
      std::copy_n(path.begin(), size, buffer);
      return size;
    }
    std::copy(path.begin(), path.end(), buffer);
    buffer[path.size()] = L'\0';
    return static_cast<unsigned long>(path.size());
  });
  EXPECT_EQ(result, path);
}

TEST(ServiceModulePath, LongPath) {
  std::wstring path = L"C:\\";
  while (path.size() < 300) {
    path += L"folder\\";
  }
  path += L"nyxplay-host-svc.exe";
  int calls = 0;
  auto result = svc::read_growing([&](wchar_t *buffer, unsigned long size) -> unsigned long {
    calls++;
    if (path.size() >= size) {
      std::copy_n(path.begin(), size, buffer);
      return size;
    }
    std::copy(path.begin(), path.end(), buffer);
    buffer[path.size()] = L'\0';
    return static_cast<unsigned long>(path.size());
  });
  EXPECT_EQ(result, path);
  EXPECT_GT(calls, 1);
}

TEST(ServiceModulePath, FailureAndLimit) {
  EXPECT_TRUE(svc::read_growing([](wchar_t *, unsigned long) -> unsigned long {
                return 0;
              })
                .empty());
  // An API that always reports truncation ends at the 32767 character limit
  EXPECT_TRUE(svc::read_growing([](wchar_t *, unsigned long size) -> unsigned long {
                return size;
              })
                .empty());
}

TEST(ServicePath, StripComponents) {
  const std::wstring module = L"C:\\Program Files\\NyxPlay Host\\tools\\nyxplay-host-svc.exe";
  EXPECT_EQ(svc::strip_components(module, 2), L"C:\\Program Files\\NyxPlay Host");
  EXPECT_EQ(svc::strip_components(module, 0), module);
  EXPECT_EQ(svc::strip_components(L"nyxplay-host-svc.exe", 2), L"nyxplay-host-svc.exe");
  EXPECT_EQ(svc::strip_components(L"C:\\a\\Şükrü İğdır\\tools\\x.exe", 2), L"C:\\a\\Şükrü İğdır");
}

TEST(ServiceWatchdog, ReadyAsSoonAsListening) {
  svc::startup_watchdog_t watchdog;
  EXPECT_EQ(watchdog.evaluate(0, true), svc::host_state_t::ready);
  EXPECT_EQ(watchdog.evaluate(svc::ready_grace_initial_ms * 10, true), svc::host_state_t::ready);
}

TEST(ServiceWatchdog, StartingWithinGrace) {
  svc::startup_watchdog_t watchdog;
  EXPECT_EQ(watchdog.evaluate(0, false), svc::host_state_t::starting);
  EXPECT_EQ(watchdog.evaluate(svc::ready_grace_initial_ms - 1, false), svc::host_state_t::starting);
}

TEST(ServiceWatchdog, StuckAfterGrace) {
  svc::startup_watchdog_t watchdog;
  EXPECT_EQ(watchdog.evaluate(svc::ready_grace_initial_ms, false), svc::host_state_t::stuck);
}

TEST(ServiceWatchdog, GraceGrowsAndIsBounded) {
  svc::startup_watchdog_t watchdog;
  watchdog.on_stuck();
  EXPECT_EQ(watchdog.grace_ms(), svc::ready_grace_initial_ms * 2);
  EXPECT_EQ(watchdog.evaluate(svc::ready_grace_initial_ms, false), svc::host_state_t::starting);
  for (int i = 0; i < 20; i++) {
    watchdog.on_stuck();
  }
  EXPECT_EQ(watchdog.grace_ms(), svc::ready_grace_max_ms);
}

TEST(ServiceWatchdog, ReadyResetsGrace) {
  svc::startup_watchdog_t watchdog;
  watchdog.on_stuck();
  watchdog.on_stuck();
  watchdog.on_ready();
  EXPECT_EQ(watchdog.grace_ms(), svc::ready_grace_initial_ms);
}

TEST(ServiceLoaderFailure, Describes) {
  for (auto code : {svc::status_dll_not_found, svc::status_entrypoint_not_found, svc::status_invalid_image_format}) {
    ASSERT_NE(svc::describe_loader_failure(code), nullptr);
    EXPECT_NE(std::string(svc::describe_loader_failure(code)).find("reinstall NyxPlay Host"), std::string::npos);
  }
  EXPECT_NE(std::string(svc::describe_loader_failure(0xC0000135u)).find("DLL is missing"), std::string::npos);
  EXPECT_EQ(svc::describe_loader_failure(0), nullptr);
  EXPECT_EQ(svc::describe_loader_failure(1), nullptr);
  EXPECT_EQ(svc::describe_loader_failure(0xC0000005u), nullptr);
}

TEST(ServiceLoaderFailure, ReportedOncePerSeries) {
  svc::loader_failure_reporter_t reporter;
  EXPECT_NE(reporter.on_exit(svc::status_dll_not_found, 200), nullptr);
  EXPECT_EQ(reporter.on_exit(svc::status_dll_not_found, 150), nullptr);
  EXPECT_EQ(reporter.on_exit(svc::status_dll_not_found, 150), nullptr);
  // A different loader failure in the same series is still the same series
  EXPECT_EQ(reporter.on_exit(svc::status_invalid_image_format, 150), nullptr);
}

TEST(ServiceLoaderFailure, NewSeriesAfterOtherExitOrHealthyRun) {
  svc::loader_failure_reporter_t reporter;
  EXPECT_NE(reporter.on_exit(svc::status_dll_not_found, 100), nullptr);
  // An unrelated exit ends the series
  EXPECT_EQ(reporter.on_exit(1, 100), nullptr);
  EXPECT_NE(reporter.on_exit(svc::status_dll_not_found, 100), nullptr);
  // So does a long run
  EXPECT_EQ(reporter.on_exit(svc::status_dll_not_found, svc::backoff_healthy_run_ms), nullptr);
  EXPECT_NE(reporter.on_exit(svc::status_entrypoint_not_found, 100), nullptr);
  // And a planned restart
  reporter.reset();
  EXPECT_NE(reporter.on_exit(svc::status_entrypoint_not_found, 100), nullptr);
}
