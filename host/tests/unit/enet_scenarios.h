/**
 * @file tests/unit/enet_scenarios.h
 * @brief Loopback scenarios for the inspection of control stream datagrams (src/control_intercept.*).
 * @details Free of the test framework so the same code runs inside the unit tests and as a standalone
 *          program under sanitizers. Every scenario returns the number of failed checks and prints each failure.
 */
#pragma once

#include <enet/enet.h>
#include <src/control_intercept.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string_view>
#include <string>
#include <vector>

#define SCN_CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++scn_failures; \
    } \
  } while (0)

namespace scn {
  struct pair_t {
    ENetHost *server = nullptr;
    ENetHost *client = nullptr;
    ENetPeer *client_peer = nullptr;
    ENetPeer *server_peer = nullptr;
    ENetAddress server_address {};
    int server_connects = 0;
    int server_disconnects = 0;
    int client_disconnects = 0;
    std::vector<std::string> received;
    control_intercept::hooks_t hooks;
  };

  inline void set_loopback(ENetAddress *address, unsigned short port) {
    std::memset(address, 0, sizeof(*address));
    enet_address_set_host(address, "127.0.0.1");
    enet_address_set_port(address, port);
  }

  /// Service the hosts for roughly `rounds * 5` milliseconds and record what the server saw.
  inline void pump(pair_t &pair, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      ENetEvent event;
      while (pair.client != nullptr && enet_host_service(pair.client, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
          ++pair.client_disconnects;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
          enet_packet_destroy(event.packet);
        }
      }
      while (enet_host_service(pair.server, &event, 5) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
          ++pair.server_connects;
          pair.server_peer = event.peer;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
          ++pair.server_disconnects;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
          pair.received.emplace_back(reinterpret_cast<const char *>(event.packet->data), event.packet->dataLength);
          enet_packet_destroy(event.packet);
        }
      }
    }
  }

  /// A host set up like the control stream host: 32 peers, eight connections per address, the inspection installed.
  inline ENetHost *make_server(pair_t &pair) {
    enet_initialize();
    control_intercept::take_report();
    ENetAddress bind_address;
    set_loopback(&bind_address, 0);
    pair.server = enet_host_create(AF_INET, &bind_address, 32, 0, 0, 0);
    if (pair.server != nullptr) {
      pair.server->duplicatePeers = 8;
      pair.server->intercept = control_intercept::filter;
      control_intercept::set_hooks(&pair.hooks);
      pair.server_address = pair.server->address;
    }
    return pair.server;
  }

  /// Connect a client to the server of `pair`.
  inline int connect_client(pair_t &pair, unsigned int connect_data) {
    pair.client = enet_host_create(AF_INET, nullptr, 1, 0, 0, 0);
    if (pair.client == nullptr) {
      return -1;
    }
    pair.client_peer = enet_host_connect(pair.client, &pair.server_address, 1, connect_data);
    if (pair.client_peer == nullptr) {
      return -1;
    }
    // The first connection request may need its retransmission (about half a second) to get through
    for (int round = 0; round < 600 && pair.client_peer->state != ENET_PEER_STATE_CONNECTED; ++round) {
      pump(pair, 1);
    }
    pump(pair, 4);
    return pair.server_peer != nullptr && pair.client_peer->state == ENET_PEER_STATE_CONNECTED ? 0 : -1;
  }

  /// Start a server and connect one client whose connect data the hooks expect.
  inline int open(pair_t &pair, unsigned int connect_data) {
    pair.hooks.connect_expected = [connect_data](std::uint32_t data, const ENetAddress &) {
      return data == connect_data;
    };
    if (make_server(pair) == nullptr) {
      return -1;
    }
    return connect_client(pair, connect_data);
  }

  inline void close(pair_t &pair) {
    control_intercept::set_hooks(nullptr);
    if (pair.client != nullptr) {
      enet_host_destroy(pair.client);
    }
    if (pair.server != nullptr) {
      enet_host_destroy(pair.server);
    }
    pair = pair_t {};
  }

  /// A second UDP socket on loopback: the "other source".
  struct injector_t {
    ENetSocket socket = ENET_SOCKET_NULL;
    ENetAddress address {};

    injector_t() = default;
    injector_t(const injector_t &) = delete;
    injector_t &operator=(const injector_t &) = delete;

    bool open() {
      ENetAddress bind_address;
      set_loopback(&bind_address, 0);
      socket = enet_socket_create(AF_INET, ENET_SOCKET_TYPE_DATAGRAM);
      if (socket == ENET_SOCKET_NULL) {
        return false;
      }
      if (enet_socket_bind(socket, &bind_address) < 0 || enet_socket_get_address(socket, &address) < 0) {
        return false;
      }
      enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
      return true;
    }

    ~injector_t() {
      if (socket != ENET_SOCKET_NULL) {
        enet_socket_destroy(socket);
      }
    }

    void send(const ENetAddress &target, const std::vector<unsigned char> &datagram) {
      ENetBuffer buffer;
      buffer.data = const_cast<unsigned char *>(datagram.data());
      buffer.dataLength = datagram.size();
      enet_socket_send(socket, &target, nullptr, &buffer, 1);
    }

    /// @return Size of the next datagram received (0 when none arrived).
    int receive(int wait_ms) {
      unsigned char data[2048];
      ENetBuffer buffer;
      ENetAddress from;
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      buffer.data = data;
      buffer.dataLength = sizeof(data);
      if (enet_socket_wait(socket, &condition, wait_ms) != 0 || !(condition & ENET_SOCKET_WAIT_RECEIVE)) {
        return 0;
      }
      const int length = enet_socket_receive(socket, &from, nullptr, &buffer, 1);
      return length > 0 ? length : 0;
    }
  };

  inline void put16(std::vector<unsigned char> &out, unsigned int value) {
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value & 0xFF));
  }

  inline void put32(std::vector<unsigned char> &out, std::uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xFFFF);
  }

  /// Header of a datagram addressed to the established peer of the server (no sent time, no checksum).
  inline std::vector<unsigned char> header(const pair_t &pair, unsigned int session_id) {
    std::vector<unsigned char> out;
    put16(out, pair.server_peer->incomingPeerID | (session_id << ENET_PROTOCOL_HEADER_SESSION_SHIFT));
    return out;
  }

  /// DISCONNECT without the acknowledge flag, the datagram that ends a connection on stock ENet.
  inline void add_disconnect(std::vector<unsigned char> &out) {
    const unsigned char command[] = {ENET_PROTOCOL_COMMAND_DISCONNECT, 0xFF, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    out.insert(out.end(), std::begin(command), std::end(command));
  }

  inline void add_ping(std::vector<unsigned char> &out) {
    const unsigned char command[] = {ENET_PROTOCOL_COMMAND_PING, 0xFF, 0x00, 0x01};
    out.insert(out.end(), std::begin(command), std::end(command));
  }

  inline void add_bandwidth_limit(std::vector<unsigned char> &out) {
    const unsigned char command[] = {ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT, 0xFF, 0x00, 0x01, 0, 0, 0, 1, 0, 0, 0, 1};
    out.insert(out.end(), std::begin(command), std::end(command));
  }

  inline void add_unsequenced(std::vector<unsigned char> &out, unsigned int group, const std::string &payload) {
    out.push_back(ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED);
    out.push_back(0);  // channel
    put16(out, 1);  // reliable sequence number
    put16(out, group);
    put16(out, static_cast<unsigned int>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
  }

  inline void add_reliable(std::vector<unsigned char> &out, unsigned int reliable_sequence, const std::string &payload) {
    out.push_back(ENET_PROTOCOL_COMMAND_SEND_RELIABLE);
    out.push_back(0);  // channel
    put16(out, reliable_sequence);
    put16(out, static_cast<unsigned int>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
  }

  /// A connection request as a client would send it first (sent time set, acknowledge requested).
  inline std::vector<unsigned char> connect_request(std::uint32_t connect_id, std::uint32_t connect_data) {
    std::vector<unsigned char> out;
    put16(out, ENET_PROTOCOL_MAXIMUM_PEER_ID | ENET_PROTOCOL_HEADER_FLAG_SENT_TIME);
    put16(out, 0x1234);  // sent time
    out.push_back(ENET_PROTOCOL_COMMAND_CONNECT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE);
    out.push_back(0xFF);
    put16(out, 1);
    put16(out, 0);  // outgoing peer id
    out.push_back(0xFF);  // incoming session id
    out.push_back(0xFF);  // outgoing session id
    put32(out, 1400);  // mtu
    put32(out, 4096);  // window size
    put32(out, 1);  // channel count
    put32(out, 0);  // incoming bandwidth
    put32(out, 0);  // outgoing bandwidth
    put32(out, 5000);  // throttle interval
    put32(out, 2);  // throttle acceleration
    put32(out, 2);  // throttle deceleration
    put32(out, connect_id);
    put32(out, connect_data);
    return out;
  }

  inline int peers_in_use(const ENetHost *host) {
    int used = 0;
    for (size_t i = 0; i < host->peerCount; ++i) {
      used += host->peers[i].state != ENET_PEER_STATE_DISCONNECTED ? 1 : 0;
    }
    return used;
  }

  /// Test authentication used where no real cipher is involved: "AUTH" + decimal sequence number.
  struct token_auth_t {
    std::set<unsigned int> accepted;

    control_intercept::message_e check(std::string_view message) const {
      if (message.size() < 5 || message.substr(0, 4) != "AUTH") {
        return control_intercept::message_e::invalid;
      }
      const unsigned int seq = static_cast<unsigned int>(std::stoul(std::string(message.substr(4))));
      return accepted.count(seq) != 0 ? control_intercept::message_e::replayed : control_intercept::message_e::fresh;
    }
  };

  /// A DISCONNECT, PING and BANDWIDTH_LIMIT from another source must change nothing.
  inline int foreign_commands_are_ignored() {
    int scn_failures = 0;
    pair_t pair;
    injector_t injector;

    SCN_CHECK(open(pair, 0x11223344) == 0);
    SCN_CHECK(injector.open());
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }

    const ENetAddress bound = pair.server_peer->address;
    const auto bandwidth_before = pair.server_peer->incomingBandwidth;
    pair.hooks.authenticate = [](ENetPeer *, std::string_view) {
      return control_intercept::message_e::invalid;
    };

    for (unsigned int session_id = 0; session_id < 4; ++session_id) {
      for (int kind = 0; kind < 3; ++kind) {
        auto datagram = header(pair, session_id);
        kind == 0 ? add_disconnect(datagram) : kind == 1 ? add_ping(datagram) : add_bandwidth_limit(datagram);
        injector.send(pair.server_address, datagram);
      }
      pump(pair, 4);
    }

    SCN_CHECK(pair.server_disconnects == 0);
    SCN_CHECK(pair.client_disconnects == 0);
    SCN_CHECK(pair.server_peer->state == ENET_PEER_STATE_CONNECTED);
    SCN_CHECK(pair.client_peer->state == ENET_PEER_STATE_CONNECTED);
    SCN_CHECK(enet_address_equal(const_cast<ENetAddress *>(&bound), &pair.server_peer->address));
    SCN_CHECK(pair.server_peer->incomingBandwidth == bandwidth_before);
    SCN_CHECK(pair.received.empty());
    const auto report = control_intercept::take_report();
    SCN_CHECK(report.foreign_datagrams >= 12);
    SCN_CHECK(report.moved_peers == 0);

    // The real client keeps working
    enet_peer_send(pair.client_peer, 0, enet_packet_create("hello", 5, ENET_PACKET_FLAG_RELIABLE));
    pump(pair, 6);
    SCN_CHECK(pair.received.size() == 1);
    SCN_CHECK(!pair.received.empty() && pair.received[0] == "hello");

    close(pair);
    return scn_failures;
  }

  /// Payload that does not authenticate is dropped; a command that must not come from a foreign source poisons the datagram.
  inline int foreign_payload_needs_authentication() {
    int scn_failures = 0;
    pair_t pair;
    injector_t injector;
    token_auth_t auth;

    SCN_CHECK(open(pair, 0x01020304) == 0);
    SCN_CHECK(injector.open());
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }
    pair.hooks.authenticate = [&auth](ENetPeer *, std::string_view message) {
      return auth.check(message);
    };
    const ENetAddress bound = pair.server_peer->address;
    const auto still_bound = [&] {
      return enet_address_equal(const_cast<ENetAddress *>(&bound), &pair.server_peer->address) != 0;
    };

    // No token
    auto datagram = header(pair, pair.server_peer->incomingSessionID);
    add_unsequenced(datagram, 1, "roam");
    injector.send(pair.server_address, datagram);
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(still_bound());

    // A valid token next to a DISCONNECT
    datagram = header(pair, pair.server_peer->incomingSessionID);
    add_unsequenced(datagram, 2, "AUTH1");
    add_disconnect(datagram);
    injector.send(pair.server_address, datagram);
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(pair.server_disconnects == 0);
    SCN_CHECK(still_bound());

    // A valid token next to a message that is not valid
    datagram = header(pair, pair.server_peer->incomingSessionID);
    add_unsequenced(datagram, 3, "AUTH1");
    add_unsequenced(datagram, 4, "junk");
    injector.send(pair.server_address, datagram);
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(still_bound());

    // Only an acknowledged token: nothing new
    auth.accepted.insert(1);
    datagram = header(pair, pair.server_peer->incomingSessionID);
    add_unsequenced(datagram, 5, "AUTH1");
    injector.send(pair.server_address, datagram);
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(still_bound());

    // Wrong session id, even with a fresh token
    datagram = header(pair, (pair.server_peer->incomingSessionID + 1) & 3);
    add_unsequenced(datagram, 6, "AUTH2");
    injector.send(pair.server_address, datagram);
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(still_bound());
    SCN_CHECK(pair.server_peer->state == ENET_PEER_STATE_CONNECTED);

    close(pair);
    return scn_failures;
  }

  /// A fresh authenticated message moves the connection (alone or next to a replayed one); the host talks to the new source afterwards.
  inline int authenticated_payload_moves_the_peer() {
    int scn_failures = 0;
    pair_t pair;
    injector_t injector;
    token_auth_t auth;

    SCN_CHECK(open(pair, 0x0A0B0C0D) == 0);
    SCN_CHECK(injector.open());
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }
    pair.hooks.authenticate = [&auth](ENetPeer *, std::string_view message) {
      return auth.check(message);
    };
    auth.accepted.insert(7);  // an earlier message the client sends again

    auto datagram = header(pair, pair.server_peer->incomingSessionID);
    add_unsequenced(datagram, 1, "AUTH7");
    add_unsequenced(datagram, 2, "AUTH8");
    injector.send(pair.server_address, datagram);
    pump(pair, 6);

    SCN_CHECK(enet_address_equal(&pair.server_peer->address, &injector.address));
    SCN_CHECK(pair.received.size() == 2);
    SCN_CHECK(!pair.received.empty() && pair.received[0] == "AUTH7");
    SCN_CHECK(pair.server_peer->state == ENET_PEER_STATE_CONNECTED);
    SCN_CHECK(control_intercept::take_report().moved_peers == 1);

    enet_peer_send(pair.server_peer, 0, enet_packet_create("to-new", 6, ENET_PACKET_FLAG_RELIABLE));
    enet_host_flush(pair.server);
    SCN_CHECK(injector.receive(500) > 0);

    // The old address is now the foreign one
    pair.received.clear();
    enet_peer_send(pair.client_peer, 0, enet_packet_create("old", 3, ENET_PACKET_FLAG_RELIABLE));
    pump(pair, 4);
    SCN_CHECK(pair.received.empty());
    SCN_CHECK(pair.server_disconnects == 0);

    close(pair);
    return scn_failures;
  }

  /// A UDP relay between a client and the server: the server sees the relay's upstream socket as the client.
  /// Switching the upstream socket is what a changed NAT mapping or network looks like to the server.
  struct relay_t {
    injector_t front;  ///< the address the client connects to
    injector_t upstream[2];  ///< source addresses the server can see
    int current = 0;
    bool blackhole = false;  ///< drop what the client sends
    ENetAddress client_address {};
    bool client_known = false;
    ENetAddress server_address {};

    bool open(const ENetAddress &server) {
      server_address = server;
      return front.open() && upstream[0].open() && upstream[1].open();
    }

    static int receive_from(injector_t &socket, ENetAddress &from, unsigned char *data, size_t size) {
      ENetBuffer buffer;
      buffer.data = data;
      buffer.dataLength = size;
      const int length = enet_socket_receive(socket.socket, &from, nullptr, &buffer, 1);
      return length > 0 ? length : 0;
    }

    void forward() {
      unsigned char data[2048];
      ENetAddress from;
      for (int length; (length = receive_from(front, from, data, sizeof(data))) > 0;) {
        client_address = from;
        client_known = true;
        if (!blackhole) {
          upstream[current].send(server_address, std::vector<unsigned char>(data, data + length));
        }
      }
      for (auto &socket : upstream) {
        for (int length; (length = receive_from(socket, from, data, sizeof(data))) > 0;) {
          if (client_known) {
            front.send(client_address, std::vector<unsigned char>(data, data + length));
          }
        }
      }
    }
  };

  /// Like pump(), with the relay moving datagrams in between.
  inline void pump_relay(pair_t &pair, relay_t &relay, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      relay.forward();
      ENetEvent event;
      while (enet_host_service(pair.client, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
          ++pair.client_disconnects;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
          enet_packet_destroy(event.packet);
        }
      }
      relay.forward();
      while (enet_host_service(pair.server, &event, 2) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
          ++pair.server_connects;
          pair.server_peer = event.peer;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
          ++pair.server_disconnects;
        } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
          pair.received.emplace_back(reinterpret_cast<const char *>(event.packet->data), event.packet->dataLength);
          enet_packet_destroy(event.packet);
        }
      }
    }
  }

  /**
   * @brief A client keeps sending reliable messages (like the periodic ping) while its path is cut for a while, then
   *        continues from another source address with everything it could not deliver in its first datagrams.
   */
  inline int roaming_client_with_a_backlog_keeps_its_connection() {
    int scn_failures = 0;
    pair_t pair;
    relay_t relay;
    token_auth_t auth;

    pair.hooks.connect_expected = [](std::uint32_t data, const ENetAddress &) {
      return data == 0x13572468;
    };
    SCN_CHECK(make_server(pair) != nullptr);
    if (scn_failures != 0) {
      return scn_failures;
    }
    pair.hooks.authenticate = [&auth](ENetPeer *, std::string_view message) {
      return auth.check(message);
    };
    SCN_CHECK(relay.open(pair.server_address));
    pair.client = enet_host_create(AF_INET, nullptr, 1, 0, 0, 0);
    SCN_CHECK(pair.client != nullptr);
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }
    pair.client_peer = enet_host_connect(pair.client, &relay.front.address, 1, 0x13572468);
    for (int round = 0; round < 600 && (pair.client_peer == nullptr || pair.client_peer->state != ENET_PEER_STATE_CONNECTED); ++round) {
      pump_relay(pair, relay, 1);
    }
    pump_relay(pair, relay, 4);
    SCN_CHECK(pair.server_peer != nullptr && pair.client_peer->state == ENET_PEER_STATE_CONNECTED);
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }

    unsigned int next = 0;
    const auto send_next = [&] {
      const auto text = "AUTH" + std::to_string(next++);
      enet_peer_send(pair.client_peer, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
    };
    const auto settle = [&] {
      // what the server has delivered counts as accepted
      for (const auto &message : pair.received) {
        auth.accepted.insert(static_cast<unsigned int>(std::stoul(message.substr(4))));
      }
    };

    // Normal operation: one message every 100 ms
    for (int i = 0; i < 3; ++i) {
      send_next();
      for (int step = 0; step < 20; ++step) {
        pump_relay(pair, relay, 1);
        settle();
      }
    }
    SCN_CHECK(pair.received.size() == 3);
    SCN_CHECK(enet_address_equal(&relay.upstream[0].address, &pair.server_peer->address));

    // The path is cut for 1.5 s and the client reappears from another source address
    relay.blackhole = true;
    relay.current = 1;
    for (int i = 0; i < 15; ++i) {
      send_next();
      for (int step = 0; step < 20; ++step) {
        pump_relay(pair, relay, 1);
      }
    }
    SCN_CHECK(pair.received.size() == 3);
    relay.blackhole = false;

    for (int step = 0; step < 1000 && pair.received.size() < next; ++step) {
      if (step % 50 == 0) {
        send_next();
      }
      pump_relay(pair, relay, 1);
      settle();
    }

    SCN_CHECK(pair.received.size() == next);
    SCN_CHECK(pair.server_disconnects == 0);
    SCN_CHECK(pair.client_disconnects == 0);
    SCN_CHECK(pair.server_peer->state == ENET_PEER_STATE_CONNECTED);
    SCN_CHECK(pair.client_peer->state == ENET_PEER_STATE_CONNECTED);
    SCN_CHECK(enet_address_equal(&relay.upstream[1].address, &pair.server_peer->address));

    // A burst of unsequenced messages in one datagram (frame statistics) from yet another address
    relay.current = 0;
    for (int i = 0; i < 20; ++i) {
      const auto text = "AUTH" + std::to_string(next++);
      enet_peer_send(pair.client_peer, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_UNSEQUENCED));
    }
    enet_host_flush(pair.client);
    for (int step = 0; step < 40; ++step) {
      pump_relay(pair, relay, 1);
      settle();
    }
    SCN_CHECK(pair.received.size() == next);
    SCN_CHECK(enet_address_equal(&relay.upstream[0].address, &pair.server_peer->address));

    close(pair);
    return scn_failures;
  }

  /// 128 connection requests nobody waits for do not use up the peers, a legitimate client still connects.
  inline int bogus_connects_do_not_block_a_legitimate_one() {
    int scn_failures = 0;
    pair_t pair;
    std::vector<injector_t> attackers(128);

    pair.hooks.connect_expected = [](std::uint32_t data, const ENetAddress &) {
      return data == 0x55667788;
    };
    SCN_CHECK(make_server(pair) != nullptr);
    if (scn_failures != 0) {
      return scn_failures;
    }

    for (size_t i = 0; i < attackers.size(); ++i) {
      SCN_CHECK(attackers[i].open());
      attackers[i].send(pair.server_address, connect_request(0xC0000000U + static_cast<std::uint32_t>(i), 0xDEAD0000U + static_cast<std::uint32_t>(i)));
    }
    pump(pair, 8);

    SCN_CHECK(peers_in_use(pair.server) == 0);
    const auto rejected = control_intercept::take_report().rejected_connects;
    // UDP may lose a datagram of the burst (the first send of a process on Windows does), none may get through
    SCN_CHECK(rejected >= 100 && rejected <= 128);
    SCN_CHECK(connect_client(pair, 0x55667788) == 0);
    SCN_CHECK(pair.server_connects == 1);

    close(pair);
    return scn_failures;
  }

  /// Without the inspection one datagram from another source ends the connection (guards the test itself).
  inline int foreign_disconnect_is_accepted_without_the_inspection() {
    int scn_failures = 0;
    pair_t pair;
    injector_t injector;

    SCN_CHECK(open(pair, 0x31415926) == 0);
    SCN_CHECK(injector.open());
    if (scn_failures != 0) {
      close(pair);
      return scn_failures;
    }
    pair.server->intercept = nullptr;

    const ENetAddress bound = pair.server_peer->address;
    for (unsigned int session_id = 0; session_id < 4 && pair.server_disconnects == 0; ++session_id) {
      auto datagram = header(pair, session_id);
      add_disconnect(datagram);
      injector.send(pair.server_address, datagram);
      pump(pair, 4);
    }

    SCN_CHECK(pair.server_disconnects == 1);
    SCN_CHECK(!enet_address_equal(const_cast<ENetAddress *>(&bound), &pair.server_peer->address));

    close(pair);
    return scn_failures;
  }

  /// Without the inspection the same flood fills the peer table (guards the test itself).
  inline int bogus_connects_fill_an_unprotected_host() {
    int scn_failures = 0;
    pair_t pair;
    std::vector<injector_t> attackers(128);

    SCN_CHECK(make_server(pair) != nullptr);
    if (scn_failures != 0) {
      return scn_failures;
    }
    pair.server->intercept = nullptr;

    for (size_t i = 0; i < attackers.size(); ++i) {
      SCN_CHECK(attackers[i].open());
      attackers[i].send(pair.server_address, connect_request(0xC0000000U + static_cast<std::uint32_t>(i), 0xDEAD0000U + static_cast<std::uint32_t>(i)));
    }
    pump(pair, 8);
    SCN_CHECK(peers_in_use(pair.server) == static_cast<int>(pair.server->peerCount));

    close(pair);
    return scn_failures;
  }
}  // namespace scn
