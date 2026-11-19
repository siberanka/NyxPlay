/**
 * @file tests/unit/test_system_tray.cpp
 * @brief Test the start-up helpers in src/system_tray.*.
 */
// test imports
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <thread>

// local imports
#include <src/system_tray.h>

#if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1

using namespace std::literals;

TEST(SystemTrayShellWait, ReturnsImmediatelyWhenShellIsUp) {
  std::atomic<bool> stop = false;
  EXPECT_TRUE(system_tray::wait_for_shell([] { return true; }, stop, 10ms));
}

TEST(SystemTrayShellWait, StopEndsTheWaitPromptly) {
  std::atomic<bool> stop = false;
  std::thread stopper([&stop] {
    std::this_thread::sleep_for(150ms);
    stop = true;
  });

  const auto start = std::chrono::steady_clock::now();
  // A long poll interval must not delay the stop
  EXPECT_FALSE(system_tray::wait_for_shell([] { return false; }, stop, 5s));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  stopper.join();
}

TEST(SystemTrayShellWait, ShellAppearingLateIsStillAccepted) {
  // The shell shows up after the old 10 s start-up timeout would have given up
  std::atomic<bool> stop = false;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(system_tray::wait_for_shell([&] { return std::chrono::steady_clock::now() - start > 11s; }, stop, 500ms));
  EXPECT_GT(std::chrono::steady_clock::now() - start, 11s);
}

TEST(SystemTrayPendingNotice, KeepsOnlyTheNewestAndHandsItOutOnce) {
  ASSERT_TRUE(system_tray::take_pending_update_notice().empty());

  // The tray is not created in unit tests, so notices are queued
  system_tray::update_tray_update_available("host-v2026.11.20.1");
  system_tray::update_tray_update_available("host-v2026.11.21.1");

  EXPECT_EQ(system_tray::take_pending_update_notice(), "host-v2026.11.21.1");
  EXPECT_TRUE(system_tray::take_pending_update_notice().empty());
}

#endif
