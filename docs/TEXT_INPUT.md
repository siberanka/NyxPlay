# Text input in NyxPlay

NyxPlay improves typing on touch devices in two ways: the on-screen keyboard opens by itself when a
text field on the host is tapped, and characters arrive correctly no matter which keyboard layout the
host uses.

## Automatic on-screen keyboard (Android)

1. The Android client opts in with `nyxTextInput=1` when it launches or resumes a stream
   (setting **Show keyboard for text fields**, enabled by default).
2. NyxPlay Host watches the keyboard focus with Windows UI Automation and sends a
   [text input state message](protocol/text-input-state.md) whenever an editable field gains or loses
   focus.
3. `AutoKeyboardController` on the client decides what to do:
   - a text field gained focus **within 1.5 s of a tap** on the stream → show the keyboard;
   - the field lost focus and the keyboard was opened automatically → hide it again;
   - tapping a field that is already focused re-opens a dismissed keyboard (after 300 ms, cancelled if
     the tap moved the focus elsewhere);
   - focus changes that the user did not cause (applications focusing a search box on their own,
     other users) never open the keyboard;
   - nothing happens while a hardware keyboard is connected.

Older hosts (Apollo, Sunshine) never send the message; the keyboard can still be opened manually.

## Layout aware typing

### The problem

GameStream sends keyboard input as Windows virtual key codes. A virtual key identifies a **key
position**, and the host turns it into a character with **its own** keyboard layout. When the layouts
differ the wrong characters appear. On a Turkish Q keyboard and an English host, for example:

| Key pressed | Expected | Host typed (positional) |
|-------------|----------|-------------------------|
| `ğ` | ğ | `[` |
| `ü` | ü | `]` |
| `ş` | ş | `;` |
| `i` | i | `'` |
| `ö` | ö | `,` |
| `ç` | ç | `.` |
| `ı` | ı | `i` |

### The solution: smart mode

`SmartKeyboardTranslator` determines the character the user intended and compares it with the
character the same key would type on a US layout:

- **Same character** → the positional virtual key is sent (games, shortcuts and key repeat keep
  working exactly as before).
- **Different character** → the exact Unicode character is sent as text, which the host types
  independently of its layout (`ğ ü ş ı ö ç İ`, AltGr symbols, shifted symbols that live elsewhere).
- Shortcuts (Ctrl, Win, left Alt) and numpad keys always use virtual keys.
- When NyxPlay Host reports that **no** text field is focused (a game is running), letter keys stay
  positional so that WASD style bindings keep working on AZERTY/QWERTZ keyboards.
- Characters typed on the on-screen keyboard are always sent as text (a Turkish host would turn `VK_I`
  into `ı`).
- A held AltGr is released on the host around the text so it is not interpreted as an Alt shortcut.

Modes (setting **Keyboard text mode**): *Smart* (default), *Always send characters*, *Key positions only*
(legacy behaviour).

### Which layout does the hardware keyboard use?

1. If Android has a layout configured for the keyboard, its key character map already produces the
   right characters and is used directly.
2. If Android only has the generic US map (layout unknown), NyxPlay infers the layout:
   the language of the active input method (ignoring English, which is often only a secondary
   subtype), otherwise the **system language**. `tr` maps to Turkish Q; German, French (AZERTY),
   Spanish and Italian are supported as well.
3. The setting **Physical keyboard layout** forces a layout (Turkish Q, Turkish F, English US, German,
   French, Spanish, Italian).

### On-screen keyboard text

With **Exact text from on-screen keyboard** (enabled by default) the stream view acts as a text editor
for the input method:

- every committed character is sent as exact Unicode text;
- with **Keyboard predictions** enabled, composing text (gesture typing, suggestions, voice) is mirrored
  live: `ImeTextComposer` turns each update into backspaces plus the changed characters;
- otherwise a visible-password style editor is announced, which makes keyboards commit every key
  immediately and disables auto-correction;
- Enter, editor actions and newline commits are sent as the Enter key.

### Protocol core fixes

The Android copy of `moonlight-common-c` now sends UTF-8 text on the keyboard channel and no longer
waits 50 ms before every UTF-8 event when talking to Sunshine based hosts (cherry-picked from upstream
`4a95eb3` and `0405384`). Mixed key and text input therefore arrives in order and without delay.

## Source map

| Area | File |
|------|------|
| Host focus monitor | `host/src/text_input.{h,cpp}` |
| Host message sending | `host/src/stream.cpp` (`send_text_input_state`) |
| Protocol core | `*/moonlight-common-c/src/ControlStream.c`, `Limelight.h` |
| Android bridge | `android/app/src/main/jni/moonlight-core/callbacks.c`, `MoonBridge.java` |
| Keyboard controller | `android/.../binding/input/text/AutoKeyboardController.java` |
| Layout tables | `android/.../binding/input/text/KeyboardLayouts.java` |
| Translation logic | `android/.../binding/input/text/SmartKeyboardTranslator.java`, `SmartKeyboard.java` |
| IME bridge | `android/.../ui/StreamInputConnection.java`, `binding/input/text/ImeTextComposer.java` |
