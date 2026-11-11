package com.limelight.binding.input.text;

import android.view.KeyEvent;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Character tables for common physical keyboard layouts.
 * <p>
 * Every table is keyed by the US QWERTY key position (expressed as an Android key code) and
 * describes which characters a key produces with and without Shift / AltGr. They are used
 * when Android cannot tell which layout a hardware keyboard uses (the generic US key
 * character map is active), in which case NyxPlay falls back to the layout that matches the
 * active input method language or the system language.
 */
public final class KeyboardLayouts {

    /** Pseudo key code for the extra ISO key between left Shift and Z (Linux scan code 86). */
    public static final int KEYCODE_ISO_102ND = -86;
    /** Linux evdev scan code of the ISO 102nd key. */
    public static final int SCANCODE_ISO_102ND = 86;

    private static final int BASE = 0;
    private static final int SHIFT = 1;
    private static final int ALTGR = 2;

    /** Supported layout identifiers (also used as preference values). */
    public static final String ID_US = "us";
    public static final String ID_TR_Q = "tr_q";
    public static final String ID_TR_F = "tr_f";
    public static final String ID_DE = "de";
    public static final String ID_FR = "fr";
    public static final String ID_ES = "es";
    public static final String ID_IT = "it";

    public static final class Layout {
        private final String id;
        private final Map<Integer, char[]> keys = new HashMap<>();

        private Layout(String id) {
            this.id = id;
        }

        public String getId() {
            return id;
        }

        private Layout key(int keyCode, char base, char shift) {
            return key(keyCode, base, shift, (char) 0);
        }

        private Layout key(int keyCode, char base, char shift, char altGr) {
            keys.put(keyCode, new char[]{base, shift, altGr});
            return this;
        }

        private Layout letters(String positions, String chars, String shifted) {
            for (int i = 0; i < positions.length(); i++) {
                int keyCode = KeyEvent.KEYCODE_A + (positions.charAt(i) - 'a');
                char[] existing = keys.get(keyCode);
                char altGr = existing != null ? existing[ALTGR] : 0;
                keys.put(keyCode, new char[]{chars.charAt(i), shifted.charAt(i), altGr});
            }
            return this;
        }

        private Layout altGr(int keyCode, char altGr) {
            char[] existing = keys.get(keyCode);
            if (existing == null) {
                keys.put(keyCode, new char[]{0, 0, altGr});
            } else {
                existing[ALTGR] = altGr;
            }
            return this;
        }

        /**
         * Resolve the character produced by a key.
         *
         * @param keyCode  US position key code
         * @param scanCode Linux scan code (used to tell the ISO 102nd key apart)
         * @param shift    Shift held
         * @param capsLock Caps Lock active
         * @param altGr    AltGr (right Alt) held
         * @return the character or 0 if the key does not produce text in this layout
         */
        public char charFor(int keyCode, int scanCode, boolean shift, boolean capsLock, boolean altGr) {
            if (keyCode == KeyEvent.KEYCODE_BACKSLASH && scanCode == SCANCODE_ISO_102ND) {
                keyCode = KEYCODE_ISO_102ND;
            }
            char[] entry = keys.get(keyCode);
            if (entry == null) {
                return 0;
            }
            if (altGr) {
                return entry[ALTGR];
            }
            boolean letter = Character.isLetter(entry[BASE]) && Character.isLetter(entry[SHIFT]);
            boolean upper = letter ? (shift ^ capsLock) : shift;
            return entry[upper ? SHIFT : BASE];
        }

        public boolean hasKey(int keyCode, int scanCode) {
            if (keyCode == KeyEvent.KEYCODE_BACKSLASH && scanCode == SCANCODE_ISO_102ND) {
                keyCode = KEYCODE_ISO_102ND;
            }
            return keys.containsKey(keyCode);
        }
    }

    private static final Map<String, Layout> LAYOUTS = new HashMap<>();

    static {
        LAYOUTS.put(ID_US, us());
        LAYOUTS.put(ID_TR_Q, turkishQ());
        LAYOUTS.put(ID_TR_F, turkishF());
        LAYOUTS.put(ID_DE, german());
        LAYOUTS.put(ID_FR, french());
        LAYOUTS.put(ID_ES, spanish());
        LAYOUTS.put(ID_IT, italian());
    }

