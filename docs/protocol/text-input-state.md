# Protocol extension: text input state

| | |
|---|---|
| Status | Implemented (NyxPlay Host, NyxPlay for Android) |
| Direction | Host → client, encrypted control stream (ENet) |
| Packet type | `0x4E01` |
| Opt-in | `nyxTextInput=1` query parameter on `/launch` and `/resume` |

## Purpose

Touch clients have no physical keyboard. When the user taps a text field on the streamed desktop the
client should show its on-screen keyboard, and hide it again when the field loses focus. The host is the
only side that knows which UI element has keyboard focus, so it reports text field focus changes to the
client.

## Negotiation

The client adds `nyxTextInput=1` to the `/launch` or `/resume` request. Hosts that do not know the
parameter ignore it (Apollo, Sunshine). NyxPlay Host only sends the message to sessions that opted in
**and** hold the keyboard input permission (`input_kbd`).

While at least one opted-in session exists the host runs a focus monitor; it is stopped when the last
such session ends.

## Message

The message uses the regular control stream framing (`control_header_v2`: little endian `type` and
`payloadLength`) and is encrypted like every other control message of the Sunshine protocol.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `version` | Payload version, currently `1`. `0` is invalid. |
| 1 | 1 | `active` | `1` while an editable text field has keyboard focus, otherwise `0` |
| 2 | 1 | `flags` | Bit field, `0` when `active` is `0` |
| 3 | 1 | `reserved` | Always `0` |

Flags:

| Bit | Name | Meaning |
|-----|------|---------|
| `0x01` | `PASSWORD` | The field hides its content |
| `0x02` | `MULTILINE` | The field accepts multiple lines (documents, rich text editors) |

Receivers must accept longer payloads (future fields) and treat a payload shorter than two bytes or with
`version == 0` as "no text field focused".

The host sends the current state as soon as the control stream of an opted-in session is connected and
then on every change. Changes to "inactive" are delayed by 250 ms on the host so that focus moving
between a text box and its suggestion popup does not make the keyboard flicker.

## Host implementation (Windows)

[`host/src/text_input.cpp`](../../host/src/text_input.cpp)

- A dedicated MTA thread subscribes to UI Automation focus change events
  (`IUIAutomation::AddFocusChangedEventHandler`). It follows the active input desktop (UAC prompts,
  lock screen) and re-subscribes when it changes.
- The focused element is classified from its control type, `IsEnabled`, `IsKeyboardFocusable`,
  `IsPassword`, the `ValuePattern.IsReadOnly` state and `TextPattern` support (see
  `text_input::classify`). Edit controls, editable documents and editable combo boxes are reported as
  text fields; a browser page (read-only document) is not.
- If UI Automation is unavailable the monitor falls back to polling the caret of the foreground thread
  (`GetGUIThreadInfo`).

Other platforms currently never report a text field.

## Client implementation

- `moonlight-common-c`: packet type `0x4E01` is registered for the encrypted (Gen 7) protocol and
  delivered to the new `CONNECTION_LISTENER_CALLBACKS::setTextInputState(bool active, uint8_t flags)`
  callback. Consecutive queued states are collapsed so only the latest one is delivered.
- Android: `MoonBridge.bridgeClSetTextInputState` → `NvConnectionListener.setTextInputState` →
  `AutoKeyboardController` (see [`docs/TEXT_INPUT.md`](../TEXT_INPUT.md)).
