# Changelog

## 2026.11.19.1

### Added
- Host: security log of pairing, Web UI logins, rejected certificates, launches, resumes, cancels, stream start and end, app exits and admin actions, with client, app and source address; shown on the new Security page of the Web UI with filters, statistics and CSV export.
- Host: automatic blocking of addresses that repeatedly fail pairing, Web UI login or RTSP checks, on every port; blocks survive restarts and can be reviewed and lifted on the Security page. New settings: security_log, security_log_max_mb, ban_enabled, ban_threshold, ban_window_seconds, ban_duration_seconds, ban_whitelist. The host PC itself is never blocked.

### Security
- Host: the control stream ignores datagrams from other addresses for an established client (a stream can no longer be ended by an outside sender), accepts connects only for a launched stream, and rejects replayed encrypted control messages.
- Host: per-device "allow client commands" is honoured for every launch, launch options (scaleFactor, audio, HDR and key fields) are validated, a stream uses the client's current permissions, OTP failures are counted per address, and pairing device names are sanitised.

### Fixed
- Installer: selecting Run at Startup during an upgrade now switches the service to automatic start; a start type chosen earlier is kept otherwise.
- Host service: a host that stays running without opening its ports is restarted after a grace period.
- Host: RTSP requests whose header end arrives in two parts are handled; a launch without a valid app returns an error.

### Changed
- Turkish translations reviewed for the host Web UI, installer, Android and PC clients; Web UI configuration tabs and remaining fixed texts are translated.

### Known issues
- Older clients that use an unencrypted control stream lose the stream when their network address changes (encrypted clients, which all current clients use, keep working).

## 2026.11.16.1

### Security
- Host: integers sent by clients are range-checked (stream parameters, session key id, frame rate) instead of being truncated.
- Host: the RTSP parser frees repeated headers; empty ANNOUNCE attribute values are handled.
- Host: settings import takes over the host certificate only as a valid, matching certificate and private key pair.
- Android and PC: host names are limited to 64 characters and stripped of control and text-direction characters.

### Fixed
- Host: no longer leaks a Windows handle on every app launch, pause, resume and stream session.
- Host: invalid JSON in `global_prep_cmd`, `global_state_cmd` or `server_cmd` is logged and ignored instead of stopping the host.
- Host: clients imported from the legacy `devices` layout get the same duplicate and ID checks as other clients.
- Host: unterminated nested lists in the configuration are no longer read past their end.
- Android: unreadable numbers in host responses (ports, current game, app index, version) no longer crash the app; a stream start ends with an error instead.
- Installer: the startup component name is no longer cut off.

### Known issues
- `/cancel` waits up to the app's exit timeout (5 s by default) and other requests to the host wait meanwhile, as in upstream.

## 2026.11.14.1

### Security
- Pairing, PIN/OTP and client registry handling hardened; invalid permissions rejected; unpairing ends running streams.
- Unencrypted RTSP launches are bound to the launching address; new option `rtsp_plaintext` (default: local network only).
- Web UI: origin checks, request size limits, validated application and configuration writes.
- Android: external links and shortcuts ask for confirmation; host responses validated and bounded.
- PC: client key encrypted per Windows user; update links restricted to the NyxPlay repository.

### Fixed
- Concurrent clients no longer reset each other; launch/resume/cancel/terminate are serialised per slot.
- Android discovery, polling, pairing and teardown robustness; IME text ordering; RTSP and server info parsing.
- PC keeps the host address you typed and recovers from connection resets; the `pair` and `list` commands always exit with a result code.
- Idle TLS connections no longer block new HTTPS handshakes on the host.
- Upgrades keep the start type of the host service; a missing ViGEmBus driver is logged as a warning.
- Android shows clear errors for invalid links, app lists that cannot be loaded and launch timeouts.

### Added
- Host: import of settings, apps and paired devices from Apollo or Sunshine.
- PC: first-run import of hosts and settings from Moonlight.
- Complete Turkish translation (Android, PC, host Web UI and installer).
- Third-party licence texts in the PC package and the host installation; signed Android release APKs.

### Changed
- Windows installer: robust service, firewall, gamepad, PATH and virtual display helpers; safer install and uninstall.
- Portable PC package is named `NyxPlayPC-<version>-windows-x64-portable.zip`.

Upgrade notes: Android shortcuts ask once; the PC portable identity is bound to the Windows user (re-pair after moving it). See the release notes for details.
