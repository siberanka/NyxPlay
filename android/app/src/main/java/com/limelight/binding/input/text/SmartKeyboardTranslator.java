package com.limelight.binding.input.text;

import android.view.KeyEvent;

/**
 * Decides whether a key press should be sent to the host as a virtual key code (positional,
 * interpreted by the host keyboard layout) or as the exact Unicode character the user typed.
 * <p>
 * Virtual keys are required for games and shortcuts, but they produce wrong characters when the
 * host uses a different keyboard layout than the Android device (for example typing ğ, ü, ş, ı,
 * ö, ç on a Turkish keyboard while the host is set to English). In {@link Mode#SMART} mode a key
 * is therefore only converted to text when the character it produces differs from what the same
 * key would type on a US layout host.
 * <p>
 * This class is free of Android framework state so it can be unit tested on the JVM.
 */
public final class SmartKeyboardTranslator {

    public enum Mode {
        /** Send characters as text only when a positional key would type something else. */
        SMART,
        /** Send every printable character as text (best for typing, not for games). */
        UNICODE,
        /** Legacy behaviour: always send positional virtual keys. */
        RAW;

        public static Mode fromPreference(String value) {
            if ("unicode".equals(value)) {
                return UNICODE;
            }
            if ("raw".equals(value)) {
                return RAW;
            }
            return SMART;
        }
    }

    /** Host text input focus as reported by NyxPlay Host. */
    public enum HostFocus {
        /** Host does not report text focus (Apollo, Sunshine, GFE) or no report yet. */
        UNKNOWN,
        /** An editable text field is focused on the host. */
        TEXT_FIELD,
        /** No text field is focused (games, desktop, buttons...). */
        NO_TEXT_FIELD
    }

    /** All information needed to classify a key press. */
    public static final class KeyInput {
        /** Key code that will be translated to a virtual key (after QWERTY normalisation). */
        public int effectiveKeyCode;
        /** QWERTY position of the key (used for layout tables). */
        public int positionKeyCode;
        /** Linux scan code. */
        public int scanCode;
        public boolean shift;
        public boolean capsLock;
        /** Right Alt / AltGr held. */
        public boolean altGr;
        /** Ctrl, Meta (Windows key) or left Alt held: the key is part of a shortcut. */
        public boolean shortcutModifier;
        /** Character reported by Android's key character map (0 if none). */
        public int deviceChar;
        /** True if the device key character map is the generic US map (layout unknown). */
        public boolean genericKeyCharacterMap;
        /** True for key events synthesised by an on-screen keyboard. */
        public boolean softKeyboard;
    }

    private SmartKeyboardTranslator() {
    }

    /**
     * Classify a key press.
     *
     * @param in             key information
     * @param mode           translation mode
     * @param fallbackLayout layout to use when the key character map is unreliable (may be null)
     * @param forceLayout    true if {@code fallbackLayout} was chosen explicitly by the user and
     *                       must be used even if Android reports a specific layout
     * @param hostFocus      host text focus state
     * @return the text to send, or {@code null} to send the positional virtual key instead
     */
    public static String translate(KeyInput in, Mode mode, KeyboardLayouts.Layout fallbackLayout,
                                   boolean forceLayout, HostFocus hostFocus) {
        if (mode == Mode.RAW || in.shortcutModifier || isNumpad(in.effectiveKeyCode)) {
            return null;
        }

        int ch = resolveCharacter(in, fallbackLayout, forceLayout);
        if (!isPrintable(ch)) {
            return null;
        }

        if (mode == Mode.UNICODE) {
            return toText(ch);
        }

        char usChar = KeyboardLayouts.us().charFor(in.effectiveKeyCode, in.scanCode, in.shift, in.capsLock, false);
        boolean sameAsUs = !in.altGr && usChar == ch;

        if (in.softKeyboard) {
            // On-screen keyboards are used for typing text: send exact characters unless the host
            // tells us no text field is focused and a positional key would type the same thing.
            if (hostFocus == HostFocus.NO_TEXT_FIELD && sameAsUs) {
                return null;
            }
            return toText(ch);
        }

        if (sameAsUs) {
            return null;
        }

        // Keep letter keys positional while the host reports that no text field is focused, so
        // that game bindings (e.g. WASD on AZERTY keyboards) keep working.
        if (hostFocus == HostFocus.NO_TEXT_FIELD && isLetterKey(in.effectiveKeyCode)) {
            return null;
        }

        return toText(ch);
    }

    /**
     * Determine which character the user intended to type.
     */
    static int resolveCharacter(KeyInput in, KeyboardLayouts.Layout fallbackLayout, boolean forceLayout) {
        boolean useLayout = fallbackLayout != null && !in.softKeyboard
                && (forceLayout || in.genericKeyCharacterMap)
                && fallbackLayout.hasKey(in.positionKeyCode, in.scanCode);
        if (useLayout) {
            return fallbackLayout.charFor(in.positionKeyCode, in.scanCode, in.shift, in.capsLock, in.altGr);
        }
        if ((in.deviceChar & android.view.KeyCharacterMap.COMBINING_ACCENT) != 0) {
            // Dead keys are composed by the host layout
            return 0;
        }
        return in.deviceChar & android.view.KeyCharacterMap.COMBINING_ACCENT_MASK;
    }

    static boolean isPrintable(int ch) {
        return ch >= 0x20 && ch != 0x7F && !(ch >= 0x80 && ch < 0xA0) && Character.isValidCodePoint(ch);
    }

    static boolean isLetterKey(int keyCode) {
        return keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z;
    }

    static boolean isNumpad(int keyCode) {
        return (keyCode >= KeyEvent.KEYCODE_NUMPAD_0 && keyCode <= KeyEvent.KEYCODE_NUMPAD_RIGHT_PAREN)
                || keyCode == KeyEvent.KEYCODE_NUM_LOCK;
    }

    private static String toText(int ch) {
        return new String(Character.toChars(ch));
    }
}
