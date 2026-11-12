# Migrating from Apollo or Sunshine

NyxPlay Host can import the configuration of an installed Apollo or Sunshine host. The other
installation is only read, never modified.

| Part | What is imported |
|------|------------------|
| Settings | `sunshine.conf`, except file locations (`file_apps`, `file_state`, `credentials_file`, `pkey`, `cert`, `log_path`) |
| Applications | `apps.json` and the `covers` folder; image paths are adapted |
| Paired devices | Paired clients together with the host identity (unique id, certificate, private key), so clients stay paired |
| Web UI credentials | Optional: user name and salted password hash |

Existing NyxPlay files are copied to `config/import-backup-<date>-<time>/` before they are replaced.

## Ways to import

- **Installer (Windows):** on a first installation the installer offers the import when it finds
  `%ProgramFiles%\Apollo\config` or `%ProgramFiles%\Sunshine\config`.
- **Web UI:** *Troubleshooting → Import from Apollo / Sunshine*. Select the detected installation or
  enter the path of a config folder, choose the parts and confirm. The host restarts afterwards.
- **Command line** (stop the host first, run elevated on Windows):

  ```text
  nyxplay-host --import-config                     # first detected installation
  nyxplay-host --import-config detect              # only list installations (exit code 0 if one exists)
  nyxplay-host --import-config "D:\Old\config" --no-clients --web-credentials --default-port
  ```

  Exit codes: `0` imported, `1` nothing found, `2` invalid arguments, `3` import failed.

Detected locations: `%ProgramFiles%\Apollo\config` and `%ProgramFiles%\Sunshine\config` on Windows,
`~/.config/sunshine` and `~/.config/apollo` (or `$XDG_CONFIG_HOME`) on Linux and macOS.

## After the import

- Two hosts cannot listen on the same ports. Stop or uninstall the other host, or import without the
  port (`--default-port`, or untick *Keep the port of the other host*).
- Settings that point into the old installation folder (for example commands) are kept as they are
  and reported as warnings; adjust them in the Web UI.
- Options that NyxPlay Host does not know are ignored with a warning in the log.
