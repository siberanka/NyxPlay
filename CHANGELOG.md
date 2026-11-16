# Changelog

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
