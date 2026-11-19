/**
 * @file src/control_intercept.h
 * @brief Declarations for the inspection of raw datagrams of the control stream before ENet handles them.
 *
 * ENet addresses a datagram to an established connection by peer index and a 2-bit session id only.
 * The inspection makes sure that
 *  - a datagram from an address other than the one a connection is bound to cannot disconnect, reset or
 *    re-address it, and moves the connection (NAT re-mapping of a roaming client) only when it carries
 *    messages that prove knowledge of the session key;
 *  - a connection request is only accepted when a launched stream waits for it, so that bogus requests
 *    cannot fill the peer table.
 * It has no dependency besides ENet so it can be tested on its own.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

// lib includes
#include <enet/enet.h>

namespace control_intercept {
  /// Most messages that are authenticated for one datagram from a foreign address. A client whose address changed
  /// sends what it could not deliver together with new messages, and a datagram holds up to a few dozen of them.
  inline constexpr std::size_t max_foreign_messages = 128;

  /// Result of the authentication of one control message.
  enum class message_e {
    invalid,  ///< Not a valid message of the client (malformed, wrong key, tampered).
    replayed,  ///< A valid message that was already accepted before.
    fresh,  ///< A valid message that was not accepted before.
  };

  /**
   * @brief What the inspection needs to know about the streams of the host.
   */
  struct hooks_t {
    /// Whether a launched stream waits for a connection with this connect data from this address.
    std::function<bool(std::uint32_t connect_data, const ENetAddress &source)> connect_expected;

    /// Authenticate `message` (the data of one ENet packet) for the client the established `peer` belongs to.
    /// It must not change any state.
    std::function<message_e(ENetPeer *peer, std::string_view message)> authenticate;
  };

  /// Counters since the last call of take_report().
  struct report_t {
    std::size_t foreign_datagrams = 0;  ///< Dropped: foreign source without a valid message.
    std::size_t moved_peers = 0;  ///< Connections moved to a new source after a valid message.
    std::size_t rejected_connects = 0;  ///< Dropped: connection request nobody waits for.
    ENetAddress last_foreign {};
    ENetAddress last_rejected {};
  };

  /**
   * @brief Install the hooks used by filter(). The object must stay alive until it is replaced or cleared.
   * @param hooks The hooks, or `nullptr` to remove them (all foreign datagrams and connection requests are then dropped).
   */
  void set_hooks(const hooks_t *hooks);

  /**
   * @brief ENet intercept callback.
   * @return 1 to drop the datagram, 0 to let ENet handle it.
   */
  int filter(ENetHost *host, ENetEvent *event);

  /// @brief Read and reset the counters.
  report_t take_report();
}  // namespace control_intercept
