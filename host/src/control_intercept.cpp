/**
 * @file src/control_intercept.cpp
 * @brief Definitions for the inspection of raw datagrams of the control stream before ENet handles them.
 */
// standard includes
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

// local includes
#include "control_intercept.h"

namespace control_intercept {
  namespace {
    std::atomic<const hooks_t *> active_hooks {nullptr};

    std::mutex report_mutex;
    report_t pending_report;

    std::uint16_t read_be16(const std::uint8_t *data) {
      return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    }

    std::uint32_t read_be32(const std::uint8_t *data) {
      return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
    }

    int drop_foreign(const ENetAddress &source) {
      std::lock_guard lg {report_mutex};
      ++pending_report.foreign_datagrams;
      pending_report.last_foreign = source;
      return 1;
    }

    /**
     * @brief Handle the datagram that starts a new connection.
     */
    int filter_connect(ENetHost *host, std::size_t header_size) {
      const auto *data = static_cast<const std::uint8_t *>(host->receivedData);
      const auto length = host->receivedDataLength;

      if (length < header_size + sizeof(ENetProtocolConnect)) {
        return 0;  // not a complete connection request, ENet drops it
      }

      const auto *command = data + header_size;
      if ((command[0] & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_CONNECT) {
        return 0;  // ENet ignores a first command that is not a connection request
      }

      const auto connect_data = read_be32(command + offsetof(ENetProtocolConnect, data));
      const auto *hooks = active_hooks.load(std::memory_order_acquire);
      if (hooks != nullptr && hooks->connect_expected && hooks->connect_expected(connect_data, host->receivedPeerAddress)) {
        return 0;
      }

      std::lock_guard lg {report_mutex};
      ++pending_report.rejected_connects;
      pending_report.last_rejected = host->receivedPeerAddress;
      return 1;
    }

    /**
     * @brief Handle a datagram for an established connection that was not sent from its bound address.
     * @details The connection is moved to the new source only if the datagram consists of payload (and the
     *          acknowledgements/pings a client piggybacks on it) and every payload message authenticates.
     */
    int filter_foreign(ENetHost *host, ENetPeer *peer, std::uint16_t header_id, std::size_t header_size) {
      const auto *data = static_cast<const std::uint8_t *>(host->receivedData);
      const auto length = host->receivedDataLength;

      const auto *hooks = active_hooks.load(std::memory_order_acquire);
      const auto session_id = static_cast<std::uint8_t>((header_id & ENET_PROTOCOL_HEADER_SESSION_MASK) >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);

      if (hooks == nullptr || !hooks->authenticate || peer->state != ENET_PEER_STATE_CONNECTED || session_id != peer->incomingSessionID || host->checksum != nullptr || (header_id & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED) != 0) {
        return drop_foreign(host->receivedPeerAddress);
      }

      std::vector<std::string_view> messages;
      std::size_t offset = header_size;

      while (offset < length) {
        if (offset + sizeof(ENetProtocolCommandHeader) > length) {
          return drop_foreign(host->receivedPeerAddress);
        }

        const auto *command = data + offset;
        std::size_t command_size = 0;
        std::size_t payload_length_offset = 0;
        switch (command[0] & ENET_PROTOCOL_COMMAND_MASK) {
          case ENET_PROTOCOL_COMMAND_ACKNOWLEDGE:
            command_size = sizeof(ENetProtocolAcknowledge);
            break;
          case ENET_PROTOCOL_COMMAND_PING:
            command_size = sizeof(ENetProtocolPing);
            break;
          case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
            command_size = sizeof(ENetProtocolSendReliable);
            payload_length_offset = offsetof(ENetProtocolSendReliable, dataLength);
            break;
          case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
            command_size = sizeof(ENetProtocolSendUnreliable);
            payload_length_offset = offsetof(ENetProtocolSendUnreliable, dataLength);
            break;
          case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
            command_size = sizeof(ENetProtocolSendUnsequenced);
            payload_length_offset = offsetof(ENetProtocolSendUnsequenced, dataLength);
            break;
          default:
            // DISCONNECT, CONNECT, VERIFY_CONNECT, bandwidth and throttle changes, fragments: never from a foreign source
            return drop_foreign(host->receivedPeerAddress);
        }

        if (offset + command_size > length) {
          return drop_foreign(host->receivedPeerAddress);
        }

        if (payload_length_offset != 0) {
          const std::size_t payload_length = read_be16(command + payload_length_offset);
          if (offset + command_size + payload_length > length || messages.size() >= max_foreign_messages) {
            return drop_foreign(host->receivedPeerAddress);
          }
          messages.emplace_back(reinterpret_cast<const char *>(command + command_size), payload_length);
          offset += payload_length;
        }

        offset += command_size;
      }

      // A client whose address changed keeps resending what the host did not acknowledge, so a datagram may
      // carry messages that were accepted before. Those are fine next to a new one, they prove nothing alone.
      bool any_fresh = false;
      for (const auto &message : messages) {
        const auto state = hooks->authenticate(peer, message);
        if (state == message_e::invalid) {
          return drop_foreign(host->receivedPeerAddress);
        }
        any_fresh = any_fresh || state == message_e::fresh;
      }

      if (!any_fresh) {
        return drop_foreign(host->receivedPeerAddress);
      }

      // Proven: the client is behind a new address. ENet takes the address from the datagram it handles now.
      peer->address = host->receivedPeerAddress;
      peer->localAddress = host->receivedLocalAddress;
      {
        std::lock_guard lg {report_mutex};
        ++pending_report.moved_peers;
        pending_report.last_foreign = host->receivedPeerAddress;
      }
      return 0;
    }
  }  // namespace

  void set_hooks(const hooks_t *hooks) {
    active_hooks.store(hooks, std::memory_order_release);
  }

  report_t take_report() {
    std::lock_guard lg {report_mutex};
    auto report = pending_report;
    pending_report = report_t {};
    return report;
  }

  int filter(ENetHost *host, ENetEvent *) {
    if (host->receivedData == nullptr || host->receivedDataLength < sizeof(std::uint16_t)) {
      return 0;
    }

    const auto *data = static_cast<const std::uint8_t *>(host->receivedData);
    const auto header_id = read_be16(data);
    const auto peer_id = static_cast<std::uint16_t>(header_id & ~(ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK));
    const std::size_t header_size = (header_id & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? sizeof(ENetProtocolHeader) : offsetof(ENetProtocolHeader, sentTime);

    if (peer_id == ENET_PROTOCOL_MAXIMUM_PEER_ID) {
      return filter_connect(host, header_size);
    }

    if (peer_id >= host->peerCount) {
      return 0;
    }

    auto *peer = &host->peers[peer_id];
    if (peer->state == ENET_PEER_STATE_DISCONNECTED || peer->state == ENET_PEER_STATE_ZOMBIE || enet_address_equal(&host->receivedPeerAddress, &peer->address)) {
      return 0;
    }

    return filter_foreign(host, peer, header_id, header_size);
  }
}  // namespace control_intercept
