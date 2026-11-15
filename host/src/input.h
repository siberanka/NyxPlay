/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <span>

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "crypto.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM& permission);

  /**
   * @brief Check that a raw input message is large enough for its declared and protocol specific fields.
   * @param packet The decrypted input message.
   * @return `true` when the message can be safely interpreted.
   */
  bool validate_input_packet(std::span<const std::uint8_t> packet);

  /**
   * @brief Release input that is held for permission groups which were just revoked.
   * @param input The input context pointer.
   * @param removed The revoked permission bits (only the input bits are considered).
   */
  void release_denied(std::shared_ptr<input_t> &input, crypto::PERM removed);

  /**
   * @brief Internal helpers exposed for unit tests.
   */
  namespace testing {
    /// Result of batching two queued input messages.
    enum class batch_outcome_e {
      batched,
      not_batchable,
      terminate_batch,
    };

    /**
     * @brief Check whether a validated input message may be queued with the given permissions.
     * @param permission The permissions of the client.
     * @param packet The validated input message.
     * @return `true` when the message belongs to a known type and a permission group the client holds.
     */
    bool is_permitted(const crypto::PERM &permission, std::span<const std::uint8_t> packet);

    /**
     * @brief Try to batch `src` into `dest` (both must be validated messages).
     */
    batch_outcome_e batch_messages(std::vector<std::uint8_t> &dest, std::vector<std::uint8_t> &src);

    /// Clamp `value` to [min, max]; a non-finite value yields `min`.
    float clamp_or_min(float value, float min, float max);

    /// Limit `value` to [-limit, limit]; a non-finite value yields 0.
    float limit_finite(float value, float limit);
  }  // namespace testing

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
