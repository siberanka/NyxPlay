/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"
#include "rtsp.h"
#include "thread_safe.h"

using namespace std::chrono_literals;

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using cmd_list_t = std::list<crypto::command_entry_t>;

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number indicates to Moonlight that this is Sunshine.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  constexpr auto OTP_EXPIRE_DURATION = 180s;

  /**
   * @brief Wrong OTP attempts of one address after which its further attempts are ignored.
   */
  constexpr int MAX_OTP_FAILURES = 5;

  /**
   * @brief Wrong OTP attempts of all addresses together after which the current one-time PIN is invalidated.
   */
  constexpr int MAX_OTP_FAILURES_TOTAL = 20;

  /**
   * @brief Counts the wrong attempts to use the current one-time PIN, per source address and in total.
   * @details One address can only use up its own attempts, so it cannot invalidate the one-time PIN
   *          an administrator just created for someone else. A small total cap remains for attackers that
   *          come from many addresses. Not thread safe, the caller serializes access.
   */
  class otp_attempt_limiter_t {
  public:
    /// @return `false` once the address used up its attempts.
    bool allowed(const std::string &address) const;

    /// @return `true` if the one-time PIN has to be invalidated (the total cap is reached).
    bool record_failure(const std::string &address);

    /// Forget all failures (new, used or invalidated one-time PIN).
    void reset();

  private:
    std::map<std::string, int> failures_;
    int total_ = 0;
  };

  /**
   * @brief Maximum number of pairing sessions retained at one time.
   */
  constexpr std::size_t MAX_PENDING_PAIRING_SESSIONS = 32;
  constexpr std::size_t MAX_PENDING_PAIRING_SESSIONS_PER_ADDRESS = 4;

  /**
   * @brief Number of hexadecimal characters in an operator approval identifier.
   */
  constexpr std::size_t PAIRING_ID_SIZE = 32;

  /**
   * @brief Maximum number of bytes accepted for an operator supplied paired client name.
   */
  constexpr std::size_t MAX_PAIRING_CLIENT_NAME_SIZE = 128;

  /**
   * @brief Lifetime of an incomplete pairing session.
   */
  constexpr auto PAIRING_SESSION_TIMEOUT = std::chrono::minutes {5};

  /**
   * @brief Time left for the remaining pairing phases once the PIN has been delivered.
   */
  constexpr auto PAIRING_POST_PIN_TIMEOUT = std::chrono::minutes {2};

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  std::string
  get_arg(const args_t &args, const char *name, const char *default_value = nullptr);

  // Helper function to extract command entries
  cmd_list_t
  extract_command_entries(const nlohmann::json& j, const std::string& key);

  std::shared_ptr<rtsp_stream::launch_session_t>
  make_launch_session(bool host_audio, bool input_only, const args_t &args, const crypto::named_cert_t* named_cert_p);

  /**
   * @brief Drop the client do/undo commands of a launch unless the app and the device both allow them.
   * @param launch_session The launch to restrict.
   * @param app_allows Whether the app allows commands of clients.
   * @param device_allows Whether the paired device is allowed to run its own commands.
   */
  void restrict_client_commands(rtsp_stream::launch_session_t &launch_session, bool app_allows, bool device_allows);

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Send the TLS close_notify, but never wait for the peer's answer. This destructor runs on the
      // only I/O thread of the HTTPS server; a blocking shutdown waits for the peer's close_notify
      // and let a client that keeps its socket open stall every other TLS handshake. In
      // non-blocking mode the shutdown sends our close_notify and returns would_block instead of
      // waiting; the socket is closed right after by its own destructor.
      SimpleWeb::error_code ec;
      lowest_layer().non_blocking(true, ec);
      ec.clear();
      shutdown(ec);
    }
  };

  enum class PAIR_PHASE {
    NONE,  ///< Sunshine is not in a pairing phase
    GETSERVERCERT,  ///< Sunshine is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sunshine is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sunshine is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sunshine is in the client pairing secret phase
  };

  struct pair_session_t {
    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key = {};
    std::vector<uint8_t> clienthash = {};

    std::string serversecret = {};
    std::string serverchallenge = {};

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>>
        response;
      std::string salt = {};
      std::string id = {};  ///< Unguessable identifier used by the Web UI to approve this session.
      std::string address = {};  ///< Network address from which the pairing request originated.
      std::chrono::steady_clock::time_point expires_at = std::chrono::steady_clock::time_point::max();  ///< Deadline for completing this pairing session.
    } async_insert_pin;

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
    bool failed = false;  ///< Protocol validation failed: the caller removes the session.
    const char *service = "http";  ///< Server the request arrived on ("http" or "https"), for the security log.
    bool failure_recorded = false;  ///< The security log already holds the failure of this session.
  };

  /**
   * @brief Operator visible context for a pairing request awaiting PIN approval.
   */
  struct pending_pairing_t {
    std::string id;  ///< Unguessable approval identifier.
    std::string name;  ///< Untrusted device name reported by the pairing client.
    std::string address;  ///< Network address from which the request originated.
  };

  /**
   * @brief Result of inserting a new pairing session into the bounded pending storage.
   */
  enum class pair_session_insert_e {
    ADDED,  ///< The session was inserted (an earlier attempt of the same client from the same address was replaced).
    ALREADY_EXISTS,  ///< A session with the same client unique ID from another address is active.
    FULL  ///< The bounded session store has reached its limit.
  };

  /**
   * @brief Validate an operator approval identifier received from the REST API.
   * @param pairing_id Approval identifier to validate.
   * @return `true` when the value has the exact hexadecimal format generated by the host.
   */
  bool is_valid_pairing_id(std::string_view pairing_id);

  /**
   * @brief Validate a Moonlight pairing PIN received from the REST API.
   * @param pin PIN to validate.
   * @return `true` when the value contains exactly four ASCII digits.
   */
  bool is_valid_pairing_pin(std::string_view pin);

  /**
   * @brief Validate an optional operator supplied client name received from the REST API.
   * @param name Client name to validate (empty keeps the name reported by the client).
   * @return `true` when the name is within the storage limit.
   */
  bool is_valid_pairing_name(std::string_view name);

  /**
   * @brief Make a client supplied device name safe to store, show and put into JSON.
   * @details Control characters and text direction/separator characters are dropped, every invalid UTF-8
   *          byte becomes U+FFFD and the result is cut on a code point boundary within
   *          `MAX_PAIRING_CLIENT_NAME_SIZE` bytes. Everything else is kept.
   * @param name The name as sent by the client.
   */
  std::string sanitize_pairing_name(std::string_view name);

  /**
   * @brief Insert a newly created pairing session into the bounded pending storage.
   * @param sess Pairing session to insert.
   * @param pairing_id Receives the unguessable approval identifier on success.
   * @return Status describing whether the session was inserted.
   */
  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id);

  /**
   * @brief Remove pairing sessions whose completion deadline has passed.
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /**
   * @brief List pairing requests that are waiting for operator PIN approval.
   * @return Pending requests ordered from oldest to newest.
   */
  std::vector<pending_pairing_t> get_pending_pairings();

  /**
   * @brief Cancel a pairing request that is waiting for operator approval.
   * @param pairing_id Approval identifier returned by @ref get_pending_pairings.
   * @return `true` if the pending request was found and cancelled.
   */
  bool cancel_pairing(std::string_view pairing_id);

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   */
  void clientpairingsecret(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  /**
   * @brief Apply the user supplied PIN to the explicitly selected pairing request.
   * @param pairing_id Identifier of the pairing request to approve (see @ref get_pending_pairings).
   * @param pin The user supplied pin.
   * @param name The user supplied name (empty keeps the name reported by the client).
   * @return `true` if the PIN was delivered to the waiting client, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("0123456789abcdef0123456789abcdef", "1234", "laptop");
   * @examples_end
   */
  bool pin(std::string_view pairing_id, std::string pin, std::string name);

  std::string request_otp(const std::string& passphrase, const std::string& deviceName);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();

  /**
   * @brief      Stops a session.
   *
   * @param      session   The session
   * @param[in]  graceful  Whether to stop gracefully
   */
  void stop_session(stream::session_t& session, bool graceful);

  /**
   * @brief      Finds and stop session.
   *
   * @param[in]  uuid      The uuid string
   * @param[in]  graceful  Whether to stop gracefully
   */
  bool find_and_stop_session(const std::string& uuid, bool graceful);

  /**
   * @brief      Update device info associated to the session
   *
   * @param      session  The session
   * @param[in]  name     New name
   * @param[in]  newPerm  New permission
   */
  void update_session_info(stream::session_t& session, const std::string& name, const crypto::PERM newPerm);

  /**
   * @brief      Finds and udpate session information.
   *
   * @param[in]  uuid     The uuid string
   * @param[in]  name     New name
   * @param[in]  newPerm  New permission
   */
  bool find_and_udpate_session_info(const std::string& uuid, const std::string& name, const crypto::PERM newPerm);

  /**
   * @brief Read a text value that is kept next to the client list in the state file.
   * @param key The top-level key.
   * @return The value, or an empty string when it is missing.
   */
  std::string load_state_text(std::string_view key);

  /**
   * @brief Store a text value next to the client list in the state file (the other keys are kept).
   * @param key The top-level key.
   * @param value The value; an empty value removes the key.
   * @return true when the file holds the requested state.
   */
  bool store_state_text(std::string_view key, std::string_view value);

  /**
   * @brief      Update device info
   *
   * @param[in]  uuid       The uuid string
   * @param[in]  name       New name
   * @param[in]  do_cmds    The do commands
   * @param[in]  undo_cmds  The undo commands
   * @param[in]  newPerm    New permission
   * @param[in]  enable_legacy_ordering  Enable legacy ordering
   * @param[in]  allow_client_commands  Allow client commands
   * @param[in]  always_use_virtual_display  Always use virtual display
   * 
   * @return     Whether the update is successful
   */
  bool update_device_info(
    const std::string& uuid,
    const std::string& name,
    const std::string& display_mode,
    const cmd_list_t& do_cmds,
    const cmd_list_t& undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display
  );

  /**
   * @brief Get a snapshot of a paired client.
   * @param uuid The UUID of the client.
   * @return The client record (never modified after it was published), or `nullptr` if unknown.
   */
  std::shared_ptr<const crypto::named_cert_t> get_client(std::string_view uuid);

  /**
   * @brief Bring a launch session that waited for its stream up to date with its client.
   * @details An administrator may change the permissions of a client, or remove it, between the launch
   *          request and the start of the stream. The permissions and the right to run client commands
   *          of the current record apply to the stream, never the ones of the request.
   * @param launch_session The launch session to update.
   * @return `false` if the client is gone or may no longer view, the stream must not start then.
   */
  bool refresh_launch_session(rtsp_stream::launch_session_t &launch_session);

  /**
   * @brief Move the deadline of a pairing session out once the PIN has been delivered.
   * @param sess The pairing session.
   * @param now Monotonic time used as the base of the new deadline.
   */
  void extend_pair_session_deadline(pair_session_t &sess, std::chrono::steady_clock::time_point now);

  /**
   * @brief Resolve the paired client record of a request.
   * @param userp The record bound to the connection; it is re-bound when the record was replaced.
   * @return The current record, or a record without permissions when the client is no longer paired.
   */
  crypto::named_cert_t *resolve_verified_cert(std::shared_ptr<void> &userp);

#ifdef SUNSHINE_TESTS
  /**
   * @brief Access to the client registry for unit tests.
   */
  namespace test_access {
    /// Remove all clients, pending pairing sessions and one-time PIN state.
    void reset();
    /// Add a client through the regular pairing path; `false` when it could not be persisted.
    bool add_client(const crypto::p_named_cert_t &named_cert_p);
    /// Number of paired clients.
    std::size_t client_count();
    /// Persist the clients to `config::nvhttp.file_state`.
    bool save();
    /// Load the clients from `config::nvhttp.file_state`.
    void load();
  }  // namespace test_access
#endif
}  // namespace nvhttp