    private KeyboardLayouts() {
    }

    public static Layout get(String id) {
        return id == null ? null : LAYOUTS.get(id);
    }

    public static Layout us() {
        if (LAYOUTS.containsKey(ID_US)) {
            return LAYOUTS.get(ID_US);
        }
        Layout l = new Layout(ID_US);
        l.letters("abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        digits(l, "!@#$%^&*()");
        l.key(KeyEvent.KEYCODE_GRAVE, '`', '~')
                .key(KeyEvent.KEYCODE_MINUS, '-', '_')
                .key(KeyEvent.KEYCODE_EQUALS, '=', '+')
                .key(KeyEvent.KEYCODE_LEFT_BRACKET, '[', '{')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, ']', '}')
                .key(KeyEvent.KEYCODE_BACKSLASH, '\\', '|')
                .key(KeyEvent.KEYCODE_SEMICOLON, ';', ':')
                .key(KeyEvent.KEYCODE_APOSTROPHE, '\'', '"')
                .key(KeyEvent.KEYCODE_COMMA, ',', '<')
                .key(KeyEvent.KEYCODE_PERIOD, '.', '>')
                .key(KeyEvent.KEYCODE_SLASH, '/', '?')
                .key(KEYCODE_ISO_102ND, '\\', '|')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** Turkish Q (the default Turkish layout). */
    private static Layout turkishQ() {
        Layout l = new Layout(ID_TR_Q);
        l.letters("abcdefghjklmnopqrstuvwxyz", "abcdefghjklmnopqrstuvwxyz", "ABCDEFGHJKLMNOPQRSTUVWXYZ");
        l.key(KeyEvent.KEYCODE_I, 'ı', 'I', 'i');           // ı / I
        digits(l, "!'^+%&/()=");
        l.altGr(KeyEvent.KEYCODE_2, '£').altGr(KeyEvent.KEYCODE_3, '#').altGr(KeyEvent.KEYCODE_4, '$')
                .altGr(KeyEvent.KEYCODE_5, '½').altGr(KeyEvent.KEYCODE_7, '{').altGr(KeyEvent.KEYCODE_8, '[')
                .altGr(KeyEvent.KEYCODE_9, ']').altGr(KeyEvent.KEYCODE_0, '}')
                .altGr(KeyEvent.KEYCODE_Q, '@').altGr(KeyEvent.KEYCODE_E, '€');
        l.key(KeyEvent.KEYCODE_GRAVE, '"', 'é', '<')
                .key(KeyEvent.KEYCODE_MINUS, '*', '?', '\\')
                .key(KeyEvent.KEYCODE_EQUALS, '-', '_', '|')
                .key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ğ', 'Ğ')  // ğ Ğ
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, 'ü', 'Ü', '~') // ü Ü
                .key(KeyEvent.KEYCODE_SEMICOLON, 'ş', 'Ş')     // ş Ş
                .key(KeyEvent.KEYCODE_APOSTROPHE, 'i', 'İ')         // i İ
                .key(KeyEvent.KEYCODE_BACKSLASH, ',', ';', '`')
                .key(KeyEvent.KEYCODE_COMMA, 'ö', 'Ö')         // ö Ö
                .key(KeyEvent.KEYCODE_PERIOD, 'ç', 'Ç')        // ç Ç
                .key(KeyEvent.KEYCODE_SLASH, '.', ':')
                .key(KEYCODE_ISO_102ND, '<', '>', '|')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** Turkish F. */
    private static Layout turkishF() {
        Layout l = new Layout(ID_TR_F);
        // Positions:            qwertyuiop / asdfghjkl / zxcvbnm
        l.letters("qwertyuiop", "fgğıodrnhp", "FGĞIODRNHP");
        l.letters("asdfghjkl", "uieaütkml", "UİEAÜTKML");
        l.letters("zxcvbnm", "jövcçzs", "JÖVCÇZS");
        digits(l, "!\"^$%&'()=");
        l.altGr(KeyEvent.KEYCODE_F, '@').altGr(KeyEvent.KEYCODE_E, '€');
        l.key(KeyEvent.KEYCODE_GRAVE, '+', '*')
                .key(KeyEvent.KEYCODE_MINUS, '/', '?')
                .key(KeyEvent.KEYCODE_EQUALS, '-', '_')
                .key(KeyEvent.KEYCODE_LEFT_BRACKET, 'q', 'Q')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, 'w', 'W')
                .key(KeyEvent.KEYCODE_SEMICOLON, 'y', 'Y')
                .key(KeyEvent.KEYCODE_APOSTROPHE, 'ş', 'Ş')
                .key(KeyEvent.KEYCODE_BACKSLASH, 'x', 'X')
                .key(KeyEvent.KEYCODE_COMMA, 'b', 'B')
                .key(KeyEvent.KEYCODE_PERIOD, '.', ':')
                .key(KeyEvent.KEYCODE_SLASH, ',', ';')
                .key(KEYCODE_ISO_102ND, '<', '>', '|')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** German QWERTZ. */
    private static Layout german() {
        Layout l = new Layout(ID_DE);
        l.letters("abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuvwxzy", "ABCDEFGHIJKLMNOPQRSTUVWXZY");
        digits(l, "!\"§$%&/()=");
        l.altGr(KeyEvent.KEYCODE_2, '²').altGr(KeyEvent.KEYCODE_3, '³').altGr(KeyEvent.KEYCODE_7, '{')
                .altGr(KeyEvent.KEYCODE_8, '[').altGr(KeyEvent.KEYCODE_9, ']').altGr(KeyEvent.KEYCODE_0, '}')
                .altGr(KeyEvent.KEYCODE_Q, '@').altGr(KeyEvent.KEYCODE_E, '€').altGr(KeyEvent.KEYCODE_M, 'µ');
        l.key(KeyEvent.KEYCODE_MINUS, 'ß', '?', '\\')
                .key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ü', 'Ü')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, '+', '*', '~')
                .key(KeyEvent.KEYCODE_SEMICOLON, 'ö', 'Ö')
                .key(KeyEvent.KEYCODE_APOSTROPHE, 'ä', 'Ä')
                .key(KeyEvent.KEYCODE_BACKSLASH, '#', '\'')
                .key(KeyEvent.KEYCODE_COMMA, ',', ';')
                .key(KeyEvent.KEYCODE_PERIOD, '.', ':')
                .key(KeyEvent.KEYCODE_SLASH, '-', '_')
                .key(KEYCODE_ISO_102ND, '<', '>', '|')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** French AZERTY. */
    private static Layout french() {
        Layout l = new Layout(ID_FR);
        // AZERTY letters on QWERTY positions (q<->a, w<->z, m moves to the semicolon key)
        l.letters("abcdefghijklnopqrstuvwxyz", "qbcdefghijklnoparstuvzxyw", "QBCDEFGHIJKLNOPARSTUVZXYW");
        l.key(KeyEvent.KEYCODE_M, ',', '?');
        l.key(KeyEvent.KEYCODE_1, '&', '1').key(KeyEvent.KEYCODE_2, 'é', '2', '~')
                .key(KeyEvent.KEYCODE_3, '"', '3', '#').key(KeyEvent.KEYCODE_4, '\'', '4', '{')
                .key(KeyEvent.KEYCODE_5, '(', '5', '[').key(KeyEvent.KEYCODE_6, '-', '6', '|')
                .key(KeyEvent.KEYCODE_7, 'è', '7', '`').key(KeyEvent.KEYCODE_8, '_', '8', '\\')
                .key(KeyEvent.KEYCODE_9, 'ç', '9', '^').key(KeyEvent.KEYCODE_0, 'à', '0', '@');
        l.altGr(KeyEvent.KEYCODE_E, '€');
        l.key(KeyEvent.KEYCODE_GRAVE, '²', '²')
                .key(KeyEvent.KEYCODE_MINUS, ')', '°', ']')
                .key(KeyEvent.KEYCODE_EQUALS, '=', '+', '}')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, '$', '£')
                .key(KeyEvent.KEYCODE_SEMICOLON, 'm', 'M')
                .key(KeyEvent.KEYCODE_APOSTROPHE, 'ù', '%')
                .key(KeyEvent.KEYCODE_BACKSLASH, '*', 'µ')
                .key(KeyEvent.KEYCODE_COMMA, ';', '.')
                .key(KeyEvent.KEYCODE_PERIOD, ':', '/')
                .key(KeyEvent.KEYCODE_SLASH, '!', '§')
                .key(KEYCODE_ISO_102ND, '<', '>')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** Spanish (Spain). */
    private static Layout spanish() {
        Layout l = new Layout(ID_ES);
        l.letters("abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        digits(l, "!\"·$%&/()=");
        l.altGr(KeyEvent.KEYCODE_1, '|').altGr(KeyEvent.KEYCODE_2, '@').altGr(KeyEvent.KEYCODE_3, '#')
                .altGr(KeyEvent.KEYCODE_6, '¬').altGr(KeyEvent.KEYCODE_E, '€');
        l.key(KeyEvent.KEYCODE_GRAVE, 'º', 'ª', '\\')
                .key(KeyEvent.KEYCODE_MINUS, '\'', '?')
                .key(KeyEvent.KEYCODE_EQUALS, '¡', '¿')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, '+', '*', ']')
                .key(KeyEvent.KEYCODE_SEMICOLON, 'ñ', 'Ñ')
                .key(KeyEvent.KEYCODE_BACKSLASH, 'ç', 'Ç', '}')
                .key(KeyEvent.KEYCODE_COMMA, ',', ';')
                .key(KeyEvent.KEYCODE_PERIOD, '.', ':')
                .key(KeyEvent.KEYCODE_SLASH, '-', '_')
                .key(KEYCODE_ISO_102ND, '<', '>')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    /** Italian. */
    private static Layout italian() {
        Layout l = new Layout(ID_IT);
        l.letters("abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        digits(l, "!\"£$%&/()=");
        l.altGr(KeyEvent.KEYCODE_E, '€');
        l.key(KeyEvent.KEYCODE_GRAVE, '\\', '|')
                .key(KeyEvent.KEYCODE_MINUS, '\'', '?')
                .key(KeyEvent.KEYCODE_EQUALS, 'ì', '^')
                .key(KeyEvent.KEYCODE_LEFT_BRACKET, 'è', 'é', '[')
                .key(KeyEvent.KEYCODE_RIGHT_BRACKET, '+', '*', ']')
                .key(KeyEvent.KEYCODE_SEMICOLON, 'ò', 'ç', '@')
                .key(KeyEvent.KEYCODE_APOSTROPHE, 'à', '°', '#')
                .key(KeyEvent.KEYCODE_BACKSLASH, 'ù', '§')
                .key(KeyEvent.KEYCODE_COMMA, ',', ';')
                .key(KeyEvent.KEYCODE_PERIOD, '.', ':')
                .key(KeyEvent.KEYCODE_SLASH, '-', '_')
                .key(KEYCODE_ISO_102ND, '<', '>')
                .key(KeyEvent.KEYCODE_SPACE, ' ', ' ');
        return l;
    }

    private static void digits(Layout l, String shifted) {
        for (int i = 0; i < 10; i++) {
            // KEYCODE_1..KEYCODE_9 then KEYCODE_0
            int keyCode = i < 9 ? KeyEvent.KEYCODE_1 + i : KeyEvent.KEYCODE_0;
            char digit = (char) (i < 9 ? '1' + i : '0');
            char[] existing = l.keys.get(keyCode);
            char altGr = existing != null ? existing[ALTGR] : 0;
            l.keys.put(keyCode, new char[]{digit, shifted.charAt(i), altGr});
        }
    }

    /**
     * Map a BCP-47 language tag (or Java locale) to the default physical layout used by that
     * language. Returns {@code null} when the language uses a US compatible layout or is unknown.
     */
    public static String layoutIdForLanguage(String languageTag) {
        if (languageTag == null || languageTag.isEmpty()) {
            return null;
        }
        String lang = Locale.forLanguageTag(languageTag.replace('_', '-')).getLanguage();
        if (lang == null || lang.isEmpty()) {
            lang = languageTag.toLowerCase(Locale.ROOT);
        }
        switch (lang) {
            case "tr":
                return ID_TR_Q;
            case "de":
                return ID_DE;
            case "fr":
                return ID_FR;
            case "es":
                return ID_ES;
            case "it":
                return ID_IT;
            case "en":
                return ID_US;
            default:
                return null;
        }
    }
}
