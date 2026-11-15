# API

Sunshine has a RESTful API which can be used to interact with the service.

Unless otherwise specified, authentication is required for all API calls. Send the Web UI username and
password to `POST /api/login` as JSON (`{"username": "...", "password": "..."}`). The answer sets the `auth`
cookie, which has to accompany every further call.

## Request rules

- Requests with a body must use `Content-Type: application/json`.
- Requests that change state (every method except GET) from a browser must come from the Web UI's own
  origin. The `Sec-Fetch-Site` header (`same-origin` or `none`) or else the `Origin` / `Referer` header has
  to match the `Host` of the request; anything else is answered with `403`
  (`{"status": false, "error": "Cross-origin request rejected"}`). Scripts that send none of these headers are
  not affected. A reverse proxy has to pass the `Host` header unchanged.
- Request bodies are limited per endpoint: 4 KiB for logins, passwords, PINs, one-time passwords and the
  small actions, 256 KiB for `/api/apps`, `/api/apps/reorder`, `/api/config` and `/api/clients/update`, and
  11 MiB for `/api/covers/upload`. Larger bodies are answered with `413`.
- Invalid JSON is answered with `400` (`Invalid JSON`). All error answers have the form
  `{"status": false, "error": "<reason>"}`.
- `429` (with a `Retry-After` header) means too many failed logins from the address.
- Responses are not cacheable (`Cache-Control: no-store`).

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/reorder
@copydoc confighttp::reorderApps()

## POST /api/apps/delete
@copydoc confighttp::deleteApp()

## POST /api/apps/launch
@copydoc confighttp::launchApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/update
@copydoc confighttp::updateClient()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/disconnect
@copydoc confighttp::disconnect()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/logs
@copydoc confighttp::getLogs()

## POST /api/password
@copydoc confighttp::savePassword()

## POST /api/otp
@copydoc confighttp::getOTP()

## GET /api/pin
@copydoc confighttp::getPendingPairings()

## POST /api/pin
@copydoc confighttp::savePin()

## DELETE /api/pin
@copydoc confighttp::cancelPairing()

## POST /api/login
@copydoc confighttp::login()

## GET /api/import
@copydoc confighttp::getImportSources()

## POST /api/import
@copydoc confighttp::importConfig()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/restart
@copydoc confighttp::restart()

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
