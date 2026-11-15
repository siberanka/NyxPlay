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
