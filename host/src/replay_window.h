/**
 * @file src/replay_window.h
 * @brief Declarations for the sliding window that rejects replayed messages of an authenticated stream.
 */
#pragma once

// standard includes
#include <bitset>
#include <cstdint>

/**
 * @brief Anti-replay window over 32-bit message sequence numbers.
 * @details The newest accepted number and a bitmap of the numbers before it are kept. A number is
 *          accepted once. Numbers may arrive out of order (the control stream sends on several ENet
 *          channels, so a retransmission on one channel is overtaken by newer messages on another),
 *          as long as they are not older than `size` numbers behind the newest.
 *          Not thread safe.
 */
class replay_window_t {
public:
  /// How far behind the newest number a message may still arrive.
  static constexpr std::uint32_t size = 1U << 16;

  /**
   * @return `true` if `seq` was not accepted before and is not too old. Does not change the window.
   */
  bool fresh(std::uint32_t seq) const {
    if (!started_) {
      return true;
    }

    const auto ahead = static_cast<std::int32_t>(seq - newest_);
    if (ahead > 0) {
      return true;
    }

    const auto behind = static_cast<std::uint32_t>(-static_cast<std::int64_t>(ahead));
    return behind < size && !seen_.test(seq % size);
  }

  /**
   * @brief Accept `seq` if it is fresh.
   * @return `false` if the number was seen before or is too old (nothing changes then).
   */
  bool accept(std::uint32_t seq) {
    if (!fresh(seq)) {
      return false;
    }

    if (!started_) {
      started_ = true;
      newest_ = seq;
      seen_.set(seq % size);
      return true;
    }

    const auto ahead = static_cast<std::int32_t>(seq - newest_);
    if (ahead > 0) {
      // The numbers between the old and the new newest are not seen yet
      const std::uint32_t steps = static_cast<std::uint32_t>(ahead) < size ? static_cast<std::uint32_t>(ahead) : size;
      for (std::uint32_t i = 1; i <= steps; ++i) {
        seen_.reset((newest_ + i) % size);
      }
      newest_ = seq;
    }

    seen_.set(seq % size);
    return true;
  }

private:
  bool started_ = false;
  std::uint32_t newest_ = 0;
  std::bitset<size> seen_;
};
